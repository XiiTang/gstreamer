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
    pub feedback: Option<&'a Feedback>,
    pub bandwidth_bps: Option<u64>,
    pub payload: Option<&'a crate::payload::Configuration<'a>>,
    pub reorder_latency: Option<Duration>,
    pub negotiated: Option<&'a crate::sdp::Selection>,
}
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct Rtx {
    pub payload_type: u32,
    pub ssrc: u32,
    pub peer_ssrc: u32,
    pub peer_rtx_ssrc: u32,
    pub cache_packets: u32,
    pub cache_time_ms: u32,
}
#[derive(Clone, Copy, Debug)]
pub struct Feedback {
    pub nack: bool,
    pub pli: bool,
    pub fir: bool,
    pub rtx: Option<Rtx>,
}
#[derive(Clone, Copy, Debug)]
pub enum FeedbackRequest {
    Nack {
        ssrc: u32,
        sequence: u16,
        max_delay: Duration,
    },
    PictureLoss {
        ssrc: u32,
    },
    FullIntraRequest {
        ssrc: u32,
    },
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
pub(crate) struct Settings {
    ssrc: u32,
    payload_type: u32,
    clock_rate: u32,
    probation: u32,
    rtcp_min_interval: u64,
    reports: i32,
    feedback: u32,
    rtx: *const Rtx,
    payload: *const crate::payload::Settings,
    reorder: i32,
    latency_ms: u32,
    bandwidth_bps: f64,
    negotiated_caps: *const c_void,
}
/// A value of the native session statistics, as the GType it holds names it.
#[derive(Clone, Debug, PartialEq)]
pub enum Statistic {
    Int(i32),
    Uint(u32),
    Int64(i64),
    Uint64(u64),
    Double(f64),
    Boolean(bool),
    String(String),
    Structure(Statistics),
    List(Vec<Statistic>),
}
/// A native statistics structure: its name, and its fields named as GStreamer
/// names them, in the structure's order.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Statistics {
    pub name: String,
    pub fields: Vec<(String, Statistic)>,
}
impl Statistics {
    pub fn get(&self, name: &str) -> Option<&Statistic> {
        self.fields
            .iter()
            .find_map(|(field, value)| (field == name).then_some(value))
    }
}
#[repr(C)]
struct RawStatistic {
    kind: u32,
    name: *const c_char,
    integer: i64,
    unsigned_integer: u64,
    number: f64,
    boolean: i32,
    text: *const c_char,
}
/// Assembles the native walk's open/value/end sequence into one tree.
#[derive(Default)]
struct StatisticsBuilder {
    open: Vec<(Option<String>, Statistic)>,
    done: Option<Statistics>,
    invalid: bool,
}
impl StatisticsBuilder {
    fn text(pointer: *const c_char) -> Option<String> {
        if pointer.is_null() {
            return None;
        }
        let text = unsafe { CStr::from_ptr(pointer) }.to_str().ok()?;
        Some(text.to_owned())
    }
    fn add(&mut self, raw: &RawStatistic) -> Option<()> {
        let name = match raw.name.is_null() {
            true => None,
            false => Some(Self::text(raw.name)?),
        };
        let value = match raw.kind {
            0 => Statistic::Int(i32::try_from(raw.integer).ok()?),
            1 => Statistic::Uint(u32::try_from(raw.unsigned_integer).ok()?),
            2 => Statistic::Int64(raw.integer),
            3 => Statistic::Uint64(raw.unsigned_integer),
            4 => Statistic::Double(raw.number),
            5 => Statistic::Boolean(raw.boolean != 0),
            6 => Statistic::String(Self::text(raw.text)?),
            7 => {
                let structure = Statistics {
                    name: Self::text(raw.text)?,
                    fields: Vec::new(),
                };
                self.open.push((name, Statistic::Structure(structure)));
                return Some(());
            }
            8 => {
                self.open.push((name, Statistic::List(Vec::new())));
                return Some(());
            }
            9 => {
                let (name, value) = self.open.pop()?;
                if self.open.is_empty() {
                    // The outer structure, which no field names.
                    let (None, Statistic::Structure(statistics)) = (name, value) else {
                        return None;
                    };
                    return self.done.replace(statistics).is_none().then_some(());
                }
                return self.place(name, value);
            }
            _ => return None,
        };
        self.place(name, value)
    }
    /// A field takes its name inside a structure; a list element has none.
    fn place(&mut self, name: Option<String>, value: Statistic) -> Option<()> {
        match (self.open.last_mut()?, name) {
            ((_, Statistic::Structure(structure)), Some(name)) => {
                structure.fields.push((name, value))
            }
            ((_, Statistic::List(list)), None) => list.push(value),
            _ => return None,
        }
        Some(())
    }
}
unsafe extern "C" fn visit_statistic(raw: *const RawStatistic, user_data: *mut c_void) -> i32 {
    let builder = unsafe { &mut *user_data.cast::<StatisticsBuilder>() };
    if builder.add(unsafe { &*raw }).is_none() {
        builder.invalid = true;
        return 1;
    }
    0
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
    fn gst_runtime_rtp_session_feedback(
        session: *mut c_void,
        kind: u32,
        ssrc: u32,
        sequence: u16,
        delay: u64,
    ) -> i32;
    fn gst_runtime_rtp_session_statistics(
        session: *mut c_void,
        visitor: unsafe extern "C" fn(*const RawStatistic, *mut c_void) -> i32,
        user_data: *mut c_void,
    ) -> i32;
    fn gst_runtime_rtp_session_stop(session: *mut c_void);
    fn gst_runtime_rtp_session_free(session: *mut c_void);
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
pub(crate) fn with_settings<T>(
    c: &Configuration<'_>,
    apply: impl FnOnce(&Settings) -> T,
) -> Result<T, Error> {
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
        feedback: c.feedback.map_or(0, |f| {
            u32::from(f.nack) | (u32::from(f.pli) << 1) | (u32::from(f.fir) << 2)
        }),
        rtx: c
            .feedback
            .and_then(|f| f.rtx.as_ref())
            .map_or(std::ptr::null(), |r| r),
        bandwidth_bps: c.bandwidth_bps.unwrap_or(0) as f64,
        negotiated_caps: c
            .negotiated
            .map_or(std::ptr::null(), |selection| selection.as_ptr()),
        payload: std::ptr::null(),
        reorder: i32::from(c.reorder_latency.is_some()),
        latency_ms: c
            .reorder_latency
            .map_or(Ok(0), |d| u32::try_from(d.as_millis()))
            .map_err(|_| Error(-5))?,
    };
    let result = match c.payload {
        Some(payload) => crate::payload::with_settings(payload, |native| {
            settings.payload = native;
            apply(&settings)
        })
        .map_err(|_| Error(-5))?,
        None => apply(&settings),
    };
    Ok(result)
}
impl Session {
    pub fn new(c: Configuration<'_>) -> Result<Self, Error> {
        crate::initialize();
        let raw = with_settings(&c, |settings| unsafe {
            gst_runtime_rtp_session_new(settings)
        })?;
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
    pub fn feedback(&mut self, request: FeedbackRequest) -> Result<bool, Error> {
        let (kind, ssrc, sequence, delay) = match request {
            FeedbackRequest::Nack {
                ssrc,
                sequence,
                max_delay,
            } => (
                1,
                ssrc,
                sequence,
                u64::try_from(max_delay.as_nanos()).map_err(|_| Error(-5))?,
            ),
            FeedbackRequest::PictureLoss { ssrc } => (2, ssrc, 0, 0),
            FeedbackRequest::FullIntraRequest { ssrc } => (4, ssrc, 0, 0),
        };
        match unsafe {
            gst_runtime_rtp_session_feedback(self.inner.0.as_ptr(), kind, ssrc, sequence, delay)
        } {
            0 => Ok(false),
            1 => Ok(true),
            code => Err(Error(code)),
        }
    }
    /// The native session statistics as typed fields; a value of a type no
    /// `Statistic` names fails rather than being dropped or written as text.
    pub fn statistics(&mut self) -> Result<Statistics, Error> {
        let mut builder = StatisticsBuilder::default();
        let result = unsafe {
            gst_runtime_rtp_session_statistics(
                self.inner.0.as_ptr(),
                visit_statistic,
                (&raw mut builder).cast(),
            )
        };
        match (result, builder) {
            (
                0,
                StatisticsBuilder {
                    done: Some(statistics),
                    invalid: false,
                    ..
                },
            ) => Ok(statistics),
            (code, _) if code < 0 => Err(Error(code)),
            _ => Err(Error(-5)),
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn feedback_abi_enforces_capabilities_and_mapping() {
        let feedback = Feedback {
            nack: true,
            pli: true,
            fir: true,
            rtx: Some(Rtx {
                payload_type: 97,
                ssrc: 70,
                peer_ssrc: 8,
                peer_rtx_ssrc: 80,
                cache_packets: 32,
                cache_time_ms: 1000,
            }),
        };
        let config = Configuration {
            ssrc: 7,
            payload_type: 96,
            clock_rate: 90000,
            probation: 0,
            reports: Some(Duration::from_millis(10)),
            feedback: Some(&feedback),
            bandwidth_bps: Some(128000),
            payload: None,
            reorder_latency: Some(Duration::from_millis(200)),
            negotiated: None,
        };
        let mut session = Session::new(config).unwrap();
        assert!(
            !session
                .feedback(FeedbackRequest::PictureLoss { ssrc: 8 })
                .unwrap()
        );
        assert!(
            session
                .feedback(FeedbackRequest::PictureLoss { ssrc: 9 })
                .is_err()
        );
        assert!(
            session
                .feedback(FeedbackRequest::Nack {
                    ssrc: 8,
                    sequence: 1,
                    max_delay: Duration::MAX
                })
                .is_err()
        );
        assert_eq!(
            session.statistics().unwrap().get("rtx-sent"),
            Some(&Statistic::Uint(0))
        );
        assert!(
            Session::new(Configuration {
                reports: None,
                ..config
            })
            .is_err()
        );
        assert!(
            Session::new(Configuration {
                reorder_latency: None,
                negotiated: None,
                ..config
            })
            .is_err()
        );
        let bad = Feedback {
            rtx: Some(Rtx {
                peer_ssrc: 7,
                ..feedback.rtx.unwrap()
            }),
            ..feedback
        };
        assert!(
            Session::new(Configuration {
                feedback: Some(&bad),
                ..config
            })
            .is_err()
        );
        session.cancellation().cancel();
        assert!(
            session
                .feedback(FeedbackRequest::PictureLoss { ssrc: 8 })
                .is_err()
        );
    }
    #[test]
    fn nonblocking_ports_preserve_packets_and_cancel_full_queues() {
        let mut s = Session::new(Configuration {
            ssrc: 7,
            payload_type: 96,
            clock_rate: 90000,
            probation: 0,
            reports: None,
            feedback: None,
            bandwidth_bps: None,
            payload: None,
            reorder_latency: None,
            negotiated: None,
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
        // The sender's own source counts the packet it sent as a 64-bit field.
        let statistics = s.statistics().unwrap();
        assert_eq!(statistics.name, "application/x-rtp-session-stats");
        assert_eq!(statistics.get("sent-nack-count"), Some(&Statistic::Uint(0)));
        let Some(Statistic::List(sources)) = statistics.get("source-stats") else {
            panic!("no source-stats list: {statistics:?}");
        };
        let source = sources
            .iter()
            .find_map(|source| match source {
                Statistic::Structure(source) if source.get("ssrc") == Some(&Statistic::Uint(7)) => {
                    Some(source)
                }
                _ => None,
            })
            .unwrap();
        assert_eq!(source.name, "application/x-rtp-source-stats");
        assert_eq!(source.get("internal"), Some(&Statistic::Boolean(true)));
        assert!(matches!(
            source.get("packets-sent"),
            Some(Statistic::Uint64(1..))
        ));
        assert!(matches!(
            source.get("packets-lost"),
            Some(Statistic::Int(_))
        ));
        assert!(matches!(
            source.get("received-rr"),
            Some(Statistic::List(_))
        ));
        let cancellation = s.cancellation();
        let start = std::time::Instant::now();
        cancellation.cancel();
        drop(s);
        assert!(start.elapsed() < Duration::from_millis(200));
        drop(cancellation);
    }
}
