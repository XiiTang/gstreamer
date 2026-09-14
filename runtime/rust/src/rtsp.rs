use crate::{Error, ffi, initialize};
use std::{
    cell::Cell,
    ffi::{CStr, CString, c_void},
    marker::PhantomData,
    ptr::NonNull,
    sync::Arc,
    time::Duration,
};
use zeroize::{Zeroize, Zeroizing};

#[derive(Clone, Copy, Debug)]
pub enum Version {
    V1,
    V2,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TrackState {
    Ready,
    Playing,
    Recording,
    Closed,
    Unknown,
}
#[derive(Clone, Copy, Debug, Default)]
pub struct Dispatch {
    pub sequence: u32,
    pub may_have_been_sent: bool,
}
#[derive(Debug)]
pub struct Message {
    pub kind: i32,
    pub status: i32,
    pub channel: i32,
    pub version: i32,
    pub method: Option<Vec<u8>>,
    pub uri: Option<Vec<u8>>,
    pub reason: Option<Vec<u8>>,
    pub headers: Vec<(Vec<u8>, Vec<u8>)>,
    pub body: Vec<u8>,
    pub raw: Vec<u8>,
    pub authentication: Option<crate::rtsp_auth::Observation>,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Address {
    pub host: String,
    pub port: u16,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionInfo {
    pub timeout_seconds: u64,
    pub timeout_explicit: bool,
    pub control_response_age: Duration,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Transport {
    pub generation: u64,
    pub profile: &'static str,
    pub lower_transport: &'static str,
    pub play: bool,
    pub record: bool,
    pub rtcp_mux: bool,
    pub interleaved: Option<(u8, Option<u8>)>,
    pub client_ports: Option<(u16, Option<u16>)>,
    pub server_ports: Option<(u16, Option<u16>)>,
    pub source: Option<String>,
    pub destination: Option<String>,
    pub source_addresses: Vec<Address>,
    pub destination_addresses: Vec<Address>,
    pub ssrcs: Vec<u32>,
}
struct Inner(NonNull<c_void>);
// The native client is movable while quiescent. Only its cancellation function
// is called through shared handles; all other access requires &mut Rtsp.
unsafe impl Send for Inner {}
unsafe impl Sync for Inner {}
impl Drop for Inner {
    fn drop(&mut self) {
        unsafe { ffi::gst_runtime_rtsp_free(self.0.as_ptr()) };
    }
}
pub struct Rtsp {
    inner: Arc<Inner>,
    version: Version,
    authentication: Option<crate::rtsp_auth::Auth>,
    _exclusive: PhantomData<Cell<()>>,
}
#[derive(Clone)]
pub struct Cancellation(Arc<Inner>);
impl Cancellation {
    pub fn cancel(&self) {
        unsafe { ffi::gst_runtime_rtsp_cancel(self.0.0.as_ptr()) };
    }
}
fn text(value: &str) -> Result<CString, Error> {
    CString::new(value).map_err(|_| Error::INVALID)
}
fn timeout(value: Duration) -> i64 {
    value.as_micros().clamp(1, i64::MAX as u128) as i64
}
struct Headers {
    _strings: Vec<(CString, Zeroizing<Vec<u8>>)>,
    values: Vec<ffi::Header>,
}
impl Headers {
    fn new(values: &[(&str, &str)]) -> Result<Self, Error> {
        let strings = values
            .iter()
            .map(|(k, v)| Ok((text(k)?, Zeroizing::new(text(v)?.into_bytes_with_nul()))))
            .collect::<Result<Vec<_>, Error>>()?;
        let values = strings
            .iter()
            .map(|(k, v)| ffi::Header {
                name: k.as_ptr(),
                value: v.as_ptr().cast(),
            })
            .collect();
        Ok(Self {
            _strings: strings,
            values,
        })
    }
}
impl Rtsp {
    #[cfg(unix)]
    pub fn from_stream(
        stream: std::os::fd::OwnedFd,
        uri: &str,
        version: Version,
        body_limit: u32,
    ) -> Result<Self, Error> {
        use std::os::fd::AsRawFd;
        initialize();
        let uri = text(uri)?;
        let mut output = std::ptr::null_mut();
        let mut taken = 0;
        let result = unsafe {
            ffi::gst_runtime_rtsp_new(
                uri.as_ptr(),
                stream.as_raw_fd(),
                match version {
                    Version::V1 => 1,
                    Version::V2 => 2,
                },
                body_limit,
                &mut output,
                &mut taken,
            )
        };
        if taken != 0 {
            std::mem::forget(stream);
        }
        Error::check(result)?;
        Ok(Self {
            inner: Arc::new(Inner(NonNull::new(output).ok_or(Error::INVALID)?)),
            version,
            authentication: None,
            _exclusive: PhantomData,
        })
    }
    #[cfg(windows)]
    pub fn from_socket(
        stream: std::os::windows::io::OwnedSocket,
        uri: &str,
        version: Version,
        body_limit: u32,
    ) -> Result<Self, Error> {
        use std::os::windows::io::AsRawSocket;
        initialize();
        let uri = text(uri)?;
        let fd = i32::try_from(stream.as_raw_socket()).map_err(|_| Error::INVALID)?;
        let mut output = std::ptr::null_mut();
        let mut taken = 0;
        let result = unsafe {
            ffi::gst_runtime_rtsp_new(
                uri.as_ptr(),
                fd,
                match version {
                    Version::V1 => 1,
                    Version::V2 => 2,
                },
                body_limit,
                &mut output,
                &mut taken,
            )
        };
        if taken != 0 {
            std::mem::forget(stream);
        }
        Error::check(result)?;
        Ok(Self {
            inner: Arc::new(Inner(NonNull::new(output).ok_or(Error::INVALID)?)),
            version,
            authentication: None,
            _exclusive: PhantomData,
        })
    }
    pub fn configure_authentication(
        &mut self,
        username: Zeroizing<String>,
        password: Zeroizing<String>,
        policy: crate::rtsp_auth::Policy,
        tls: bool,
    ) -> Result<(), Error> {
        if self.authentication.is_some() {
            return Err(Error::AUTHENTICATION);
        }
        self.authentication = Some(crate::rtsp_auth::Auth::new(
            username,
            password,
            policy,
            tls || matches!(self.version, Version::V1),
        )?);
        Ok(())
    }
    pub fn request_authenticated_begin(
        &mut self,
        method: &str,
        uri: &str,
        headers: &[(&str, &str)],
        body: &[u8],
        challenge: &str,
        qop: crate::rtsp_auth::Qop,
    ) -> (Result<bool, Error>, Dispatch) {
        let prepared = match self
            .authentication
            .as_mut()
            .ok_or(Error::AUTHENTICATION)
            .and_then(|auth| auth.prepare(challenge, qop, method, uri, body))
        {
            Ok(prepared) => prepared,
            Err(error) => return (Err(error), Dispatch::default()),
        };
        let mut headers = headers.to_vec();
        headers.push(("Authorization", &prepared.header));
        let result = self.request_begin(method, uri, &headers, body);
        if matches!(result.0, Ok(true)) {
            self.authentication.as_mut().unwrap().dispatched(prepared);
        }
        result
    }
    fn authenticate_received(
        &mut self,
        valid: bool,
        complete: bool,
        message: &mut Message,
    ) -> Result<(), Error> {
        let Some(auth) = self.authentication.as_mut() else {
            return Ok(());
        };
        if !valid {
            message.raw.zeroize();
            message.raw.clear();
            for (name, value) in &mut message.headers {
                if name.eq_ignore_ascii_case(b"www-authenticate")
                    || name.eq_ignore_ascii_case(b"authentication-info")
                    || name.eq_ignore_ascii_case(b"proxy-authenticate")
                    || name.eq_ignore_ascii_case(b"proxy-authentication-info")
                {
                    value.zeroize();
                    value.clear();
                }
            }
            message.authentication = Some(crate::rtsp_auth::Observation {
                protected_headers: true,
                ..Default::default()
            });
        } else if complete && message.kind == 2 {
            match auth.response(
                message.status,
                &mut message.headers,
                &message.body,
                &mut message.raw,
            ) {
                Ok(observation) => message.authentication = Some(observation),
                Err(error) => {
                    message.authentication = Some(crate::rtsp_auth::Observation {
                        protected_headers: true,
                        server_proof: Some(false),
                        ..Default::default()
                    });
                    unsafe { ffi::gst_runtime_rtsp_invalidate(self.inner.0.as_ptr()) };
                    return Err(error);
                }
            }
        }
        Ok(())
    }
    pub fn cancellation(&self) -> Cancellation {
        Cancellation(self.inner.clone())
    }
    /// False means the single native writer is occupied; no admission occurred.
    pub fn request_begin(
        &mut self,
        method: &str,
        uri: &str,
        headers: &[(&str, &str)],
        body: &[u8],
    ) -> (Result<bool, Error>, Dispatch) {
        let mut dispatch = Dispatch::default();
        let result = (|| {
            let method = text(method)?;
            let uri = text(uri)?;
            let headers = Headers::new(headers)?;
            let mut stage = 0;
            let code = unsafe {
                ffi::gst_runtime_rtsp_request_begin(
                    self.inner.0.as_ptr(),
                    method.as_ptr(),
                    uri.as_ptr(),
                    headers.values.as_ptr(),
                    headers.values.len(),
                    body.as_ptr(),
                    body.len(),
                    &mut dispatch.sequence,
                    &mut stage,
                )
            };
            dispatch.may_have_been_sent = stage != 0;
            if code == 1 {
                Ok(false)
            } else {
                Error::check(code).map(|_| true)
            }
        })();
        (result, dispatch)
    }
    pub fn respond_begin(
        &mut self,
        status: u16,
        reason: &str,
        headers: &[(&str, &str)],
        body: &[u8],
    ) -> Result<bool, Error> {
        let reason = text(reason)?;
        let headers = Headers::new(headers)?;
        let code = unsafe {
            ffi::gst_runtime_rtsp_respond_begin(
                self.inner.0.as_ptr(),
                status.into(),
                reason.as_ptr(),
                headers.values.as_ptr(),
                headers.values.len(),
                body.as_ptr(),
                body.len(),
            )
        };
        if code == 1 {
            Ok(false)
        } else {
            Error::check(code).map(|_| true)
        }
    }
    /// True means the owned write completed. Partial wire progress is never replayed.
    pub fn write_step(&mut self) -> (Result<bool, Error>, Dispatch) {
        let mut dispatch = Dispatch::default();
        let mut stage = 0;
        let code = unsafe {
            ffi::gst_runtime_rtsp_write_step(
                self.inner.0.as_ptr(),
                &mut dispatch.sequence,
                &mut stage,
            )
        };
        dispatch.may_have_been_sent = stage != 0;
        (
            if code == 1 {
                Ok(false)
            } else {
                Error::check(code).map(|_| true)
            },
            dispatch,
        )
    }
    pub fn send_data_begin(&mut self, channel: u8, bytes: &[u8]) -> Result<bool, Error> {
        let code = unsafe {
            ffi::gst_runtime_rtsp_send_data_begin(
                self.inner.0.as_ptr(),
                channel,
                bytes.as_ptr(),
                bytes.len(),
            )
        };
        if code == 1 {
            Ok(false)
        } else {
            Error::check(code).map(|_| true)
        }
    }
    pub fn request(
        &mut self,
        method: &str,
        uri: &str,
        headers: &[(&str, &str)],
        body: &[u8],
        limit: Duration,
    ) -> (Result<(), Error>, Dispatch) {
        let mut dispatch = Dispatch::default();
        let result = (|| {
            let method = text(method)?;
            let uri = text(uri)?;
            let headers = Headers::new(headers)?;
            let mut native_dispatch = 0;
            let code = unsafe {
                ffi::gst_runtime_rtsp_request(
                    self.inner.0.as_ptr(),
                    method.as_ptr(),
                    uri.as_ptr(),
                    headers.values.as_ptr(),
                    headers.values.len(),
                    body.as_ptr(),
                    body.len(),
                    timeout(limit),
                    &mut dispatch.sequence,
                    &mut native_dispatch,
                )
            };
            dispatch.may_have_been_sent = native_dispatch != 0;
            Error::check(code)
        })();
        (result, dispatch)
    }
    pub fn respond(
        &mut self,
        status: u16,
        reason: &str,
        headers: &[(&str, &str)],
        body: &[u8],
        limit: Duration,
    ) -> Result<(), Error> {
        let reason = text(reason)?;
        let headers = Headers::new(headers)?;
        Error::check(unsafe {
            ffi::gst_runtime_rtsp_respond(
                self.inner.0.as_ptr(),
                status.into(),
                reason.as_ptr(),
                headers.values.as_ptr(),
                headers.values.len(),
                body.as_ptr(),
                body.len(),
                timeout(limit),
            )
        })
    }
    pub fn wait(&mut self, limit: Duration) -> Result<bool, Error> {
        match Error::check(unsafe {
            ffi::gst_runtime_rtsp_wait(self.inner.0.as_ptr(), timeout(limit))
        }) {
            Ok(()) => Ok(true),
            Err(Error::TIMEOUT) => Ok(false),
            Err(error) => Err(error),
        }
    }
    pub fn receive(&mut self, limit: Duration) -> (Result<(), Error>, Message) {
        let mut message = std::ptr::null_mut();
        let result = Error::check(unsafe {
            ffi::gst_runtime_rtsp_receive(self.inner.0.as_ptr(), timeout(limit), &mut message)
        });
        // The native ABI always owns a result message, including failed reads.
        let message = NativeMessage(NonNull::new(message).expect("Native RTSP message contract"));
        let mut message = message.copy();
        let result = self
            .authenticate_received(result.is_ok(), true, &mut message)
            .and(result);
        (result, message)
    }
    /// False means a retained partial message. No partial event is published;
    /// the next complete message or terminal error includes its exact raw bytes.
    pub fn receive_step(&mut self) -> (Result<bool, Error>, Message) {
        let mut message = std::ptr::null_mut();
        let code =
            unsafe { ffi::gst_runtime_rtsp_receive_step(self.inner.0.as_ptr(), &mut message) };
        let result = if code == 1 {
            Ok(false)
        } else {
            Error::check(code).map(|_| true)
        };
        let message = NativeMessage(NonNull::new(message).expect("Native RTSP message contract"));
        let mut message = message.copy();
        let result = self
            .authenticate_received(result.is_ok(), matches!(result, Ok(true)), &mut message)
            .and(result);
        (result, message)
    }
    pub fn transport(&mut self, session: &str, uri: &str) -> Result<Option<Transport>, Error> {
        let session = text(session)?;
        let uri = text(uri)?;
        let mut view = ffi::TransportView::default();
        let result = unsafe {
            ffi::gst_runtime_rtsp_transport(
                self.inner.0.as_ptr(),
                session.as_ptr(),
                uri.as_ptr(),
                &mut view,
            )
        };
        if result == 1 {
            return Ok(None);
        }
        Error::check(result)?;
        let string = |ptr: *const std::ffi::c_char| -> Result<Option<String>, Error> {
            if ptr.is_null() {
                Ok(None)
            } else {
                unsafe { CStr::from_ptr(ptr) }
                    .to_str()
                    .map(|v| Some(v.to_owned()))
                    .map_err(|_| Error::INVALID)
            }
        };
        let addresses = |hosts: [*const std::ffi::c_char; 2],
                         ports: [u32; 2],
                         count: u32|
         -> Result<Vec<Address>, Error> {
            if count > 2 {
                return Err(Error::INVALID);
            }
            (0..count as usize)
                .map(|i| {
                    Ok(Address {
                        host: string(hosts[i])?.ok_or(Error::INVALID)?,
                        port: u16::try_from(ports[i]).map_err(|_| Error::INVALID)?,
                    })
                })
                .collect()
        };
        let range = |first: i32, last: i32| -> Result<Option<(u16, Option<u16>)>, Error> {
            if first == -1 {
                return Ok(None);
            }
            Ok(Some((
                u16::try_from(first).map_err(|_| Error::INVALID)?,
                if last == -1 {
                    None
                } else {
                    Some(u16::try_from(last).map_err(|_| Error::INVALID)?)
                },
            )))
        };
        let channels = range(view.interleaved_first, view.interleaved_last)?
            .map(|(a, b)| {
                Ok((
                    u8::try_from(a).map_err(|_| Error::INVALID)?,
                    b.map(u8::try_from)
                        .transpose()
                        .map_err(|_| Error::INVALID)?,
                ))
            })
            .transpose()?;
        let ssrcs = if view.ssrc_count == 0 {
            Vec::new()
        } else {
            if view.ssrcs.is_null() {
                return Err(Error::INVALID);
            }
            unsafe { std::slice::from_raw_parts(view.ssrcs, view.ssrc_count as usize) }.to_vec()
        };
        Ok(Some(Transport {
            generation: view.generation,
            profile: match view.profile {
                1 => "AVP",
                2 => "SAVP",
                4 => "AVPF",
                8 => "SAVPF",
                _ => return Err(Error::INVALID),
            },
            lower_transport: match view.lower_transport {
                1 => "udp",
                2 => "multicast",
                4 => "tcp",
                _ => return Err(Error::INVALID),
            },
            play: view.mode_play != 0,
            record: view.mode_record != 0,
            rtcp_mux: view.rtcp_mux != 0,
            interleaved: channels,
            client_ports: range(view.client_first, view.client_last)?,
            server_ports: range(view.server_first, view.server_last)?,
            source: string(view.source)?,
            destination: string(view.destination)?,
            source_addresses: addresses(view.src_host, view.src_port, view.src_count)?,
            destination_addresses: addresses(view.dest_host, view.dest_port, view.dest_count)?,
            ssrcs,
        }))
    }
    pub fn session_info(&mut self, session: &str) -> Result<Option<SessionInfo>, Error> {
        let session = CString::new(session).map_err(|_| Error::INVALID)?;
        let mut seconds = 0;
        let mut explicit = 0;
        let mut age = 0;
        let result = unsafe {
            ffi::gst_runtime_rtsp_session_info(
                self.inner.0.as_ptr(),
                session.as_ptr(),
                &mut seconds,
                &mut explicit,
                &mut age,
            )
        };
        match result {
            0 => Ok(Some(SessionInfo {
                timeout_seconds: seconds,
                timeout_explicit: explicit != 0,
                control_response_age: Duration::from_micros(age),
            })),
            1 => Ok(None),
            value => Err(Error(value)),
        }
    }
    pub fn state(&mut self, session: &str, uri: &str) -> Result<Option<TrackState>, Error> {
        let session = text(session)?;
        let uri = text(uri)?;
        Ok(
            match unsafe {
                ffi::gst_runtime_rtsp_state(self.inner.0.as_ptr(), session.as_ptr(), uri.as_ptr())
            } {
                0 => Some(TrackState::Ready),
                1 => Some(TrackState::Playing),
                2 => Some(TrackState::Recording),
                3 => Some(TrackState::Closed),
                4 => Some(TrackState::Unknown),
                _ => None,
            },
        )
    }
}
struct NativeMessage(NonNull<c_void>);
impl Drop for NativeMessage {
    fn drop(&mut self) {
        unsafe { ffi::gst_runtime_rtsp_message_free(self.0.as_ptr()) };
    }
}
impl NativeMessage {
    fn copy(&self) -> Message {
        let mut view = ffi::MessageView::default();
        unsafe { ffi::gst_runtime_rtsp_message_view(self.0.as_ptr(), &mut view) };
        let string = |ptr| {
            if ptr == std::ptr::null() {
                None
            } else {
                Some(unsafe { CStr::from_ptr(ptr) }.to_bytes().to_vec())
            }
        };
        let bytes = |ptr, length| {
            if length == 0 {
                Vec::new()
            } else {
                unsafe { std::slice::from_raw_parts(ptr, length) }.to_vec()
            }
        };
        let mut headers = Vec::new();
        for index in 0..u32::MAX {
            let mut header = ffi::Header {
                name: std::ptr::null(),
                value: std::ptr::null(),
            };
            if unsafe { ffi::gst_runtime_rtsp_message_header(self.0.as_ptr(), index, &mut header) }
                == 0
            {
                break;
            }
            headers.push((
                string(header.name).unwrap_or_default(),
                string(header.value).unwrap_or_default(),
            ));
        }
        Message {
            kind: view.kind,
            status: view.status,
            channel: view.channel,
            version: view.version,
            method: string(view.method),
            uri: string(view.uri),
            reason: string(view.reason),
            headers,
            body: bytes(view.body, view.body_length),
            raw: bytes(view.raw, view.raw_length),
            authentication: None,
        }
    }
}
