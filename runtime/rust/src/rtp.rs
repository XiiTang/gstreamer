//! One exclusive, nonblocking RTP/RTCP session owner for a declared transport.
//! The native engine owns sources, packet accounting and RTCP scheduling.
use std::{
    cell::Cell,
    ffi::{CStr, c_char, c_void},
    marker::PhantomData,
    ptr::NonNull,
    sync::Arc,
    time::Duration,
};
#[derive(Clone, Copy)]
pub struct Configuration<'a> {
    pub ssrc: u32,
    pub payload_type: u8,
    pub clock_rate: u32,
    pub probation: u32,
    pub reports: Option<Duration>,
    pub feedback_profile: bool,
    pub bandwidth_bps: Option<u64>,
    pub payload: Option<&'a crate::payload::Configuration<'a>>,
    pub reorder_latency: Option<Duration>,
}
#[derive(Debug, Clone, Copy)]
#[repr(i32)]
pub enum Input {
    SendRtp = 0,
    ReceiveRtp = 1,
    ReceiveRtcp = 2,
}
#[derive(Debug, Clone, Copy)]
#[repr(i32)]
pub enum Output {
    SendRtp = 0,
    ReceiveRtp = 1,
    SendRtcp = 2,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Error(pub i32);
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Native RTP flow result {}", self.0)
    }
}
impl std::error::Error for Error {}
#[repr(C)]
struct Settings {
    ssrc: u32,
    payload_type: u32,
    clock_rate: u32,
    probation: u32,
    rtcp_min_interval: u64,
    reports: i32,
    feedback_profile: i32,
    payload: *const crate::payload::Settings,
    reorder: i32,
    latency_ms: u32,
    bandwidth_bps: f64,
}
unsafe extern "C" {
    fn gst_runtime_rtp_session_new(settings: *const Settings) -> *mut c_void;
    fn gst_runtime_rtp_session_try_write(
        session: *mut c_void,
        port: i32,
        data: *const u8,
        length: usize,
    ) -> i32;
    fn gst_runtime_rtp_session_try_write_frame(
        session: *mut c_void,
        data: *const u8,
        length: usize,
        pts: u64,
        duration: u64,
    ) -> i32;
    fn gst_runtime_rtp_session_pull(
        session: *mut c_void,
        port: i32,
        timeout: u64,
        frame: *mut *mut c_void,
    ) -> i32;
    fn gst_runtime_rtp_session_report(session: *mut c_void, delay: u64) -> i32;
    fn gst_runtime_rtp_session_stats(session: *mut c_void) -> *mut c_char;
    fn gst_runtime_rtp_session_stop(session: *mut c_void);
    fn gst_runtime_rtp_session_free(session: *mut c_void);
    fn gst_runtime_rtp_session_stats_free(memory: *mut c_char);
}
struct Inner(NonNull<c_void>);
// Only cancellation is shared; all data and state entry points require the
// unique Session. Native stop is designed to run concurrently with them.
unsafe impl Send for Inner {}
unsafe impl Sync for Inner {}
impl Drop for Inner {
    fn drop(&mut self) {
        unsafe { gst_runtime_rtp_session_free(self.0.as_ptr()) }
    }
}
pub struct Session {
    inner: Arc<Inner>,
    _exclusive: PhantomData<Cell<()>>,
}
#[derive(Clone)]
pub struct Cancellation(Arc<Inner>);
impl Cancellation {
    pub fn cancel(&self) {
        unsafe { gst_runtime_rtp_session_stop(self.0.0.as_ptr()) }
    }
}
impl Drop for Session {
    fn drop(&mut self) {
        self.cancellation().cancel();
    }
}
impl Session {
    pub fn new(c: Configuration<'_>) -> Result<Self, Error> {
        crate::initialize();
        let mut settings = Settings {
            ssrc: c.ssrc,
            payload_type: c.payload_type.into(),
            clock_rate: c.clock_rate,
            probation: c.probation,
            rtcp_min_interval: c
                .reports
                .map_or(Ok(0), |v| u64::try_from(v.as_nanos()))
                .map_err(|_| Error(-5))?,
            reports: i32::from(c.reports.is_some()),
            feedback_profile: i32::from(c.feedback_profile),
            bandwidth_bps: c.bandwidth_bps.unwrap_or(0) as f64,
            payload: std::ptr::null(),
            reorder: i32::from(c.reorder_latency.is_some()),
            latency_ms: c
                .reorder_latency
                .map_or(Ok(0), |d| u32::try_from(d.as_millis()))
                .map_err(|_| Error(-5))?,
        };
        let raw = match c.payload {
            Some(payload) => crate::payload::with_settings(payload, |native| {
                settings.payload = native;
                unsafe { gst_runtime_rtp_session_new(&settings) }
            })
            .map_err(|_| Error(-5))?,
            None => unsafe { gst_runtime_rtp_session_new(&settings) },
        };
        let raw = NonNull::new(raw).ok_or(Error(-5))?;
        Ok(Self {
            inner: Arc::new(Inner(raw)),
            _exclusive: PhantomData,
        })
    }
    pub fn cancellation(&self) -> Cancellation {
        Cancellation(self.inner.clone())
    }
    /// False means bounded native input pressure; no bytes were admitted.
    pub fn try_write(&mut self, port: Input, packet: &[u8]) -> Result<bool, Error> {
        match unsafe {
            gst_runtime_rtp_session_try_write(
                self.inner.0.as_ptr(),
                port as _,
                packet.as_ptr(),
                packet.len(),
            )
        } {
            0 => Ok(true),
            1 => Ok(false),
            code => Err(Error(code)),
        }
    }
    pub fn try_write_frame(
        &mut self,
        bytes: &[u8],
        pts: u64,
        duration: Option<u64>,
    ) -> Result<bool, Error> {
        match unsafe {
            gst_runtime_rtp_session_try_write_frame(
                self.inner.0.as_ptr(),
                bytes.as_ptr(),
                bytes.len(),
                pts,
                duration.unwrap_or(u64::MAX),
            )
        } {
            0 => Ok(true),
            1 => Ok(false),
            code => Err(Error(code)),
        }
    }
    pub fn try_read(&mut self, port: Output) -> Result<Option<crate::payload::Frame>, Error> {
        let mut raw = std::ptr::null_mut();
        match unsafe { gst_runtime_rtp_session_pull(self.inner.0.as_ptr(), port as _, 0, &mut raw) }
        {
            0 => unsafe { crate::payload::take_frame(raw) }
                .map(Some)
                .map_err(|_| Error(-5)),
            1 => Ok(None),
            code => Err(Error(code)),
        }
    }
    pub fn report(&mut self, max_delay: Duration) -> Result<bool, Error> {
        let delay = u64::try_from(max_delay.as_nanos()).map_err(|_| Error(-5))?;
        Ok(unsafe { gst_runtime_rtp_session_report(self.inner.0.as_ptr(), delay) } != 0)
    }
    pub fn statistics(&mut self) -> Result<String, Error> {
        let raw = unsafe { gst_runtime_rtp_session_stats(self.inner.0.as_ptr()) };
        if raw.is_null() {
            return Err(Error(-5));
        }
        let value = unsafe { CStr::from_ptr(raw) }
            .to_string_lossy()
            .into_owned();
        unsafe { gst_runtime_rtp_session_stats_free(raw) };
        Ok(value)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn nonblocking_ports_preserve_packets_and_cancel_full_queues() {
        let mut s = Session::new(Configuration {
            ssrc: 7,
            payload_type: 96,
            clock_rate: 90000,
            probation: 0,
            reports: None,
            feedback_profile: false,
            bandwidth_bps: None,
            payload: None,
            reorder_latency: None,
        })
        .unwrap();
        let packet = [128, 96, 0, 1, 0, 0, 0, 1, 0, 0, 0, 7, 9];
        assert!(s.try_write(Input::SendRtp, &packet).unwrap());
        let deadline = std::time::Instant::now() + Duration::from_secs(1);
        loop {
            if let Some(result) = s.try_read(Output::SendRtp).unwrap() {
                assert_eq!(result.data, packet);
                break;
            }
            assert!(std::time::Instant::now() < deadline);
            std::thread::yield_now();
        }
        let mut pressure = false;
        for _ in 0..1000 {
            if !s.try_write(Input::SendRtp, &packet).unwrap() {
                pressure = true;
                break;
            }
        }
        assert!(pressure);
        assert!(s.statistics().unwrap().contains("source-stats"));
        let cancellation = s.cancellation();
        let start = std::time::Instant::now();
        cancellation.cancel();
        drop(s);
        assert!(start.elapsed() < Duration::from_millis(200));
        drop(cancellation);
    }
}
