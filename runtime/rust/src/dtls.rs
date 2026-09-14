//! One native DTLS-SRTP association. Runtime owns all transport and trust policy.
//! No sockets, shared identity cache, key export logging or implicit reconnect.
use crate::{Error, srtp::Profile};
use std::{
    cell::Cell,
    ffi::{CString, c_char, c_void},
    marker::PhantomData,
    ptr::NonNull,
};
use zeroize::{Zeroize, Zeroizing};

#[repr(C)]
struct CertificateDer {
    data: *const u8,
    length: usize,
}
type Verify = unsafe extern "C" fn(*const CertificateDer, usize, *mut c_void) -> i32;
type Verifier = Box<dyn Fn(&[&[u8]]) -> bool + Send + Sync>;
#[repr(C)]
struct NativeKeys {
    profile: u32,
    length: usize,
    send: [u8; 44],
    receive: [u8; 44],
}
impl Default for NativeKeys {
    fn default() -> Self {
        Self {
            profile: 0,
            length: 0,
            send: [0; 44],
            receive: [0; 44],
        }
    }
}
impl Drop for NativeKeys {
    fn drop(&mut self) {
        self.send.zeroize();
        self.receive.zeroize();
    }
}
unsafe extern "C" {
    fn gst_runtime_dtls_create(
        pem: *const c_char,
        profiles: *const c_char,
        verify: Verify,
        data: *mut c_void,
        out: *mut *mut c_void,
    ) -> i32;
    fn gst_runtime_dtls_start(raw: *mut c_void, client: i32) -> i32;
    fn gst_runtime_dtls_input(raw: *mut c_void, bytes: *mut u8, length: usize) -> i32;
    fn gst_runtime_dtls_status(raw: *mut c_void) -> i32;
    fn gst_runtime_dtls_output(raw: *mut c_void, bytes: *mut u8, length: *mut usize) -> i32;
    fn gst_runtime_dtls_take_keys(raw: *mut c_void, keys: *mut NativeKeys) -> i32;
    fn gst_runtime_dtls_fingerprint(raw: *mut c_void, output: *mut u8) -> i32;
    fn gst_runtime_dtls_free(raw: *mut c_void);
}
unsafe extern "C" fn verify_chain(
    chain: *const CertificateDer,
    length: usize,
    data: *mut c_void,
) -> i32 {
    if chain.is_null() || data.is_null() || length == 0 || length > 16 {
        return 0;
    }
    // Native lifetime contract: valid immutable slices for this callback only.
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let verifier = unsafe { &*data.cast::<Verifier>() };
        let native = unsafe { std::slice::from_raw_parts(chain, length) };
        let mut slices = Vec::with_capacity(length);
        for cert in native {
            if cert.data.is_null() || cert.length == 0 || cert.length > 65536 {
                return false;
            }
            slices.push(unsafe { std::slice::from_raw_parts(cert.data, cert.length) });
        }
        verifier(&slices)
    }));
    i32::from(result.unwrap_or(false))
}

pub struct Keys {
    pub profile: Profile,
    pub send: Zeroizing<Vec<u8>>,
    pub receive: Zeroizing<Vec<u8>>,
}
pub struct Association {
    raw: NonNull<c_void>,
    // Boxed twice so the native opaque userdata points at a stable sized value.
    _verifier: Box<Verifier>,
    _exclusive: PhantomData<Cell<()>>,
}
// Native mutexes synchronize its timer callbacks; every public operation is
// exclusive. Drop joins the timeout pool before the verifier is freed.
unsafe impl Send for Association {}
impl Drop for Association {
    fn drop(&mut self) {
        unsafe { gst_runtime_dtls_free(self.raw.as_ptr()) };
    }
}
impl Association {
    pub fn new(
        pem: Option<&str>,
        profiles: &[Profile],
        verify: impl Fn(&[&[u8]]) -> bool + Send + Sync + 'static,
    ) -> Result<Self, Error> {
        crate::initialize();
        if profiles.is_empty()
            || profiles.len() > 3
            || profiles
                .iter()
                .enumerate()
                .any(|(i, p)| profiles[..i].contains(p))
        {
            return Err(Error::INVALID);
        }
        let names = CString::new(
            profiles
                .iter()
                .map(|p| match p {
                    Profile::AesCm128HmacSha1_80 => "SRTP_AES128_CM_SHA1_80",
                    Profile::AeadAes128Gcm => "SRTP_AEAD_AES_128_GCM",
                    Profile::AeadAes256Gcm => "SRTP_AEAD_AES_256_GCM",
                })
                .collect::<Vec<_>>()
                .join(":"),
        )
        .map_err(|_| Error::INVALID)?;
        // CString does not zero its allocation. Use an explicitly terminated
        // private buffer for the identity crossing this synchronous boundary.
        let pem = pem
            .map(|p| {
                if p.is_empty() || p.len() > 1024 * 1024 || p.as_bytes().contains(&0) {
                    return Err(Error::INVALID);
                }
                let mut bytes = Zeroizing::new(p.as_bytes().to_vec());
                bytes.push(0);
                Ok(bytes)
            })
            .transpose()?;
        let mut verifier: Box<Verifier> = Box::new(Box::new(verify));
        let mut raw = std::ptr::null_mut();
        Error::check(unsafe {
            gst_runtime_dtls_create(
                pem.as_ref().map_or(std::ptr::null(), |p| p.as_ptr().cast()),
                names.as_ptr(),
                verify_chain,
                (&mut *verifier as *mut Verifier).cast(),
                &mut raw,
            )
        })?;
        Ok(Self {
            raw: NonNull::new(raw).ok_or(Error::INVALID)?,
            _verifier: verifier,
            _exclusive: PhantomData,
        })
    }
    pub fn start(&mut self, client: bool) -> Result<(), Error> {
        Error::check(unsafe { gst_runtime_dtls_start(self.raw.as_ptr(), i32::from(client)) })
    }
    pub fn receive(&mut self, datagram: &[u8]) -> Result<(), Error> {
        if datagram.is_empty() || datagram.len() > 65535 {
            return Err(Error::INVALID);
        }
        let mut bytes = Zeroizing::new(datagram.to_vec());
        Error::check(unsafe {
            gst_runtime_dtls_input(self.raw.as_ptr(), bytes.as_mut_ptr(), bytes.len())
        })
    }
    pub fn connected(&mut self) -> Result<bool, Error> {
        match unsafe { gst_runtime_dtls_status(self.raw.as_ptr()) } {
            0 => Ok(false),
            1 => Ok(true),
            n => Err(Error(n)),
        }
    }
    pub fn output(&mut self) -> Result<Option<Vec<u8>>, Error> {
        let mut bytes = vec![0; 65535];
        let mut length = bytes.len();
        match unsafe { gst_runtime_dtls_output(self.raw.as_ptr(), bytes.as_mut_ptr(), &mut length) }
        {
            0 => Ok(None),
            1 => {
                bytes.truncate(length);
                Ok(Some(bytes))
            }
            n => Err(Error(n)),
        }
    }
    pub fn take_keys(&mut self) -> Result<Option<Keys>, Error> {
        let mut keys = NativeKeys::default();
        match unsafe { gst_runtime_dtls_take_keys(self.raw.as_ptr(), &mut keys) } {
            0 => Ok(None),
            1 => {
                let profile = match keys.profile {
                    1 => Profile::AesCm128HmacSha1_80,
                    2 => Profile::AeadAes128Gcm,
                    3 => Profile::AeadAes256Gcm,
                    _ => return Err(Error::INVALID),
                };
                if keys.length != profile.key_length() {
                    return Err(Error::INVALID);
                }
                Ok(Some(Keys {
                    profile,
                    send: Zeroizing::new(keys.send[..keys.length].to_vec()),
                    receive: Zeroizing::new(keys.receive[..keys.length].to_vec()),
                }))
            }
            n => Err(Error(n)),
        }
    }
    pub fn fingerprint_sha256(&mut self) -> Result<[u8; 32], Error> {
        let mut bytes = [0; 32];
        Error::check(unsafe {
            gst_runtime_dtls_fingerprint(self.raw.as_ptr(), bytes.as_mut_ptr())
        })?;
        Ok(bytes)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    };

    fn exchange(client: &mut Association, server: &mut Association) -> Result<(), Error> {
        client.start(true)?;
        server.start(false)?;
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(3);
        while std::time::Instant::now() < deadline {
            while let Some(packet) = client.output()? {
                server.receive(&packet)?;
            }
            while let Some(packet) = server.output()? {
                client.receive(&packet)?;
            }
            if client.connected()? && server.connected()? {
                return Ok(());
            }
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        Err(Error::TIMEOUT)
    }
    #[test]
    fn association_verifies_before_one_shot_keys_and_matches_both_directions() {
        for profile in [
            Profile::AesCm128HmacSha1_80,
            Profile::AeadAes128Gcm,
            Profile::AeadAes256Gcm,
        ] {
            let verified = Arc::new(AtomicUsize::new(0));
            let counter = verified.clone();
            let mut client = Association::new(None, &[profile], move |chain| {
                assert_eq!(chain.len(), 1);
                assert!(!chain[0].is_empty());
                counter.fetch_add(1, Ordering::SeqCst);
                true
            })
            .unwrap();
            let mut server = Association::new(None, &[profile], |_| true).unwrap();
            assert!(client.take_keys().unwrap().is_none());
            assert_ne!(
                client.fingerprint_sha256().unwrap(),
                server.fingerprint_sha256().unwrap()
            );
            exchange(&mut client, &mut server).unwrap();
            assert!(verified.load(Ordering::SeqCst) > 0);
            let c = client.take_keys().unwrap().unwrap();
            let s = server.take_keys().unwrap().unwrap();
            assert_eq!(c.profile, profile);
            assert_eq!(c.send, s.receive);
            assert_eq!(c.receive, s.send);
            assert!(client.take_keys().is_err());
            assert!(client.start(true).is_err());
        }
    }
    #[test]
    fn rejected_or_panicking_verifier_never_releases_keys() {
        for panic in [false, true] {
            let mut client = Association::new(None, &[Profile::AeadAes128Gcm], move |_| {
                assert!(!panic, "synthetic verifier failure");
                false
            })
            .unwrap();
            let mut server = Association::new(None, &[Profile::AeadAes128Gcm], |_| true).unwrap();
            assert!(exchange(&mut client, &mut server).is_err());
            assert!(client.take_keys().is_err());
        }
    }
    #[test]
    fn supplied_invalid_identity_is_rejected_and_pending_timer_owner_is_released() {
        assert!(
            Association::new(Some("invalid identity"), &[Profile::AeadAes128Gcm], |_| {
                true
            })
            .is_err()
        );
        for _ in 0..32 {
            let marker = Arc::new(());
            let callback_marker = marker.clone();
            let mut owner = Association::new(None, &[Profile::AeadAes128Gcm], move |_| {
                let _ = &callback_marker;
                true
            })
            .unwrap();
            owner.start(true).unwrap();
            assert_eq!(Arc::strong_count(&marker), 2);
            drop(owner);
            assert_eq!(Arc::strong_count(&marker), 1);
        }
    }
}
