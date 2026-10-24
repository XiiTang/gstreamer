use boundless_media::sdp::{Description, Parameter};
#[test]
#[ignore = "requires independent FFmpeg executable"]
fn independent_ffmpeg_pcmu_description_retains_actual_payload_mapping() {
    let executable = std::env::var_os("BOUNDLESS_FFMPEG").expect("explicit FFmpeg executable");
    let socket = std::net::UdpSocket::bind("127.0.0.1:0").unwrap();
    let path = std::env::temp_dir().join(format!(
        "boundless-sdp-{}-{}.sdp",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    let status = std::process::Command::new(executable)
        .args([
            "-v",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "anullsrc=r=8000:cl=mono",
            "-frames:a",
            "1",
            "-c:a",
            "pcm_mulaw",
            "-f",
            "rtp",
            "-sdp_file",
        ])
        .arg(&path)
        .arg(format!("rtp://{}", socket.local_addr().unwrap()))
        .status()
        .unwrap();
    assert!(status.success());
    let bytes = std::fs::read(&path).unwrap();
    std::fs::remove_file(path).unwrap();
    let sdp = Description::parse(&bytes).unwrap();
    assert_eq!(sdp.media.len(), 1);
    assert_eq!(sdp.media[0].kind, "audio");
    assert_eq!(sdp.media[0].port, socket.local_addr().unwrap().port());
    assert_eq!(sdp.media[0].formats[0].name, "0");
    assert!(
        sdp.media[0].formats[0]
            .parameters
            .contains(&("encoding-name".into(), Parameter::Text("PCMU".into())))
    );
    assert!(
        sdp.media[0].formats[0]
            .parameters
            .contains(&("clock-rate".into(), Parameter::Integer(8000)))
    );
}

#[test]
fn selected_description_owns_caps_and_enforces_the_negotiated_track() {
    use boundless_media::{
        rtp,
        sdp::{Selection, Track},
    };
    let config = rtp::Configuration {
        ssrc: 42,
        payload_type: 0,
        clock_rate: 8000,
        probation: 0,
        reports: None,
        feedback: None,
        bandwidth_bps: None,
        payload: None,
        reorder_latency: None,
        negotiated: None,
    };
    let body = b"v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=x\r\nt=0 0\r\na=control:track\r\nm=audio 0 RTP/AVP 0\r\n";
    let track = || Track {
        media: 0,
        base_uri: "rtsp://localhost/media/",
        uri: "rtsp://localhost/media/track",
        profile: "AVP",
        lower_transport: "tcp",
        record: false,
    };
    let selected = Selection::new(body, track(), &config).unwrap();
    assert!(
        Selection::new(
            body,
            track(),
            &rtp::Configuration {
                clock_rate: 90000,
                ..config
            }
        )
        .is_err()
    );
    assert!(
        Selection::new(
            body,
            Track {
                uri: "rtsp://localhost/other",
                ..track()
            },
            &config
        )
        .is_err()
    );
    let mut session = rtp::Session::new(rtp::Configuration {
        negotiated: Some(&selected),
        ..config
    })
    .unwrap();
    drop(selected);
    assert!(session.statistics().is_ok());
}

fn feedback_description(feedback: &[String]) -> String {
    let mut body = String::from("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=feedback\r\nt=0 0\r\n");
    let formats = (96..128)
        .map(|pt| pt.to_string())
        .collect::<Vec<_>>()
        .join(" ");
    for _ in 0..4 {
        body.push_str(&format!("m=audio 5004 RTP/AVP {formats}\r\n"));
        for pt in 96..128 {
            body.push_str(&format!("a=rtpmap:{pt} PCMU/8000\r\n"));
        }
        for value in feedback {
            body.push_str(&format!("a=rtcp-fb:* {value}\r\n"));
        }
    }
    body
}

fn assert_feedback_for_each_format(feedback: Vec<String>) {
    let body = feedback_description(&feedback);
    let description = Description::parse(body.as_bytes()).unwrap();
    assert_eq!(description.media.len(), 4);
    let keys = feedback
        .iter()
        .map(|value| format!("rtcp-fb-{value}"))
        .collect::<Vec<_>>();
    for media in &description.media {
        assert_eq!(media.formats.len(), 32);
        for (index, format) in media.formats.iter().enumerate() {
            assert_eq!(format.name, (index + 96).to_string());
            let parameters = format
                .parameters
                .iter()
                .filter(|(name, _)| name.starts_with("rtcp-fb-"))
                .collect::<Vec<_>>();
            assert_eq!(parameters.len(), keys.len());
            for ((name, value), expected) in parameters.into_iter().zip(&keys) {
                assert_eq!(name, expected);
                assert_eq!(value, &Parameter::Boolean(true));
            }
        }
    }
}

#[test]
fn media_feedback_is_retained_for_every_format() {
    assert_feedback_for_each_format(
        (0..600)
            .map(|index| format!("x-feedback-{index}"))
            .collect(),
    );
}

#[test]
fn long_feedback_names_are_retained_for_every_format() {
    let text = "x".repeat(24_000);
    assert_feedback_for_each_format(
        (0..12)
            .map(|index| format!("x-feedback-{index}-{text}"))
            .collect(),
    );
}
