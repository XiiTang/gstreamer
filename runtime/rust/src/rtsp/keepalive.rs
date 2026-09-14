//! Declared session maintenance on the same exclusive RTSP owner. The caller
//! polls this with receive_step; no thread, socket, retry or alternate identity.
use super::{Dispatch, Error, Message, Rtsp, text};
use crate::rtsp_auth::Qop;
use std::{
    collections::BTreeMap,
    ffi::{c_char, c_void},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    time::{Duration, Instant},
};

#[derive(Clone, Copy, Debug)]
pub enum KeepaliveMethod {
    Options,
    GetParameter,
}
impl KeepaliveMethod {
    fn name(self) -> &'static str {
        match self {
            Self::Options => "OPTIONS",
            Self::GetParameter => "GET_PARAMETER",
        }
    }
}
#[derive(Clone, Debug)]
pub struct KeepaliveAuthentication {
    pub challenge: String,
    pub qop: Qop,
}
#[derive(Clone, Debug)]
pub struct Keepalive {
    pub uri: String,
    pub method: KeepaliveMethod,
    pub interval: Duration,
    /// Bounds the complete keepalive write and response, not the remote session.
    pub response_timeout: Duration,
    pub authentication: Option<KeepaliveAuthentication>,
}
#[derive(Clone)]
pub struct KeepaliveCancellation(Arc<AtomicBool>);
impl KeepaliveCancellation {
    /// Disable future sends; an already dispatched request retains its deadline.
    pub fn cancel(&self) {
        self.0.store(true, Ordering::Release);
    }
    fn cancelled(&self) -> bool {
        self.0.load(Ordering::Acquire)
    }
}
#[derive(Clone, Debug)]
pub struct KeepaliveStatus {
    pub enabled: bool,
    pub uri: String,
    pub method: KeepaliveMethod,
    pub interval: Duration,
    pub response_timeout: Duration,
    pub pending_sequence: Option<u32>,
    pub writing: bool,
}
#[derive(Clone, Debug)]
pub enum KeepaliveEventKind {
    Sent,
    Response { status: i32 },
    Rejected { error: Error },
    SessionEnded,
}
#[derive(Clone, Debug)]
pub struct KeepaliveEvent {
    pub session: String,
    pub sequence: Option<u32>,
    pub kind: KeepaliveEventKind,
}
#[derive(Clone, Debug)]
pub struct KeepaliveFailure {
    pub session: String,
    pub dispatch: Dispatch,
    pub error: Error,
}
struct Scheduled {
    options: Keepalive,
    next: Instant,
    cancel: KeepaliveCancellation,
}
struct Pending {
    session: String,
    deadline: Instant,
    dispatch: Dispatch,
    writing: bool,
}
#[derive(Default)]
pub(super) struct State {
    schedules: BTreeMap<String, Scheduled>,
    pending: Option<Pending>,
    event: Option<KeepaliveEvent>,
}
unsafe extern "C" {
    fn gst_runtime_rtsp_request_ready(client: *mut c_void) -> i32;
    fn gst_runtime_rtsp_session_active(client: *mut c_void, session: *const c_char) -> i32;
}
fn active(native: &Rtsp, session: &str) -> Result<bool, Error> {
    let session = text(session)?;
    Ok(unsafe { gst_runtime_rtsp_session_active(native.inner.0.as_ptr(), session.as_ptr()) } != 0)
}
impl Rtsp {
    pub fn configure_keepalive(
        &mut self,
        session: &str,
        options: Option<Keepalive>,
    ) -> Result<Option<KeepaliveCancellation>, Error> {
        if options.is_none() {
            if let Some(value) = self.keepalives.schedules.get(session) {
                value.cancel.cancel();
            }
            return Ok(None);
        }
        if session.is_empty()
            || !active(self, session)?
            || self
                .keepalives
                .pending
                .as_ref()
                .is_some_and(|p| p.session == session)
        {
            return Err(Error::INVALID);
        }
        let options = options.unwrap();
        text(&options.uri)?;
        if options.uri.is_empty()
            || options.interval.is_zero()
            || options.response_timeout.is_zero()
        {
            return Err(Error::INVALID);
        }
        let now = Instant::now();
        let next = now.checked_add(options.interval).ok_or(Error::INVALID)?;
        now.checked_add(options.response_timeout)
            .ok_or(Error::INVALID)?;
        let cancel = KeepaliveCancellation(Arc::new(AtomicBool::new(false)));
        if let Some(old) = self.keepalives.schedules.insert(
            session.into(),
            Scheduled {
                options,
                next,
                cancel: cancel.clone(),
            },
        ) {
            old.cancel.cancel();
        }
        Ok(Some(cancel))
    }
    pub fn keepalive_status(&self, session: &str) -> Option<KeepaliveStatus> {
        let value = self.keepalives.schedules.get(session)?;
        let pending = self
            .keepalives
            .pending
            .as_ref()
            .filter(|p| p.session == session);
        Some(KeepaliveStatus {
            enabled: !value.cancel.cancelled(),
            uri: value.options.uri.clone(),
            method: value.options.method,
            interval: value.options.interval,
            response_timeout: value.options.response_timeout,
            pending_sequence: pending.map(|p| p.dispatch.sequence),
            writing: pending.is_some_and(|p| p.writing),
        })
    }
    pub fn keepalive_pending(&self) -> bool {
        self.keepalives.pending.is_some()
    }
    pub fn keepalive_writing(&self) -> bool {
        self.keepalives.pending.as_ref().is_some_and(|p| p.writing)
    }
    /// Drive only explicitly configured maintenance. Responses still leave through
    /// receive_step with their original headers, body and authentication evidence.
    pub fn keepalive_step(&mut self) -> Result<Option<KeepaliveEvent>, KeepaliveFailure> {
        let mut state = std::mem::take(&mut self.keepalives);
        let result = state.step(self);
        self.keepalives = state;
        result
    }
}
impl State {
    pub(super) fn observe(&mut self, complete: bool, message: &Message) {
        // The C engine already correlated this response to its sole outstanding
        // request. Do not reparse CSeq or maintain a second response matcher here.
        if !complete || message.kind != 2 || message.status < 200 {
            return;
        }
        let Some(pending) = self.pending.take() else {
            return;
        };
        if let Some(schedule) = self.schedules.get_mut(&pending.session) {
            if (200..300).contains(&message.status) {
                if let Some(next) = Instant::now().checked_add(schedule.options.interval) {
                    schedule.next = next;
                } else {
                    schedule.cancel.cancel();
                }
            } else {
                schedule.cancel.cancel();
            }
        }
        self.event = Some(KeepaliveEvent {
            session: pending.session,
            sequence: Some(pending.dispatch.sequence),
            kind: KeepaliveEventKind::Response {
                status: message.status,
            },
        });
    }
    fn step(&mut self, native: &mut Rtsp) -> Result<Option<KeepaliveEvent>, KeepaliveFailure> {
        if let Some(event) = self.event.take() {
            return Ok(Some(event));
        }
        if let Some(mut pending) = self.pending.take() {
            let failure = if Instant::now() >= pending.deadline {
                Some(Error::TIMEOUT)
            } else if pending.writing {
                let (result, dispatch) = native.write_step();
                pending.dispatch.may_have_been_sent |= dispatch.may_have_been_sent;
                match result {
                    Ok(true) => {
                        pending.writing = false;
                        let event = KeepaliveEvent {
                            session: pending.session.clone(),
                            sequence: Some(pending.dispatch.sequence),
                            kind: KeepaliveEventKind::Sent,
                        };
                        self.pending = Some(pending);
                        return Ok(Some(event));
                    }
                    Ok(false) => None,
                    Err(error) => Some(error),
                }
            } else {
                None
            };
            if let Some(error) = failure {
                native.cancellation().cancel();
                if let Some(schedule) = self.schedules.get(&pending.session) {
                    schedule.cancel.cancel();
                }
                return Err(KeepaliveFailure {
                    session: pending.session,
                    dispatch: pending.dispatch,
                    error,
                });
            }
            self.pending = Some(pending);
            return Ok(None);
        }
        // A cancelled declaration never starts another request. Retire it only
        // after its already-dispatched response has been observed or timed out.
        self.schedules.retain(|_, value| !value.cancel.cancelled());
        if unsafe { gst_runtime_rtsp_request_ready(native.inner.0.as_ptr()) } == 0 {
            return Ok(None);
        }
        let now = Instant::now();
        let Some(session) = self
            .schedules
            .iter()
            .filter(|(_, v)| v.next <= now)
            .min_by_key(|(_, v)| v.next)
            .map(|(k, _)| k.clone())
        else {
            return Ok(None);
        };
        if !active(native, &session).unwrap_or(false) {
            self.schedules.remove(&session);
            return Ok(Some(KeepaliveEvent {
                session,
                sequence: None,
                kind: KeepaliveEventKind::SessionEnded,
            }));
        }
        let schedule = self.schedules.get(&session).unwrap();
        let Some(deadline) = now.checked_add(schedule.options.response_timeout) else {
            schedule.cancel.cancel();
            return Ok(Some(KeepaliveEvent {
                session,
                sequence: None,
                kind: KeepaliveEventKind::Rejected {
                    error: Error::INVALID,
                },
            }));
        };
        let headers = [("Session", session.as_str())];
        let (result, dispatch) = match &schedule.options.authentication {
            Some(auth) => native.request_authenticated_begin(
                schedule.options.method.name(),
                &schedule.options.uri,
                &headers,
                &[],
                &auth.challenge,
                auth.qop,
            ),
            None => native.request_begin(
                schedule.options.method.name(),
                &schedule.options.uri,
                &headers,
                &[],
            ),
        };
        match result {
            Ok(true) => {
                self.pending = Some(Pending {
                    session,
                    deadline,
                    dispatch,
                    writing: true,
                });
                self.step(native)
            }
            Ok(false) => Ok(None),
            Err(error) => {
                schedule.cancel.cancel();
                if dispatch.may_have_been_sent {
                    native.cancellation().cancel();
                    Err(KeepaliveFailure {
                        session,
                        dispatch,
                        error,
                    })
                } else {
                    Ok(Some(KeepaliveEvent {
                        session,
                        sequence: None,
                        kind: KeepaliveEventKind::Rejected { error },
                    }))
                }
            }
        }
    }
}
