//! What the RTSP owner sends itself, on the same exclusive owner: declared
//! session keepalives, and a request sent again with the answer to its 401.
//! The caller polls this with receive_step; no thread, socket or alternate
//! identity.
use super::{Credential, Dispatch, Error, Message, Rtsp, text};
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
pub struct Keepalive {
    pub uri: String,
    pub method: KeepaliveMethod,
    pub interval: Duration,
    /// Bounds preparation, write and response from the first eligible due selection.
    pub response_timeout: Duration,
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
/// One selected maintenance exchange. Clones identify the same configuration
/// and scheduled cycle; pending polls keep its original absolute deadline.
#[derive(Clone)]
pub struct KeepaliveDue {
    session: String,
    next: Instant,
    deadline: Instant,
    cancel: KeepaliveCancellation,
}
impl std::fmt::Debug for KeepaliveDue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("KeepaliveDue")
            .field("session", &self.session)
            .field("next", &self.next)
            .field("deadline", &self.deadline)
            .field("cancelled", &self.is_cancelled())
            .finish()
    }
}
impl PartialEq for KeepaliveDue {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.cancel.0, &other.cancel.0) && self.next == other.next
    }
}
impl Eq for KeepaliveDue {}
impl KeepaliveDue {
    pub fn session(&self) -> &str {
        &self.session
    }
    pub fn is_cancelled(&self) -> bool {
        self.cancel.cancelled()
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
/// A request the owner sent itself failed, and the client with it: a
/// keepalive for `session`, or with none a request sent again with an answer.
#[derive(Clone, Debug)]
pub struct MaintenanceFailure {
    pub session: Option<String>,
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
    selected: Option<KeepaliveDue>,
    #[cfg(test)]
    clock: Option<Arc<std::sync::Mutex<Instant>>>,
    event: Option<KeepaliveEvent>,
}
unsafe extern "C" {
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
        let now = self.keepalives.now();
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
    /// A request the owner sends itself is due, being written or awaiting its
    /// response: the caller's own requests wait.
    pub fn maintenance_pending(&self) -> bool {
        self.keepalives.selected.is_some()
            || self.keepalives.pending.is_some()
            || self.authentication.as_ref().is_some_and(|auth| auth.busy())
    }
    /// A request the owner sends itself is being written: other writes wait.
    pub fn maintenance_writing(&self) -> bool {
        self.keepalives.pending.as_ref().is_some_and(|p| p.writing)
            || self
                .authentication
                .as_ref()
                .is_some_and(|auth| auth.writing)
    }
    /// Drives what the owner sends itself: a request sent again with an
    /// answer, then declared keepalives. Responses still leave through
    /// receive_step with their original headers, body and authentication
    /// evidence.
    /// `prepare` is called only for callback authentication, after native
    /// admission is available. It must not block: None keeps this exchange
    /// pending, while a ready result supplies or rejects only this exchange.
    /// Cancellation before final admission sends nothing. Preparation consumes
    /// the same original response allowance as the admitted write and response.
    pub fn maintain(
        &mut self,
        mut prepare: impl FnMut(&KeepaliveDue) -> Option<Result<Credential, Error>>,
    ) -> Result<Option<KeepaliveEvent>, MaintenanceFailure> {
        self.answer_step()
            .map_err(|(dispatch, error)| MaintenanceFailure {
                session: None,
                dispatch,
                error,
            })?;
        let mut state = std::mem::take(&mut self.keepalives);
        let result = state.step(self, &mut prepare);
        self.keepalives = state;
        result
    }
}
impl State {
    fn now(&self) -> Instant {
        #[cfg(test)]
        if let Some(clock) = &self.clock {
            return *clock.lock().unwrap();
        }
        Instant::now()
    }
    pub(super) fn observe(&mut self, complete: bool, message: &Message) {
        // The C engine already correlated this response to its sole outstanding
        // request. Do not reparse CSeq or maintain a second response matcher here.
        if !complete || message.kind != 2 || message.status < 200 {
            return;
        }
        // A 401 answered by sending the keepalive again: its response is the
        // one that counts.
        if let Some(sequence) = message
            .authentication
            .as_ref()
            .and_then(|authentication| authentication.answered_by)
        {
            if let Some(pending) = self.pending.as_mut() {
                pending.dispatch.sequence = sequence;
            }
            return;
        }
        let Some(pending) = self.pending.take() else {
            return;
        };
        let now = self.now();
        if let Some(schedule) = self.schedules.get_mut(&pending.session) {
            if (200..300).contains(&message.status) {
                if let Some(next) = now.checked_add(schedule.options.interval) {
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
    fn step(
        &mut self,
        native: &mut Rtsp,
        prepare: &mut impl FnMut(&KeepaliveDue) -> Option<Result<Credential, Error>>,
    ) -> Result<Option<KeepaliveEvent>, MaintenanceFailure> {
        if let Some(event) = self.event.take() {
            return Ok(Some(event));
        }
        if let Some(mut pending) = self.pending.take() {
            let failure = if self.now() >= pending.deadline {
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
                unsafe { crate::ffi::gst_runtime_rtsp_invalidate(native.inner.0.as_ptr()) };
                if let Some(schedule) = self.schedules.get(&pending.session) {
                    schedule.cancel.cancel();
                }
                return Err(MaintenanceFailure {
                    session: Some(pending.session),
                    dispatch: pending.dispatch,
                    error,
                });
            }
            self.pending = Some(pending);
            return Ok(None);
        }
        // Cancellation cannot retire admitted work before its response/deadline.
        self.schedules.retain(|_, value| !value.cancel.cancelled());
        if let Some(event) = self.check_selected(native) {
            return Ok(Some(event));
        }
        if unsafe { crate::ffi::gst_runtime_rtsp_request_ready(native.inner.0.as_ptr()) } == 0 {
            return Ok(None);
        }
        if self.selected.is_none() {
            let now = self.now();
            let Some((session, schedule)) = self
                .schedules
                .iter()
                .filter(|(_, schedule)| schedule.next <= now)
                .min_by_key(|(_, schedule)| schedule.next)
            else {
                return Ok(None);
            };
            let Some(deadline) = now.checked_add(schedule.options.response_timeout) else {
                schedule.cancel.cancel();
                return Ok(Some(KeepaliveEvent {
                    session: session.clone(),
                    sequence: None,
                    kind: KeepaliveEventKind::Rejected {
                        error: Error::INVALID,
                    },
                }));
            };
            self.selected = Some(KeepaliveDue {
                session: session.clone(),
                next: schedule.next,
                deadline,
                cancel: schedule.cancel.clone(),
            });
        }
        if let Some(event) = self.check_selected(native) {
            return Ok(Some(event));
        }
        let Some(due) = self.selected.clone() else {
            return Ok(None);
        };
        // Exclusive native ownership keeps this admission available through
        // the short supplier call; no native writer is held by preparation.
        match native.authentication_ready() {
            Ok(true) => {}
            Ok(false) => return Ok(None),
            Err(error) => return Ok(Some(self.reject_selected(error))),
        }
        let supplied = if native
            .authentication
            .as_ref()
            .is_some_and(|auth| auth.acquires())
        {
            let Some(result) = prepare(&due) else {
                return Ok(None);
            };
            if let Some(event) = self.check_selected(native) {
                return Ok(Some(event));
            }
            if self.selected.is_none() {
                return Ok(None);
            }
            match result {
                Ok(credential) => Some(credential),
                Err(error) => return Ok(Some(self.reject_selected(error))),
            }
        } else {
            None
        };
        if let Some(event) = self.check_selected(native) {
            return Ok(Some(event));
        }
        if self.selected.is_none() {
            return Ok(None);
        }
        let schedule = self.schedules.get(due.session()).unwrap();
        let headers = [("Session", due.session())];
        let (result, dispatch) = native.request_begin_with(
            schedule.options.method.name(),
            &schedule.options.uri,
            &headers,
            &[],
            supplied,
        );
        match result {
            Ok(true) => {
                self.selected = None;
                self.pending = Some(Pending {
                    session: due.session,
                    deadline: due.deadline,
                    dispatch,
                    writing: true,
                });
                self.step(native, prepare)
            }
            Ok(false) => Ok(None),
            Err(error) => {
                if dispatch.may_have_been_sent {
                    self.selected.take().unwrap().cancel.cancel();
                    unsafe { crate::ffi::gst_runtime_rtsp_invalidate(native.inner.0.as_ptr()) };
                    Err(MaintenanceFailure {
                        session: Some(due.session),
                        dispatch,
                        error,
                    })
                } else {
                    Ok(Some(self.reject_selected(error)))
                }
            }
        }
    }
    fn reject_selected(&mut self, error: Error) -> KeepaliveEvent {
        let due = self.selected.take().expect("selected maintenance exchange");
        due.cancel.cancel();
        KeepaliveEvent {
            session: due.session,
            sequence: None,
            kind: KeepaliveEventKind::Rejected { error },
        }
    }
    /// Validate again after readiness, immediately before native admission.
    fn check_selected(&mut self, native: &Rtsp) -> Option<KeepaliveEvent> {
        let due = self.selected.as_ref()?;
        let current = self.schedules.get(due.session()).is_some_and(|schedule| {
            Arc::ptr_eq(&schedule.cancel.0, &due.cancel.0) && schedule.next == due.next
        });
        if !current || due.is_cancelled() {
            self.selected = None;
            return None;
        }
        if !active(native, due.session()).unwrap_or(false) {
            let due = self.selected.take().unwrap();
            due.cancel.cancel();
            self.schedules.remove(due.session());
            return Some(KeepaliveEvent {
                session: due.session,
                sequence: None,
                kind: KeepaliveEventKind::SessionEnded,
            });
        }
        if self.now() >= due.deadline {
            return Some(self.reject_selected(Error::TIMEOUT));
        }
        None
    }
}

#[cfg(all(test, unix))]
mod tests;
