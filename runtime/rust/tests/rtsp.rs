#![cfg(unix)]
use imapipe_media::{
    Error,
    rtsp::{Rtsp, Version},
};
use std::{
    io::{Read, Write},
    os::unix::net::UnixStream,
    time::{Duration, Instant},
};
const SECOND: Duration = Duration::from_secs(1);
const URI: &str = "rtsps://never-resolve.invalid/media";
fn request(server: &mut UnixStream) -> Vec<u8> {
    let mut bytes = Vec::new();
    while !bytes.ends_with(b"\r\n\r\n") {
        let mut byte = [0];
        server.read_exact(&mut byte).unwrap();
        bytes.push(byte[0]);
    }
    bytes
}
#[test]
fn opaque_owner_preserves_extensions_binary_and_negative_response() {
    for (version, wire) in [(Version::V1, "1.0"), (Version::V2, "2.0")] {
        let (stream, mut server) = UnixStream::pair().unwrap();
        let mut client = Rtsp::from_stream(stream.into(), URI, version, 4096).unwrap();
        assert!(!client.wait(Duration::from_millis(1)).unwrap());
        let (result, dispatch) = client.request("OPTIONS", URI, &[], &[], SECOND);
        result.unwrap();
        assert_eq!(dispatch.sequence, 1);
        assert!(dispatch.may_have_been_sent);
        let sent = request(&mut server);
        assert!(sent.starts_with(format!("OPTIONS {URI} RTSP/{wire}\r\n").as_bytes()));
        let mut response = format!("RTSP/{wire} 461 Unsupported Transport\r\nCSeq: 1\r\nX-Repeated: first\r\nX-Repeated: second\r\nContent-Length: 3\r\n\r\n").into_bytes();
        response.extend_from_slice(&[0, 255, 1]);
        let frame = [b'$', 3, 0, 3, 1, 0, 255];
        server
            .write_all(&[response.clone(), frame.to_vec()].concat())
            .unwrap();
        assert!(client.wait(SECOND).unwrap());
        let (result, message) = client.receive(SECOND);
        result.unwrap();
        assert_eq!(message.status, 461);
        assert_eq!(message.body, [0, 255, 1]);
        assert_eq!(message.raw, response);
        let repeated: Vec<_> = message
            .headers
            .iter()
            .filter(|(k, _)| k.eq_ignore_ascii_case(b"X-Repeated"))
            .map(|(_, v)| v.clone())
            .collect();
        assert_eq!(repeated, [b"first".to_vec(), b"second".to_vec()]);
        assert!(client.wait(SECOND).unwrap());
        let (result, message) = client.receive(SECOND);
        result.unwrap();
        assert_eq!(message.kind, 5);
        assert_eq!(message.channel, 3);
        assert_eq!(message.raw, frame);
        let (result, dispatch) = client.request(
            "OPTIONS",
            URI,
            &[("X-Test", "a\r\nInjected: yes")],
            &[],
            SECOND,
        );
        assert_eq!(result, Err(Error::INVALID));
        assert_eq!(dispatch.sequence, 0);
        assert!(!dispatch.may_have_been_sent);
        drop(client);
        assert_eq!(
            server.read(&mut [0]).unwrap(),
            0,
            "Dropping an owner must not send TEARDOWN"
        );
    }
}
#[test]
fn cancellation_interrupts_partial_response_and_preserves_raw_evidence() {
    let (stream, mut server) = UnixStream::pair().unwrap();
    let mut client = Rtsp::from_stream(stream.into(), URI, Version::V2, 4096).unwrap();
    client.request("OPTIONS", URI, &[], &[], SECOND).0.unwrap();
    request(&mut server);
    let partial = b"RTSP/2.0 200 OK\r\nCSeq: 1\r\nContent-Length: 50\r\n\r\npartial";
    server.write_all(partial).unwrap();
    let cancellation = client.cancellation();
    let task = std::thread::spawn(move || client.receive(Duration::from_secs(60)));
    std::thread::sleep(Duration::from_millis(20));
    let start = Instant::now();
    cancellation.cancel();
    let (result, message) = task.join().unwrap();
    assert_eq!(result, Err(Error::CANCELLED));
    assert_eq!(message.raw, partial);
    assert!(start.elapsed() < Duration::from_millis(200));
    drop(cancellation);
    assert_eq!(server.read(&mut [0]).unwrap(), 0);
}

#[test]
fn versioned_transport_view_is_owned_after_next_setup_and_close() {
    for (version, wire, header) in [
        (
            Version::V1,
            "1.0",
            "RTP/AVP/TCP;unicast;interleaved=4-5;ssrc=01020304",
        ),
        (
            Version::V2,
            "2.0",
            "RTP/AVP/UDP;unicast;dest_addr=\":8000\"/\":8001\";src_addr=\"[2001:db8::1]:9000\"/\"media.example:9001\";ssrc=01020304/00000000",
        ),
    ] {
        let (stream, mut server) = UnixStream::pair().unwrap();
        let mut client = Rtsp::from_stream(stream.into(), URI, version, 4096).unwrap();
        assert!(client.transport("s", URI).unwrap().is_none());
        client.request("SETUP", URI, &[], &[], SECOND).0.unwrap();
        request(&mut server);
        server
            .write_all(
                format!(
                    "RTSP/{wire} 200 OK\r\nCSeq: 1\r\nSession: s\r\nTransport: {header}\r\n\r\n"
                )
                .as_bytes(),
            )
            .unwrap();
        client.receive(SECOND).0.unwrap();
        let view = client.transport("s", URI).unwrap().unwrap();
        assert_eq!(view.profile, "AVP");
        assert_eq!(view.generation, 1);
        assert_eq!(view.ssrcs[0], 0x01020304);
        match version {
            Version::V1 => assert_eq!(view.interleaved, Some((4, Some(5)))),
            Version::V2 => {
                assert_eq!(view.source_addresses[0].host, "2001:db8::1");
                assert_eq!(view.source_addresses[1].port, 9001);
                assert_eq!(view.destination_addresses[0].host, "");
                assert_eq!(view.ssrcs, [0x01020304, 0]);
            }
        }
        client
            .request("SETUP", URI, &[("Session", "s")], &[], SECOND)
            .0
            .unwrap();
        request(&mut server);
        server.write_all(format!("RTSP/{wire} 200 OK\r\nCSeq: 2\r\nSession: s\r\nTransport: RTP/AVP/TCP;unicast;interleaved=8-9\r\n\r\n").as_bytes()).unwrap();
        client.receive(SECOND).0.unwrap();
        assert_eq!(
            client.transport("s", URI).unwrap().unwrap().interleaved,
            Some((8, Some(9)))
        );
        assert_eq!(client.transport("s", URI).unwrap().unwrap().generation, 2);
        drop(client);
        assert_eq!(view.ssrcs[0], 0x01020304);
        assert_eq!(server.read(&mut [0]).unwrap(), 0);
    }
}

#[test]
fn overlapping_interleaved_channels_cannot_bind_a_sibling_track() {
    let (stream, mut server) = UnixStream::pair().unwrap();
    let mut client = Rtsp::from_stream(stream.into(), URI, Version::V2, 4096).unwrap();
    for (sequence, uri) in [(1, URI.to_owned()), (2, format!("{URI}/other"))] {
        let headers = if sequence == 1 {
            vec![]
        } else {
            vec![("Session", "s")]
        };
        client
            .request("SETUP", &uri, &headers, &[], SECOND)
            .0
            .unwrap();
        request(&mut server);
        server.write_all(format!("RTSP/2.0 200 OK\r\nCSeq: {sequence}\r\nSession: s\r\nTransport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n\r\n").as_bytes()).unwrap();
        let result = client.receive(SECOND).0;
        if sequence == 1 {
            result.unwrap();
        } else {
            assert!(result.is_err());
            assert!(client.transport("s", &uri).unwrap().is_none());
            assert!(client.request("OPTIONS", URI, &[], &[], SECOND).0.is_err());
        }
    }
}

#[test]
fn native_incremental_reader_preserves_every_fragment_and_cancel_evidence() {
    for wire in [
        b"RTSP/2.0 200 OK\r\nCSeq: 1\r\nX-Test: held\r\nContent-Length: 3\r\n\r\n\0\xff\x01"
            .as_slice(),
        b"$\x03\0\x03\0\xff\x01".as_slice(),
    ] {
        let (stream, mut server) = UnixStream::pair().unwrap();
        let mut client = Rtsp::from_stream(stream.into(), URI, Version::V2, 4096).unwrap();
        client.request("OPTIONS", URI, &[], &[], SECOND).0.unwrap();
        request(&mut server);
        for (index, byte) in wire.iter().enumerate() {
            server.write_all(&[*byte]).unwrap();
            let start = Instant::now();
            let (result, message) = client.receive_step();
            assert!(start.elapsed() < Duration::from_millis(100));
            if index + 1 == wire.len() {
                assert!(result.unwrap());
                assert_eq!(message.raw, wire);
                assert_eq!(message.body, [0, 255, 1]);
            } else {
                assert!(!result.unwrap());
                assert!(message.raw.is_empty());
                assert!(client.state("s", URI).unwrap().is_none());
            }
        }
    }
    let (stream, mut server) = UnixStream::pair().unwrap();
    let mut client = Rtsp::from_stream(stream.into(), URI, Version::V2, 4096).unwrap();
    client.request("OPTIONS", URI, &[], &[], SECOND).0.unwrap();
    request(&mut server);
    let partial = b"RTSP/2.0 200 OK\r\nCSeq: 1\r\nContent-Length: 99\r\n\r\npartial";
    server.write_all(partial).unwrap();
    assert!(!client.receive_step().0.unwrap());
    client.cancellation().cancel();
    let (result, message) = client.receive_step();
    assert_eq!(result, Err(Error::CANCELLED));
    assert_eq!(message.raw, partial);
}

#[test]
fn incremental_owned_request_and_declared_data_progress_while_read_is_partial() {
    let (stream, mut server) = UnixStream::pair().unwrap();
    let mut client = Rtsp::from_stream(stream.into(), URI, Version::V2, 4096).unwrap();
    assert!(client.send_data_begin(4, b"no setup").is_err());
    let (result, dispatch) = client.request_begin("SETUP", URI, &[], &[]);
    assert!(result.unwrap());
    assert!(!dispatch.may_have_been_sent);
    assert!(!client.request_begin("OPTIONS", URI, &[], &[]).0.unwrap());
    let (result, dispatch) = client.write_step();
    assert!(result.unwrap());
    assert!(dispatch.may_have_been_sent);
    request(&mut server);
    server.write_all(b"RTSP/2.0 200 OK\r\nCSeq: 1\r\nSession: s\r\nTransport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n\r\n").unwrap();
    assert!(client.receive_step().0.unwrap());
    assert!(
        client
            .request_begin("GET_PARAMETER", URI, &[("Session", "s")], &[])
            .0
            .unwrap()
    );
    assert!(client.write_step().0.unwrap());
    request(&mut server);
    server
        .write_all(b"RTSP/2.0 200 OK\r\nCSeq: 2\r\nContent-Length: 3\r\n\r\na")
        .unwrap();
    assert!(!client.receive_step().0.unwrap());
    assert!(client.send_data_begin(5, &[0, 255, 1]).unwrap());
    assert!(client.write_step().0.unwrap());
    let mut wire = [0; 7];
    server.read_exact(&mut wire).unwrap();
    assert_eq!(wire, [b'$', 5, 0, 3, 0, 255, 1]);
    server.write_all(b"bc").unwrap();
    let (result, message) = client.receive_step();
    assert!(result.unwrap());
    assert_eq!(message.body, b"abc");
    assert!(client.send_data_begin(6, b"wrong channel").is_err());
}

fn auth_policy() -> imapipe_media::rtsp_auth::Policy {
    use imapipe_media::rtsp_auth::{Algorithm, Policy, Qop};
    Policy {
        basic: true,
        algorithms: vec![Algorithm::Sha256],
        qops: vec![Qop::Auth, Qop::AuthInt],
        realm: Some("camera".into()),
        require_server_proof: true,
    }
}
#[test]
fn explicit_digest_dispatch_redacts_challenges_and_fails_closed_on_bad_proof() {
    use imapipe_media::rtsp_auth::Qop;
    let (stream, mut server) = UnixStream::pair().unwrap();
    server.set_read_timeout(Some(SECOND)).unwrap();
    let mut client = Rtsp::from_stream(stream.into(), URI, Version::V2, 4096).unwrap();
    client
        .configure_authentication(
            "user".to_owned().into(),
            "private-password".to_owned().into(),
            auth_policy(),
            true,
        )
        .unwrap();
    client.request("OPTIONS", URI, &[], &[], SECOND).0.unwrap();
    request(&mut server);
    server.write_all(b"RTSP/2.0 401 Unauthorized\r\nCSeq: 1\r\nWWW-Authenticate: Digest realm=\"camera\",nonce=\"private-nonce\",algorithm=SHA-256,qop=\"auth,auth-int\"\r\n\r\n").unwrap();
    let (result, message) = client.receive(SECOND);
    result.unwrap();
    assert_eq!(message.status, 401);
    assert!(message.raw.is_empty());
    assert!(
        message
            .headers
            .iter()
            .filter(|(n, _)| n.eq_ignore_ascii_case(b"www-authenticate"))
            .all(|(_, v)| v.is_empty())
    );
    let auth = message.authentication.unwrap();
    assert!(auth.protected_headers);
    assert_eq!(auth.challenges.len(), 1);
    assert!(!format!("{auth:?}").contains("private-nonce"));
    let (result, dispatch) =
        client.request_authenticated_begin("SETUP", URI, &[], &[], "absent", Qop::Auth);
    assert_eq!(result, Err(Error::AUTHENTICATION));
    assert_eq!(dispatch.sequence, 0);
    assert!(!dispatch.may_have_been_sent);
    server.set_nonblocking(true).unwrap();
    assert_eq!(
        server.read(&mut [0]).unwrap_err().kind(),
        std::io::ErrorKind::WouldBlock
    );
    server.set_nonblocking(false).unwrap();
    let (result, dispatch) = client.request_authenticated_begin(
        "SETUP",
        URI,
        &[],
        &[],
        &auth.challenges[0].id,
        Qop::AuthInt,
    );
    assert_eq!(result, Ok(true));
    assert_eq!(dispatch.sequence, 2);
    loop {
        if client.write_step().0.unwrap() {
            break;
        }
    }
    let sent = String::from_utf8(request(&mut server)).unwrap();
    assert!(sent.contains("Authorization: Digest "));
    assert!(sent.contains(&format!("uri=\"{URI}\"")));
    assert!(sent.contains("qop=auth-int"));
    assert!(!sent.contains("private-password"));
    server.write_all(b"RTSP/2.0 200 OK\r\nCSeq: 2\r\nSession: s\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nAuthentication-Info: rspauth=\"wrong\"\r\n\r\n").unwrap();
    let (result, message) = client.receive(SECOND);
    assert_eq!(result, Err(Error::AUTHENTICATION));
    assert!(message.raw.is_empty());
    assert_eq!(message.authentication.unwrap().server_proof, Some(false));
    assert_eq!(
        client.state("s", URI).unwrap(),
        Some(imapipe_media::rtsp::TrackState::Unknown)
    );
}
#[test]
fn rtsp2_basic_is_only_offered_over_verified_tls_and_never_auto_sent() {
    for tls in [false, true] {
        let (stream, mut server) = UnixStream::pair().unwrap();
        let mut client = Rtsp::from_stream(stream.into(), URI, Version::V2, 4096).unwrap();
        client
            .configure_authentication(
                "user".to_owned().into(),
                "secret".to_owned().into(),
                auth_policy(),
                tls,
            )
            .unwrap();
        client.request("OPTIONS", URI, &[], &[], SECOND).0.unwrap();
        request(&mut server);
        server.write_all(b"RTSP/2.0 401 Unauthorized\r\nCSeq: 1\r\nWWW-Authenticate: Basic realm=\"camera\"\r\n\r\n").unwrap();
        let (result, message) = client.receive(SECOND);
        result.unwrap();
        assert_eq!(
            message.authentication.unwrap().challenges.len(),
            usize::from(tls)
        );
        server.set_nonblocking(true).unwrap();
        assert_eq!(
            server.read(&mut [0]).unwrap_err().kind(),
            std::io::ErrorKind::WouldBlock
        );
    }
}

#[test]
fn session_timeout_is_lossless_and_explicit_keepalive_preserves_the_track() {
    for (version, wire) in [(Version::V1, "1.0"), (Version::V2, "2.0")] {
        let (stream, mut server) = UnixStream::pair().unwrap();
        let mut client = Rtsp::from_stream(stream.into(), URI, version, 4096).unwrap();
        assert!(client.session_info("retained-session").unwrap().is_none());
        client
            .request(
                "SETUP",
                URI,
                &[("Transport", "RTP/AVP/TCP;unicast;interleaved=0-1")],
                &[],
                SECOND,
            )
            .0
            .unwrap();
        request(&mut server);
        server.write_all(format!("RTSP/{wire} 200 OK\r\nCSeq: 1\r\nSession: retained-session;timeout=18446744073709551615\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n").as_bytes()).unwrap();
        client.receive(SECOND).0.unwrap();
        let info = client.session_info("retained-session").unwrap().unwrap();
        assert_eq!(info.timeout_seconds, u64::MAX);
        assert!(info.timeout_explicit);
        client
            .request(
                "GET_PARAMETER",
                URI,
                &[("Session", "retained-session")],
                &[],
                SECOND,
            )
            .0
            .unwrap();
        request(&mut server);
        server
            .write_all(
                format!("RTSP/{wire} 200 OK\r\nCSeq: 2\r\nSession: retained-session\r\n\r\n")
                    .as_bytes(),
            )
            .unwrap();
        client.receive(SECOND).0.unwrap();
        let info = client.session_info("retained-session").unwrap().unwrap();
        assert_eq!(info.timeout_seconds, u64::MAX);
        assert!(info.control_response_age < SECOND);
        assert_eq!(
            client.state("retained-session", URI).unwrap(),
            Some(imapipe_media::rtsp::TrackState::Ready)
        );
    }
}
