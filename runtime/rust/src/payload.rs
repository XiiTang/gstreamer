//! Typed payload configuration and frame views for the exclusive RTP Session.
use std::{
    ffi::{CString, c_char, c_void},
    ptr::NonNull,
};
#[repr(i32)]
#[derive(Clone, Copy, Debug)]
pub enum Format {
    Raw,
    H264,
    H265,
    Jpeg,
    Opus,
    Aac,
    Pcma,
    Pcmu,
}
pub struct Configuration<'a> {
    pub format: Format,
    pub sending: bool,
    pub payload_type: u8,
    pub ssrc: u32,
    pub sequence: u16,
    pub timestamp: u32,
    pub mtu: u32,
    pub clock_rate: u32,
    pub channels: u8,
    pub width: u32,
    pub height: u32,
    pub codec_data: &'a [u8],
    pub h264_parameter_sets: &'a str,
    pub h265_vps: &'a str,
    pub h265_sps: &'a str,
    pub h265_pps: &'a str,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Error {
    Configuration,
    Flow(i32),
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Configuration => f.write_str("Invalid native RTP payload configuration"),
            Self::Flow(code) => write!(f, "Native RTP payload flow result {code}"),
        }
    }
}
impl std::error::Error for Error {}
#[repr(C)]
pub(crate) struct Settings {
    format: i32,
    sending: i32,
    payload_type: u32,
    ssrc: u32,
    sequence: u32,
    timestamp: u32,
    mtu: u32,
    clock_rate: u32,
    channels: u32,
    width: u32,
    height: u32,
    codec_data: *const u8,
    codec_data_length: usize,
    h264_parameter_sets: *const c_char,
    h265_vps: *const c_char,
    h265_sps: *const c_char,
    h265_pps: *const c_char,
}
#[repr(C)]
#[derive(Default)]
struct FrameView {
    data: *const u8,
    length: usize,
    pts: u64,
    duration: u64,
}
unsafe extern "C" {
    fn gst_runtime_payload_frame_view(frame: *mut c_void, view: *mut FrameView);
    fn gst_runtime_payload_frame_free(frame: *mut c_void);
}
#[derive(Debug)]
pub struct Frame {
    pub data: Vec<u8>,
    pub pts: Option<u64>,
    pub duration: Option<u64>,
}
pub(crate) fn with_settings<T>(
    c: &Configuration<'_>,
    use_settings: impl FnOnce(&Settings) -> T,
) -> Result<T, Error> {
    let parameters = [c.h264_parameter_sets, c.h265_vps, c.h265_sps, c.h265_pps].map(CString::new);
    let parameters = parameters
        .into_iter()
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| Error::Configuration)?;
    let settings = Settings {
        format: c.format as i32,
        sending: i32::from(c.sending),
        payload_type: c.payload_type.into(),
        ssrc: c.ssrc,
        sequence: c.sequence.into(),
        timestamp: c.timestamp,
        mtu: c.mtu,
        clock_rate: c.clock_rate,
        channels: c.channels.into(),
        width: c.width,
        height: c.height,
        codec_data: c.codec_data.as_ptr(),
        codec_data_length: c.codec_data.len(),
        h264_parameter_sets: parameters[0].as_ptr(),
        h265_vps: parameters[1].as_ptr(),
        h265_sps: parameters[2].as_ptr(),
        h265_pps: parameters[3].as_ptr(),
    };
    Ok(use_settings(&settings))
}
struct NativeFrame(NonNull<c_void>);
impl Drop for NativeFrame {
    fn drop(&mut self) {
        unsafe { gst_runtime_payload_frame_free(self.0.as_ptr()) };
    }
}
/// Takes the native sample lease, copies its public media bytes, then unmaps.
pub(crate) unsafe fn take_frame(raw: *mut c_void) -> Result<Frame, Error> {
    let frame = NativeFrame(NonNull::new(raw).ok_or(Error::Flow(-5))?);
    let mut view = FrameView::default();
    unsafe { gst_runtime_payload_frame_view(frame.0.as_ptr(), &mut view) };
    let data = if view.length == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(view.data, view.length) }.to_vec()
    };
    Ok(Frame {
        data,
        pts: (view.pts != u64::MAX).then_some(view.pts),
        duration: (view.duration != u64::MAX).then_some(view.duration),
    })
}
