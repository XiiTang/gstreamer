use imapipe_media::payload::{self, Configuration, Format};
use std::{
    sync::mpsc,
    time::{Duration, Instant},
};
fn config(format: Format, sending: bool) -> Configuration<'static> {
    Configuration {
        format,
        sending,
        payload_type: 96,
        ssrc: 123,
        sequence: 65535,
        timestamp: 0xffff0000,
        mtu: 256,
        clock_rate: 8000,
        channels: 1,
        width: 0,
        height: 0,
        codec_data: &[],
        h264_parameter_sets: "",
        h265_vps: "",
        h265_sps: "",
        h265_pps: "",
    }
}
#[test]
fn independent_input_output_preserve_frames_and_raw_packets() {
    for format in [Format::Pcma, Format::Pcmu] {
        let (_send, mut input, mut packets) = payload::open(&config(format, true)).unwrap();
        let (_receive, mut packet_input, mut frames) =
            payload::open(&config(format, false)).unwrap();
        let samples: Vec<u8> = (0..160).collect();
        for i in 0..2u64 {
            input
                .push(&samples, Some(i * 20_000_000), Some(20_000_000))
                .unwrap();
            let packet = packets.pull(Duration::from_secs(1)).unwrap().unwrap();
            assert_eq!(
                &packet.data[2..4],
                &(65535u16.wrapping_add(i as u16)).to_be_bytes()
            );
            packet_input
                .push(&packet.data, packet.pts, packet.duration)
                .unwrap();
            assert_eq!(
                frames.pull(Duration::from_secs(1)).unwrap().unwrap().data,
                samples
            );
        }
    }
    let (_control, mut input, mut output) = payload::open(&config(Format::Raw, true)).unwrap();
    let bytes = [0x80, 96, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 255, 0, 1];
    input.push(&bytes, Some(123), None).unwrap();
    assert_eq!(
        output.pull(Duration::from_secs(1)).unwrap().unwrap().data,
        bytes
    );
    assert!(input.push(&[0; 12], None, None).is_err());
}
#[test]
fn dropping_control_releases_blocked_writer_before_join() {
    let (control, mut input, _output) = payload::open(&config(Format::Pcma, true)).unwrap();
    let (ready, started) = mpsc::channel();
    let worker = std::thread::spawn(move || {
        for i in 0..10000 {
            if i == 8 {
                ready.send(()).unwrap();
            }
            if input.push(&[0; 160], Some(0), None).is_err() {
                return;
            }
        }
        panic!("Native queues did not apply backpressure");
    });
    started.recv_timeout(Duration::from_secs(1)).unwrap();
    std::thread::sleep(Duration::from_millis(20));
    let start = Instant::now();
    drop(control);
    worker.join().unwrap();
    assert!(start.elapsed() < Duration::from_millis(200));
}
