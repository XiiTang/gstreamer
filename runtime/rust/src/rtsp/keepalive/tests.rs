use super::*;
use crate::rtsp::{Credential, TrackState, Version};
use sha2::{Digest, Sha256};
use std::{
    io::{Read, Write},
    os::unix::net::UnixStream,
};
use zeroize::Zeroizing;

const URI: &str = "rtsp://localhost/media";
const BUDGET: Duration = Duration::from_secs(30);

fn request(peer: &mut UnixStream) -> String {
    let mut bytes = Vec::new();
    while !bytes.ends_with(b"\r\n\r\n") {
        let mut byte = [0];
        peer.read_exact(&mut byte).unwrap();
        bytes.push(byte[0]);
    }
    String::from_utf8(bytes).unwrap()
}
fn sequence(request: &str) -> &str {
    request
        .lines()
        .find_map(|line| line.strip_prefix("CSeq: "))
        .unwrap()
}
fn reply(peer: &mut UnixStream, wire: &str, sequence: &str, extra: &str) {
    peer.write_all(
        format!("RTSP/{wire} 200 OK\r\nCSeq: {sequence}\r\nSession: s\r\n{extra}\r\n").as_bytes(),
    )
    .unwrap();
}
fn credential(name: &str) -> Credential {
    Credential::Digest {
        username: Zeroizing::new(name.into()),
        password: Zeroizing::new(format!("{name}-password")),
    }
}
struct Fixture {
    native: Rtsp,
    peer: UnixStream,
    clock: Arc<std::sync::Mutex<Instant>>,
    calls: Arc<std::sync::atomic::AtomicUsize>,
}
fn connected(version: Version, wire: &str) -> Fixture {
    let (client, mut peer) = UnixStream::pair().unwrap();
    peer.set_read_timeout(Some(BUDGET)).unwrap();
    peer.set_write_timeout(Some(BUDGET)).unwrap();
    let mut native = Rtsp::from_stream(client.into(), URI, version, 4096).unwrap();
    native
        .request(
            "SETUP",
            URI,
            &[("Transport", "RTP/AVP/TCP;unicast;interleaved=0-1")],
            &[],
            BUDGET,
        )
        .0
        .unwrap();
    let sent = request(&mut peer);
    reply(
        &mut peer,
        wire,
        sequence(&sent),
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=\"PLAY\"\r\n",
    );
    native.receive(BUDGET).0.unwrap();
    let clock = Arc::new(std::sync::Mutex::new(Instant::now()));
    native.keepalives.clock = Some(clock.clone());
    let calls = Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let observed = calls.clone();
    native
        .configure_authentication_callback(
            Box::new(move || {
                observed.fetch_add(1, Ordering::AcqRel);
                Ok(credential("ordinary"))
            }),
            false,
            false,
        )
        .unwrap();
    Fixture {
        native,
        peer,
        clock,
        calls,
    }
}
fn configuration() -> Keepalive {
    Keepalive {
        uri: URI.into(),
        method: KeepaliveMethod::GetParameter,
        interval: Duration::from_secs(1),
        response_timeout: Duration::from_secs(5),
    }
}
fn finish_write(native: &mut Rtsp, first: Option<KeepaliveEvent>) {
    if first.is_some_and(|event| matches!(event.kind, KeepaliveEventKind::Sent)) {
        return;
    }
    while native.maintenance_writing() {
        native.wait(BUDGET).unwrap();
        if let Some(event) = native
            .maintain(|_| panic!("an admitted exchange keeps its material"))
            .unwrap()
        {
            assert!(matches!(event.kind, KeepaliveEventKind::Sent));
        }
    }
}
fn check_digest(sent: &str, username: &str) {
    let header = sent
        .lines()
        .find_map(|line| line.strip_prefix("Authorization: Digest "))
        .unwrap();
    let fields: BTreeMap<_, _> = header
        .split(',')
        .map(|field| {
            let (key, value) = field.trim().split_once('=').unwrap();
            (key, value.trim_matches('"'))
        })
        .collect();
    assert_eq!(fields["username"], username);
    let hash = |value: &str| format!("{:x}", Sha256::digest(value.as_bytes()));
    let a1 = hash(&format!("{username}:camera:{username}-password"));
    let a2 = hash(&format!("GET_PARAMETER:{URI}"));
    let expected = hash(&format!(
        "{a1}:{}:{}:{}:auth:{a2}",
        fields["nonce"], fields["nc"], fields["cnonce"]
    ));
    assert_eq!(fields["response"], expected);
}

#[test]
fn pending_supply_keeps_reads_live_and_one_generation_through_challenge() {
    for (version, wire) in [(Version::V1, "1.0"), (Version::V2, "2.0")] {
        let Fixture {
            mut native,
            mut peer,
            clock,
            calls,
        } = connected(version, wire);
        native
            .configure_keepalive("s", Some(configuration()))
            .unwrap();
        *clock.lock().unwrap() += Duration::from_secs(1);
        let mut selected = None;
        for _ in 0..3 {
            let event = native
                .maintain(|due| {
                    if let Some(previous) = &selected {
                        assert_eq!(due, previous);
                    }
                    selected = Some(due.clone());
                    None
                })
                .unwrap();
            assert_eq!(
                calls.load(Ordering::Acquire),
                0,
                "pending maintenance cannot call the synchronous source"
            );
            assert!(event.is_none());
            assert!(native.maintenance_pending());
            assert!(!native.maintenance_writing());
            let status = native.keepalive_status("s").unwrap();
            assert_eq!(status.pending_sequence, None);
            assert!(!status.writing);
        }
        assert_eq!(calls.load(Ordering::Acquire), 0);
        let due = selected.unwrap();
        peer.write_all(&[b'$', 0, 0, 3, 1, 2, 3]).unwrap();
        let (result, message) = native.receive(BUDGET);
        result.unwrap();
        assert_eq!(message.kind, 5);
        peer.write_all(format!("OPTIONS {URI} RTSP/{wire}\r\nCSeq: 77\r\n\r\n").as_bytes())
            .unwrap();
        let (result, message) = native.receive(BUDGET);
        result.unwrap();
        assert_eq!(message.kind, 1);
        *clock.lock().unwrap() = due.deadline - Duration::from_millis(1);
        let mut supplies = 0;
        let first = native
            .maintain(|selected| {
                assert_eq!(selected, &due);
                supplies += 1;
                Some(Ok(credential("generation-one")))
            })
            .unwrap();
        finish_write(&mut native, first);
        let sent = request(&mut peer);
        assert!(!sent.contains("Authorization"));
        peer.write_all(format!("RTSP/{wire} 401 Unauthorized\r\nCSeq: {}\r\nWWW-Authenticate: Digest realm=\"camera\",nonce=\"one\",algorithm=SHA-256,qop=\"auth\"\r\n\r\n", sequence(&sent)).as_bytes()).unwrap();
        let (result, message) = native.receive(BUDGET);
        result.unwrap();
        assert_eq!(message.status, 401);
        while native.maintenance_writing() {
            native.wait(BUDGET).unwrap();
            native
                .maintain(|_| panic!("a challenge keeps its generation"))
                .unwrap();
        }
        let answered = request(&mut peer);
        check_digest(&answered, "generation-one");
        reply(&mut peer, wire, sequence(&answered), "");
        native.receive(BUDGET).0.unwrap();
        let event = native
            .maintain(|_| panic!("response event is drained first"))
            .unwrap()
            .unwrap();
        assert!(matches!(
            event.kind,
            KeepaliveEventKind::Response { status: 200 }
        ));
        assert_eq!(supplies, 1);
        assert_eq!(calls.load(Ordering::Acquire), 0);
        *clock.lock().unwrap() += Duration::from_secs(1);
        let first = native
            .maintain(|next| {
                assert_ne!(next, &due);
                assert!(!next.is_cancelled());
                Some(Ok(credential("generation-two")))
            })
            .unwrap();
        finish_write(&mut native, first);
        let sent = request(&mut peer);
        check_digest(&sent, "generation-two");
        reply(&mut peer, wire, sequence(&sent), "");
        native.receive(BUDGET).0.unwrap();
    }
}

#[test]
fn selected_supply_obeys_cancellation_replacement_and_original_deadline() {
    for (version, wire) in [(Version::V1, "1.0"), (Version::V2, "2.0")] {
        for mode in [
            "cancel",
            "cancel_error",
            "disable",
            "expiry_wins",
            "replace",
            "pre_timeout",
            "ready_error",
            "admitted_timeout",
        ] {
            let Fixture {
                mut native,
                mut peer,
                clock,
                calls,
            } = connected(version, wire);
            let cancel = native
                .configure_keepalive("s", Some(configuration()))
                .unwrap()
                .unwrap();
            *clock.lock().unwrap() += Duration::from_secs(1);
            let mut due = None;
            native
                .maintain(|selected| {
                    due = Some(selected.clone());
                    None
                })
                .unwrap();
            let due = due.unwrap();
            if mode == "replace" {
                native
                    .configure_keepalive("s", Some(configuration()))
                    .unwrap();
                assert!(due.is_cancelled());
                *clock.lock().unwrap() += Duration::from_secs(1);
                let first = native
                    .maintain(|next| {
                        assert_ne!(next, &due);
                        Some(Ok(credential("replacement")))
                    })
                    .unwrap();
                finish_write(&mut native, first);
                let sent = request(&mut peer);
                assert!(sent.starts_with("GET_PARAMETER "));
                reply(&mut peer, wire, sequence(&sent), "");
                native.receive(BUDGET).0.unwrap();
                continue;
            }
            if mode == "disable" {
                native.configure_keepalive("s", None).unwrap();
            }
            if mode == "pre_timeout" {
                *clock.lock().unwrap() = due.deadline;
            } else {
                *clock.lock().unwrap() = due.deadline - Duration::from_millis(1);
            }
            let event = native
                .maintain(|selected| {
                    assert_eq!(selected, &due);
                    if mode == "cancel" || mode == "cancel_error" {
                        cancel.cancel();
                    }
                    if mode == "expiry_wins" {
                        *clock.lock().unwrap() = due.deadline;
                    }
                    Some(
                        if matches!(mode, "ready_error" | "expiry_wins" | "cancel_error") {
                            Err(Error::AUTHENTICATION)
                        } else {
                            Ok(credential("ready"))
                        },
                    )
                })
                .unwrap();
            if mode == "admitted_timeout" {
                finish_write(&mut native, event);
                let sent = request(&mut peer);
                cancel.cancel();
                *clock.lock().unwrap() = due.deadline;
                let failure = native.maintain(|_| panic!("already admitted")).unwrap_err();
                assert_eq!(failure.error, Error::TIMEOUT);
                assert_eq!(failure.dispatch.sequence.to_string(), sequence(&sent));
                assert!(failure.dispatch.may_have_been_sent);
                assert_eq!(native.state("s", URI).unwrap(), Some(TrackState::Ready));
                continue;
            }
            if matches!(mode, "cancel" | "cancel_error" | "disable") {
                assert!(event.is_none());
            } else {
                let event = event.unwrap();
                assert_eq!(event.sequence, None);
                assert!(matches!(event.kind, KeepaliveEventKind::Rejected { error }
                    if error == if matches!(mode, "pre_timeout" | "expiry_wins") { Error::TIMEOUT } else { Error::AUTHENTICATION }));
            }
            assert!(due.is_cancelled());
            assert_eq!(native.state("s", URI).unwrap(), Some(TrackState::Ready));
            // A later real request, on the same native connection, is the
            // barrier proving no stale maintenance bytes or CSeq escaped.
            assert_eq!(calls.load(Ordering::Acquire), 0);
            native.request("OPTIONS", URI, &[], &[], BUDGET).0.unwrap();
            assert_eq!(calls.load(Ordering::Acquire), 1);
            let sent = request(&mut peer);
            assert!(sent.starts_with("OPTIONS "));
            assert_eq!(sequence(&sent), "2");
            reply(&mut peer, wire, sequence(&sent), "");
            native.receive(BUDGET).0.unwrap();
        }
    }
}
