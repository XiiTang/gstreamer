//! Exclusive opaque SRTP context. Native libSRTP owns all packet/index/replay
//! logic. The caller must persist `export()` after processing and before making
//! any packet visible. Restoring state is not reconnecting an Execution.
use std::{cell::Cell, ffi::c_void, marker::PhantomData, ptr::NonNull};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum Profile {
    AesCm128HmacSha1_80 = 1,
    AeadAes128Gcm = 2,
    AeadAes256Gcm = 3,
}
impl Profile {
    pub fn key_length(self) -> usize {
        match self {
            Self::AesCm128HmacSha1_80 => 30,
            Self::AeadAes128Gcm => 28,
            Self::AeadAes256Gcm => 44,
        }
    }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum Direction {
    Send = 1,
    Receive = 2,
}
#[derive(Clone, Copy)]
pub struct Configuration<'a> {
    pub profile: Profile,
    pub direction: Direction,
    pub key: &'a [u8],
    pub replay_window: u32,
    pub encrypted_extensions: &'a [u32],
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Error(pub i32);
impl Error {
    pub const INVALID: Self = Self(2);
    pub const REPLAY: Self = Self(9);
    pub const TOO_OLD: Self = Self(10);
    pub const KEY_EXPIRED: Self = Self(15);
    fn check(value: i32) -> Result<(), Self> {
        if value == 0 { Ok(()) } else { Err(Self(value)) }
    }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Native SRTP result {}", self.0)
    }
}
impl std::error::Error for Error {}
#[repr(C)]
struct Options {
    profile: u32,
    direction: u32,
    key: *const u8,
    key_length: usize,
    replay_window: u32,
    encrypted_extensions: *const u32,
    encrypted_extension_count: usize,
}
unsafe extern "C" {
    fn gst_runtime_srtp_create(
        options: *const Options,
        state: *const u8,
        length: usize,
        restore: i32,
        out: *mut *mut c_void,
    ) -> i32;
    fn gst_runtime_srtp_export(context: *mut c_void, output: *mut u8, length: *mut usize) -> i32;
    fn gst_runtime_srtp_packet(
        context: *mut c_void,
        rtcp: i32,
        packet: *mut u8,
        capacity: usize,
        length: *mut usize,
    ) -> i32;
    fn gst_runtime_srtp_free(context: *mut c_void);
}
pub struct Context {
    raw: NonNull<c_void>,
    _exclusive: PhantomData<Cell<()>>,
}
// Every entry point requires exclusive ownership. Native objects have no
// callbacks, thread-local dependencies or external references.
unsafe impl Send for Context {}
impl Drop for Context {
    fn drop(&mut self) {
        unsafe { gst_runtime_srtp_free(self.raw.as_ptr()) }
    }
}
impl Context {
    pub fn create(config: Configuration<'_>) -> Result<Self, Error> {
        Self::open(config, None)
    }
    pub fn restore(config: Configuration<'_>, state: &[u8]) -> Result<Self, Error> {
        Self::open(config, Some(state))
    }
    fn open(c: Configuration<'_>, state: Option<&[u8]>) -> Result<Self, Error> {
        crate::initialize();
        let options = Options {
            profile: c.profile as _,
            direction: c.direction as _,
            key: c.key.as_ptr(),
            key_length: c.key.len(),
            replay_window: c.replay_window,
            encrypted_extensions: c.encrypted_extensions.as_ptr(),
            encrypted_extension_count: c.encrypted_extensions.len(),
        };
        let mut raw = std::ptr::null_mut();
        Error::check(unsafe {
            gst_runtime_srtp_create(
                &options,
                state.map_or(std::ptr::null(), |s| s.as_ptr()),
                state.map_or(0, |s| s.len()),
                i32::from(state.is_some()),
                &mut raw,
            )
        })?;
        Ok(Self {
            raw: NonNull::new(raw).ok_or(Error::INVALID)?,
            _exclusive: PhantomData,
        })
    }
    pub fn export(&mut self) -> Result<Vec<u8>, Error> {
        let mut length = 0;
        Error::check(unsafe {
            gst_runtime_srtp_export(self.raw.as_ptr(), std::ptr::null_mut(), &mut length)
        })?;
        let mut result = Vec::new();
        result.try_reserve_exact(length).map_err(|_| Error(3))?;
        result.resize(length, 0);
        Error::check(unsafe {
            gst_runtime_srtp_export(self.raw.as_ptr(), result.as_mut_ptr(), &mut length)
        })?;
        result.truncate(length);
        Ok(result)
    }
    /// Returns processed bytes only on success. No persistence or network I/O.
    /// A failed native call may consume state; export that state or retire owner.
    pub fn packet(&mut self, rtcp: bool, bytes: &[u8]) -> Result<Vec<u8>, Error> {
        let capacity = bytes.len().checked_add(32).ok_or(Error::INVALID)?;
        if capacity > i32::MAX as usize {
            return Err(Error::INVALID);
        }
        let mut packet = Vec::new();
        packet.try_reserve_exact(capacity).map_err(|_| Error(3))?;
        packet.extend_from_slice(bytes);
        packet.resize(capacity, 0);
        let mut length = bytes.len();
        let status = unsafe {
            gst_runtime_srtp_packet(
                self.raw.as_ptr(),
                i32::from(rtcp),
                packet.as_mut_ptr(),
                capacity,
                &mut length,
            )
        };
        if let Err(error) = Error::check(status) {
            // Failed authentication/decryption buffers must not survive release.
            for byte in &mut packet {
                unsafe { std::ptr::write_volatile(byte, 0) };
            }
            std::sync::atomic::compiler_fence(std::sync::atomic::Ordering::SeqCst);
            return Err(error);
        }
        packet.truncate(length);
        Ok(packet)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn restored_owners_preserve_rtp_and_rtcp_replay() {
        for profile in [
            Profile::AesCm128HmacSha1_80,
            Profile::AeadAes128Gcm,
            Profile::AeadAes256Gcm,
        ] {
            let key = vec![7; profile.key_length()];
            let send = Configuration {
                profile,
                direction: Direction::Send,
                key: &key,
                replay_window: 128,
                encrypted_extensions: &[],
            };
            let receive = Configuration {
                direction: Direction::Receive,
                ..send
            };
            let mut tx = Context::create(send).unwrap();
            let mut rx = Context::create(receive).unwrap();
            for (rtcp, packet) in [
                (false, vec![128, 96, 255, 255, 0, 0, 0, 1, 0, 0, 0, 7, 9]),
                (true, vec![128, 201, 0, 1, 0, 0, 0, 7]),
            ] {
                let wire = tx.packet(rtcp, &packet).unwrap();
                assert_eq!(rx.packet(rtcp, &wire).unwrap(), packet);
                tx = Context::restore(send, &tx.export().unwrap()).unwrap();
                rx = Context::restore(receive, &rx.export().unwrap()).unwrap();
                assert_eq!(rx.packet(rtcp, &wire).unwrap_err(), Error::REPLAY);
            }
            assert_eq!(
                tx.packet(false, &[128, 96, 255, 255, 0, 0, 0, 1, 0, 0, 0, 7, 9])
                    .unwrap_err(),
                Error::REPLAY
            );
            let wire = tx
                .packet(false, &[128, 96, 0, 0, 0, 0, 0, 2, 0, 0, 0, 7, 8])
                .unwrap();
            assert_eq!(rx.packet(false, &wire).unwrap()[12], 8);
        }
    }
}
