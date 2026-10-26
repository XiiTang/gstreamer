mod common;
use boundless_media::{
    payload::{Configuration, Format, Frame},
    rtp::{self, Input, Output, Session},
};
use std::sync::Arc;
use std::time::{Duration, Instant};

fn config(format: Format) -> Configuration<'static> {
    Configuration {
        format,
        sending: true,
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

fn session(payload: Option<&Configuration<'_>>, ssrc: u32) -> (Session, Arc<common::Recording>) {
    let rec = Arc::new(common::Recording::default());
    let owner = Session::new(
        rtp::Configuration {
            ssrc,
            payload_type: 96,
            clock_rate: 8000,
            probation: 0,
            reports: None,
            feedback: None,
            bandwidth_bps: None,
            payload,
            reorder_latency: None,
            negotiated: None,
        },
        rec.clone(),
    )
    .unwrap();
    (owner, rec)
}

fn read(session: &mut Session, port: Output) -> Frame {
    let end = Instant::now() + Duration::from_secs(5);
    loop {
        if let Some(frame) = session.try_read(port).unwrap() {
            return frame;
        }
        assert!(Instant::now() < end, "session did not produce {port:?}");
        std::thread::yield_now();
    }
}

#[test]
fn production_sessions_preserve_encoded_frames_and_raw_packets() {
    for format in [Format::Pcma, Format::Pcmu] {
        let codec = config(format);
        let (mut send, send_facts) = session(Some(&codec), 123);
        let receive_codec = Configuration {
            ssrc: 456,
            ..config(format)
        };
        let (mut receive, receive_facts) = session(Some(&receive_codec), 456);
        let samples: Vec<u8> = (0..160).collect();
        for index in 0..2u64 {
            assert!(
                send.try_write_frame(&samples, index * 20_000_000, Some(20_000_000))
                    .unwrap()
            );
            let packet = read(&mut send, Output::SendRtp);
            assert_eq!(
                &packet.data[2..4],
                &(65535u16.wrapping_add(index as u16)).to_be_bytes()
            );
            assert_eq!(&packet.data[8..12], &123u32.to_be_bytes());
            assert!(receive.try_write(Input::ReceiveRtp, &packet.data).unwrap());
            assert_eq!(read(&mut receive, Output::ReceiveRtp).data, samples);
        }
        drop(send);
        drop(receive);
        send_facts.assert_none();
        receive_facts.assert_none();
    }
    let (mut raw, raw_facts) = session(None, 123);
    let bytes = [0x80, 96, 0, 0, 0, 1, 2, 3, 0, 0, 0, 123, 255, 0, 1];
    assert!(raw.try_write(Input::SendRtp, &bytes).unwrap());
    assert_eq!(read(&mut raw, Output::SendRtp).data, bytes);
    assert!(raw.try_write(Input::SendRtp, &[0; 12]).is_err());
    drop(raw);
    raw_facts.assert_none();
}

#[test]
fn production_session_cancellation_releases_pressured_frame_input() {
    let codec = config(Format::Pcma);
    let (mut owner, owner_facts) = session(Some(&codec), 123);
    let end = Instant::now() + Duration::from_secs(5);
    let mut admitted = 0;
    while owner
        .try_write_frame(&[0; 160], 0, Some(20_000_000))
        .unwrap()
    {
        admitted += 1;
        assert!(Instant::now() < end, "frame input did not apply pressure");
    }
    assert!(admitted > 0);
    let cancellation = owner.cancellation();
    cancellation.cancel();
    assert!(
        owner
            .try_write_frame(&[0; 160], 0, Some(20_000_000))
            .is_err()
    );
    let outcome = owner.close();
    assert!(outcome.bus_errors == 0, "{outcome:?}");
    drop(cancellation);
    owner_facts.assert_none();
}
