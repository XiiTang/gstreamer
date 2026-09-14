use imapipe_media::sdp::{Description, Parameter};
#[test]
#[ignore = "requires independent FFmpeg executable"]
fn independent_ffmpeg_pcmu_description_retains_actual_payload_mapping() {
    let executable = std::env::var_os("IMAPIPE_FFMPEG").expect("explicit FFmpeg executable");
    let socket = std::net::UdpSocket::bind("127.0.0.1:0").unwrap();
    let path = std::env::temp_dir().join(format!(
        "imapipe-sdp-{}-{}.sdp",
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
