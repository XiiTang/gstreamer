//! Ownership adapters for the pinned private media library. Wire parsing,
//! protocol transitions and media engines remain in the native library.
mod ffi;
pub mod payload;
pub mod rtsp;
use std::sync::Once;
static INITIALIZE: Once = Once::new();
pub fn initialize() {
    INITIALIZE.call_once(|| unsafe { ffi::gst_runtime_initialize() });
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Error(pub i32);
impl Error {
    pub const INVALID: Self = Self(-2);
    pub const CANCELLED: Self = Self(-3);
    pub const EOF: Self = Self(-11);
    pub const TIMEOUT: Self = Self(-14);
    pub(crate) fn check(code: i32) -> Result<(), Self> {
        if code == 0 { Ok(()) } else { Err(Self(code)) }
    }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Native media result {}", self.0)
    }
}
impl std::error::Error for Error {}
