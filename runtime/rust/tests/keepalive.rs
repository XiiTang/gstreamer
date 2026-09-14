#![cfg(unix)]
use imapipe_media::{
    Error,
    rtsp::{Keepalive, KeepaliveEventKind, KeepaliveFailure, KeepaliveMethod, Rtsp, Version},
};
use std::{
    io::{Read, Write},
    os::unix::net::UnixStream,
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicI32, Ordering},
        mpsc,
    },
    thread,
    time::{Duration, Instant},
};
const URI: &str = "rtsp://localhost/media";
struct Peer {
    stop: Arc<AtomicBool>,
    status: Arc<AtomicI32>,
    requests: mpsc::Receiver<String>,
    thread: Option<thread::JoinHandle<()>>,
}
impl Drop for Peer {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        self.thread.take().unwrap().join().unwrap();
    }
}
fn receive(native: &mut Rtsp) {
    let end = Instant::now() + Duration::from_secs(2);
    loop {
        let (result, message) = native.receive_step();
        if result.unwrap() {
            assert_eq!(message.kind, 2);
            return;
        }
        assert!(Instant::now() < end);
        thread::sleep(Duration::from_millis(1));
    }
}
fn connected(version: Version) -> (Rtsp, Peer) {
    let (client, mut server) = UnixStream::pair().unwrap();
    server
        .set_read_timeout(Some(Duration::from_millis(5)))
        .unwrap();
    server
        .set_write_timeout(Some(Duration::from_secs(1)))
        .unwrap();
    let stop = Arc::new(AtomicBool::new(false));
    let thread_stop = stop.clone();
    let status = Arc::new(AtomicI32::new(200));
    let thread_status = status.clone();
    let (sent, requests) = mpsc::channel();
    let worker = thread::spawn(move || {
        let mut request = Vec::new();
        while !thread_stop.load(Ordering::Acquire) {
            let mut byte = [0];
            match server.read(&mut byte) {
                Ok(0) => break,
                Ok(_) => request.push(byte[0]),
                Err(e)
                    if matches!(
                        e.kind(),
                        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                    ) =>
                {
                    continue;
                }
                Err(_) => break,
            }
            if !request.ends_with(b"\r\n\r\n") {
                continue;
            }
            let text = String::from_utf8(std::mem::take(&mut request)).unwrap();
            let sequence = text.lines().find_map(|l| l.strip_prefix("CSeq: ")).unwrap();
            let code = thread_status.load(Ordering::Acquire);
            let setup = text.starts_with("SETUP ");
            sent.send(text.clone()).unwrap();
            if code == 0 {
                continue;
            }
            let transport = if setup {
                "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=\"PLAY\"\r\n"
            } else {
                ""
            };
            let version = match version {
                Version::V1 => "1.0",
                Version::V2 => "2.0",
            };
            if server.write_all(format!("RTSP/{version} {code} Fixture\r\nCSeq: {sequence}\r\nSession: s;timeout=60\r\n{transport}\r\n").as_bytes()).is_err() {break;}
        }
    });
    let mut native = Rtsp::from_stream(client.into(), URI, version, 4096).unwrap();
    native
        .request(
            "SETUP",
            URI,
            &[("Transport", "RTP/AVP/TCP;unicast;interleaved=0-1")],
            &[],
            Duration::from_secs(1),
        )
        .0
        .unwrap();
    receive(&mut native);
    assert!(
        requests
            .recv_timeout(Duration::from_secs(1))
            .unwrap()
            .starts_with("SETUP ")
    );
    (
        native,
        Peer {
            stop,
            status,
            requests,
            thread: Some(worker),
        },
    )
}
fn configuration(method: KeepaliveMethod) -> Keepalive {
    Keepalive {
        uri: URI.into(),
        method,
        interval: Duration::from_millis(10),
        response_timeout: Duration::from_millis(150),
        authentication: None,
    }
}
fn pump(
    native: &mut Rtsp,
    duration: Duration,
) -> Result<Vec<KeepaliveEventKind>, KeepaliveFailure> {
    let until = Instant::now() + duration;
    let mut events = Vec::new();
    while Instant::now() < until {
        if let Some(event) = native.keepalive_step()? {
            events.push(event.kind);
        }
        native.receive_step().0.unwrap();
        thread::sleep(Duration::from_millis(1));
    }
    Ok(events)
}
#[test]
fn both_versions_keepalive_is_explicit_cancellable_and_uses_the_same_session() {
    for version in [Version::V1, Version::V2] {
        for method in [KeepaliveMethod::Options, KeepaliveMethod::GetParameter] {
            let (mut native, peer) = connected(version);
            pump(&mut native, Duration::from_millis(25)).unwrap();
            assert!(peer.requests.try_recv().is_err());
            assert!(
                native
                    .configure_keepalive("missing", Some(configuration(method)))
                    .is_err()
            );
            let cancel = native
                .configure_keepalive("s", Some(configuration(method)))
                .unwrap()
                .unwrap();
            let until = Instant::now() + Duration::from_secs(1);
            let mut sent = false;
            loop {
                if let Some(event) = native.keepalive_step().unwrap() {
                    assert_eq!(event.session, "s");
                    match event.kind {
                        KeepaliveEventKind::Sent => sent = true,
                        KeepaliveEventKind::Response { status } => {
                            assert_eq!(status, 200);
                            break;
                        }
                        _ => panic!("{event:?}"),
                    }
                }
                native.receive_step().0.unwrap();
                assert!(Instant::now() < until);
                thread::sleep(Duration::from_millis(1));
            }
            assert!(sent);
            let request = peer.requests.try_recv().unwrap();
            assert!(request.starts_with(match method {
                KeepaliveMethod::Options => "OPTIONS ",
                KeepaliveMethod::GetParameter => "GET_PARAMETER ",
            }));
            assert!(request.contains("Session: s\r\n"));
            cancel.cancel();
            pump(&mut native, Duration::from_millis(35)).unwrap();
            assert!(peer.requests.try_recv().is_err());
            assert!(!native.keepalive_pending());
            assert!(native.keepalive_status("s").is_none());
            drop(native);
            drop(peer);
        }
    }
}
#[test]
fn negative_keepalive_stops_its_cycle_without_replaying_or_closing_the_connection() {
    let (mut native, peer) = connected(Version::V2);
    peer.status.store(455, Ordering::Release);
    native
        .configure_keepalive("s", Some(configuration(KeepaliveMethod::GetParameter)))
        .unwrap();
    let events = pump(&mut native, Duration::from_millis(65)).unwrap();
    assert!(
        events
            .iter()
            .any(|e| matches!(e, KeepaliveEventKind::Response { status: 455 }))
    );
    assert_eq!(peer.requests.try_iter().count(), 1);
    peer.status.store(200, Ordering::Release);
    native
        .request(
            "OPTIONS",
            URI,
            &[("Session", "s")],
            &[],
            Duration::from_secs(1),
        )
        .0
        .unwrap();
    receive(&mut native);
    assert!(peer.requests.try_recv().unwrap().starts_with("OPTIONS "));
}
#[test]
fn cancelling_an_inflight_cycle_retains_its_response_deadline_and_unknown_outcome() {
    let (mut native, peer) = connected(Version::V1);
    peer.status.store(0, Ordering::Release);
    let cancel = native
        .configure_keepalive(
            "s",
            Some(Keepalive {
                response_timeout: Duration::from_millis(30),
                ..configuration(KeepaliveMethod::Options)
            }),
        )
        .unwrap()
        .unwrap();
    let until = Instant::now() + Duration::from_secs(1);
    loop {
        match native.keepalive_step() {
            Ok(Some(event)) if matches!(event.kind, KeepaliveEventKind::Sent) => cancel.cancel(),
            Ok(_) => {
                native.receive_step().0.unwrap();
            }
            Err(failure) => {
                assert_eq!(failure.error, Error::TIMEOUT);
                assert!(failure.dispatch.may_have_been_sent);
                assert_eq!(failure.dispatch.sequence, 2);
                assert_eq!(
                    native.state("s", URI).unwrap(),
                    Some(imapipe_media::rtsp::TrackState::Unknown)
                );
                break;
            }
        }
        assert!(Instant::now() < until);
        thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(peer.requests.try_iter().count(), 1);
}
#[test]
fn maintenance_waits_for_explicit_control_and_stops_after_teardown() {
    let (mut native, peer) = connected(Version::V1);
    let config = configuration(KeepaliveMethod::Options);
    for bad in [
        Keepalive {
            interval: Duration::ZERO,
            ..config.clone()
        },
        Keepalive {
            response_timeout: Duration::ZERO,
            ..config.clone()
        },
        Keepalive {
            interval: Duration::MAX,
            ..config.clone()
        },
    ] {
        assert!(native.configure_keepalive("s", Some(bad)).is_err());
    }
    native.configure_keepalive("s", Some(config)).unwrap();
    native
        .request(
            "TEARDOWN",
            URI,
            &[("Session", "s")],
            &[],
            Duration::from_secs(1),
        )
        .0
        .unwrap();
    // The native request matcher is occupied; maintenance cannot inject a request.
    thread::sleep(Duration::from_millis(15));
    assert!(native.keepalive_step().unwrap().is_none());
    receive(&mut native);
    let event = native.keepalive_step().unwrap().unwrap();
    assert!(matches!(event.kind, KeepaliveEventKind::SessionEnded));
    assert_eq!(peer.requests.try_iter().count(), 1);
}
