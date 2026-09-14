//! Typed encoded-frame/RTP transformation. Dropping the control owner stops
//! blocked input/output; callers join their workers before releasing resources.
use std::{
    cell::Cell,
    ffi::{CString, c_char, c_void},
    marker::PhantomData,
    ptr::NonNull,
    sync::Arc,
    time::Duration,
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
pub enum Error {
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
struct Settings {
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
    fn gst_runtime_payload_create(settings: *const Settings) -> *mut c_void;
    fn gst_runtime_payload_write(
        payload: *mut c_void,
        data: *const u8,
        length: usize,
        pts: u64,
        duration: u64,
    ) -> i32;
    fn gst_runtime_payload_read(payload: *mut c_void, timeout: u64, frame: *mut *mut c_void)
    -> i32;
    fn gst_runtime_payload_frame_view(frame: *mut c_void, view: *mut FrameView);
    fn gst_runtime_payload_frame_free(frame: *mut c_void);
    fn gst_runtime_payload_stop(payload: *mut c_void);
    fn gst_runtime_payload_free(payload: *mut c_void);
}
struct Inner(NonNull<c_void>);
// Native appsrc/appsink permit one producer and one consumer concurrently with
// state transition to NULL. Exclusive handles enforce the producer/consumer rule.
unsafe impl Send for Inner {}
unsafe impl Sync for Inner {}
impl Drop for Inner {
    fn drop(&mut self) {
        unsafe { gst_runtime_payload_free(self.0.as_ptr()) };
    }
}
pub struct Control(Arc<Inner>);
impl Control {
    pub fn stop(&self) {
        unsafe { gst_runtime_payload_stop(self.0.0.as_ptr()) };
    }
}
impl Drop for Control {
    fn drop(&mut self) {
        self.stop();
    }
}
pub struct Input {
    inner: Arc<Inner>,
    _exclusive: PhantomData<Cell<()>>,
}
pub struct Output {
    inner: Arc<Inner>,
    _exclusive: PhantomData<Cell<()>>,
}
#[derive(Debug)]
pub struct Frame {
    pub data: Vec<u8>,
    pub pts: Option<u64>,
    pub duration: Option<u64>,
}
pub fn open(c: &Configuration<'_>) -> Result<(Control, Input, Output), Error> {
    crate::initialize();
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
    let inner = Arc::new(Inner(
        NonNull::new(unsafe { gst_runtime_payload_create(&settings) })
            .ok_or(Error::Configuration)?,
    ));
    Ok((
        Control(inner.clone()),
        Input {
            inner: inner.clone(),
            _exclusive: PhantomData,
        },
        Output {
            inner,
            _exclusive: PhantomData,
        },
    ))
}
impl Input {
    pub fn push(
        &mut self,
        bytes: &[u8],
        pts: Option<u64>,
        duration: Option<u64>,
    ) -> Result<(), Error> {
        let code = unsafe {
            gst_runtime_payload_write(
                self.inner.0.as_ptr(),
                bytes.as_ptr(),
                bytes.len(),
                pts.unwrap_or(u64::MAX),
                duration.unwrap_or(u64::MAX),
            )
        };
        if code == 0 {
            Ok(())
        } else {
            Err(Error::Flow(code))
        }
    }
}
struct NativeFrame(NonNull<c_void>);
impl Drop for NativeFrame {
    fn drop(&mut self) {
        unsafe { gst_runtime_payload_frame_free(self.0.as_ptr()) };
    }
}
impl Output {
    pub fn pull(&mut self, timeout: Duration) -> Result<Option<Frame>, Error> {
        let mut frame = std::ptr::null_mut();
        let code = unsafe {
            gst_runtime_payload_read(
                self.inner.0.as_ptr(),
                timeout.as_nanos().min(u64::MAX as u128) as u64,
                &mut frame,
            )
        };
        if code == 1 {
            return Ok(None);
        }
        if code != 0 {
            return Err(Error::Flow(code));
        }
        let frame = NativeFrame(NonNull::new(frame).expect("Native payload frame contract"));
        let mut view = FrameView::default();
        unsafe { gst_runtime_payload_frame_view(frame.0.as_ptr(), &mut view) };
        let data = if view.length == 0 {
            Vec::new()
        } else {
            unsafe { std::slice::from_raw_parts(view.data, view.length) }.to_vec()
        };
        Ok(Some(Frame {
            data,
            pts: (view.pts != u64::MAX).then_some(view.pts),
            duration: (view.duration != u64::MAX).then_some(view.duration),
        }))
    }
}
