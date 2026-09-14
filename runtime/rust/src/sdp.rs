//! Owned views of the native SDP parser. Original fields (including timing,
//! repeated and unknown attributes) are retained without interpreting policy.
use crate::Error;
use std::{
    ffi::{CStr, CString, c_char, c_void},
    ptr::NonNull,
};
#[repr(C)]
struct MediaView {
    media: *const c_char,
    protocol: *const c_char,
    port: u32,
    ports: u32,
    formats: u32,
}
#[repr(C)]
struct FieldView {
    scope: i32,
    kind: i32,
    value: *const c_char,
}
unsafe extern "C" {
    fn gst_runtime_sdp_new(data: *const u8, length: usize, result: *mut *mut c_void) -> i32;
    fn gst_runtime_sdp_free(description: *mut c_void);
    fn gst_runtime_sdp_control(
        description: *mut c_void,
        media: i32,
        base: *const c_char,
        result: *mut *mut c_char,
    ) -> i32;
    fn gst_runtime_sdp_text_free(text: *mut c_char);
    fn gst_runtime_sdp_media_count(description: *mut c_void) -> u32;
    fn gst_runtime_sdp_media(description: *mut c_void, index: u32, view: *mut MediaView) -> i32;
    fn gst_runtime_sdp_field(description: *mut c_void, index: u32, view: *mut FieldView) -> i32;
    fn gst_runtime_sdp_format(
        description: *mut c_void,
        media: u32,
        format: u32,
        name: *mut *const c_char,
        caps: *mut *const c_char,
    ) -> i32;
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Field {
    pub media: Option<u32>,
    pub kind: char,
    pub value: String,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Format {
    pub name: String,
    pub native_caps: Option<String>,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Media {
    pub kind: String,
    pub protocol: String,
    pub port: u16,
    pub port_count: u16,
    pub formats: Vec<Format>,
    pub control_uri: Option<String>,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Description {
    pub control_uri: Option<String>,
    pub fields: Vec<Field>,
    pub media: Vec<Media>,
}
struct Owned(NonNull<c_void>);
impl Drop for Owned {
    fn drop(&mut self) {
        unsafe { gst_runtime_sdp_free(self.0.as_ptr()) };
    }
}
unsafe fn text(value: *const c_char) -> Result<String, Error> {
    if value.is_null() {
        return Err(Error::INVALID);
    }
    unsafe { CStr::from_ptr(value) }
        .to_str()
        .map(str::to_owned)
        .map_err(|_| Error::INVALID)
}
impl Description {
    pub fn parse(bytes: &[u8]) -> Result<Self, Error> {
        Self::parse_options(bytes, None)
    }
    pub fn parse_with_base(bytes: &[u8], base: &str) -> Result<Self, Error> {
        Self::parse_options(bytes, Some(base))
    }
    fn parse_options(bytes: &[u8], base: Option<&str>) -> Result<Self, Error> {
        crate::initialize();
        let base = base
            .map(CString::new)
            .transpose()
            .map_err(|_| Error::INVALID)?;
        let mut pointer = std::ptr::null_mut();
        Error::check(unsafe { gst_runtime_sdp_new(bytes.as_ptr(), bytes.len(), &mut pointer) })?;
        let owned = Owned(NonNull::new(pointer).ok_or(Error::INVALID)?);
        let raw = owned.0.as_ptr();
        let control = |scope: i32| -> Result<Option<String>, Error> {
            let Some(base) = base.as_ref() else {
                return Ok(None);
            };
            let mut value = std::ptr::null_mut();
            Error::check(unsafe {
                gst_runtime_sdp_control(raw, scope, base.as_ptr(), &mut value)
            })?;
            if value.is_null() {
                return Ok(None);
            }
            let result = unsafe { text(value) };
            unsafe { gst_runtime_sdp_text_free(value) };
            result.map(Some)
        };
        let control_uri = control(-1)?;
        let mut fields = Vec::new();
        loop {
            let mut view = FieldView {
                scope: 0,
                kind: 0,
                value: std::ptr::null(),
            };
            if unsafe {
                gst_runtime_sdp_field(
                    raw,
                    fields.len().try_into().map_err(|_| Error::INVALID)?,
                    &mut view,
                )
            } != 0
            {
                break;
            }
            fields.push(Field {
                media: if view.scope < 0 {
                    None
                } else {
                    Some(view.scope as u32)
                },
                kind: char::from_u32(view.kind as u32).ok_or(Error::INVALID)?,
                value: unsafe { text(view.value) }?,
            });
        }
        let mut media = Vec::new();
        for index in 0..unsafe { gst_runtime_sdp_media_count(raw) } {
            let mut view = MediaView {
                media: std::ptr::null(),
                protocol: std::ptr::null(),
                port: 0,
                ports: 0,
                formats: 0,
            };
            Error::check(unsafe { gst_runtime_sdp_media(raw, index, &mut view) })?;
            let mut formats = Vec::new();
            for format in 0..view.formats {
                let mut name = std::ptr::null();
                let mut caps = std::ptr::null();
                Error::check(unsafe {
                    gst_runtime_sdp_format(raw, index, format, &mut name, &mut caps)
                })?;
                formats.push(Format {
                    name: unsafe { text(name) }?,
                    native_caps: if caps.is_null() {
                        None
                    } else {
                        Some(unsafe { text(caps) }?)
                    },
                });
            }
            media.push(Media {
                control_uri: control(index.try_into().map_err(|_| Error::INVALID)?)?,
                kind: unsafe { text(view.media) }?,
                protocol: unsafe { text(view.protocol) }?,
                port: view.port.try_into().map_err(|_| Error::INVALID)?,
                port_count: view.ports.try_into().map_err(|_| Error::INVALID)?,
                formats,
            });
        }
        Ok(Self {
            control_uri,
            fields,
            media,
        })
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    const HEADER: &str = "v=0\r\no=- 123456789012345678901234567890 2 IN IP4 127.0.0.1\r\ns=Example\r\nc=IN IP4 127.0.0.1\r\nt=12345678901 12345678999\r\nr=7d 1h 0 25h\r\nr=1d 1h 0\r\nt=0 0\r\na=control:*\r\n";
    #[test]
    fn native_fields_and_payload_maps_survive_owner_drop() {
        let bytes = format!(
            "{HEADER}m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z0IAH5WoFAFuQA==,aM4G4g==\r\na=control:track/1\r\na=x-unknown:one\r\na=x-unknown:two\r\nm=audio 5004/2 RTP/AVP 0 8\r\na=control:rtsp://elsewhere.invalid/audio\r\n"
        );
        let d =
            Description::parse_with_base(bytes.as_bytes(), "rtsps://camera.invalid/base/").unwrap();
        assert_eq!(
            d.control_uri.as_deref(),
            Some("rtsps://camera.invalid/base/")
        );
        assert_eq!(
            d.media[0].control_uri.as_deref(),
            Some("rtsps://camera.invalid/base/track/1")
        );
        assert_eq!(
            d.media[1].control_uri.as_deref(),
            Some("rtsp://elsewhere.invalid/audio")
        );
        assert_eq!(d.media.len(), 2);
        assert_eq!((d.media[1].port, d.media[1].port_count), (5004, 2));
        assert!(
            d.media[0].formats[0]
                .native_caps
                .as_ref()
                .unwrap()
                .contains("H264")
        );
        assert_eq!(
            d.fields
                .iter()
                .filter(|f| f.kind == 't')
                .map(|f| f.value.as_str())
                .collect::<Vec<_>>(),
            ["12345678901 12345678999", "0 0"]
        );
        assert_eq!(d.fields.iter().filter(|f| f.kind == 'r').count(), 2);
        assert_eq!(
            d.fields
                .iter()
                .filter(|f| f.value.starts_with("x-unknown:"))
                .count(),
            2
        );
        assert_eq!(d.fields.last().unwrap().media, Some(1));
    }
    #[test]
    fn malformed_and_truncated_descriptions_are_rejected_before_mapping() {
        for body in [
            "",
            "v=1\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=x\r\nt=0 0\r\n",
            "v=0\r\ns=x\r\nt=0 0\r\n",
            "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=x\r\nr=1d 1h 0\r\nt=0 0\r\n",
        ] {
            assert!(Description::parse(body.as_bytes()).is_err(), "{body:?}");
        }
        for tail in [
            "m=audio -1 RTP/AVP 0\r\n",
            "m=audio 65536 RTP/AVP 0\r\n",
            "m=audio 5004 RTP/AVP 128\r\n",
            "m=audio 1 RTP/AVP\r\n",
            "x=ignored\r\n",
            "a=bad name:value\r\n",
            "a=x:one\0hidden\r\n",
            "o=- 1 1 IN IP4 127.0.0.1\r\n",
        ] {
            assert!(
                Description::parse(format!("{HEADER}{tail}").as_bytes()).is_err(),
                "{tail:?}"
            );
        }
        assert!(
            Description::parse(format!("{HEADER}a=x:{}\r\n", "a".repeat(65536)).as_bytes())
                .is_err()
        );
    }
}
