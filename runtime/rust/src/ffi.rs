use std::ffi::{c_char, c_int, c_void};
#[repr(C)]
pub struct Header {
    pub name: *const c_char,
    pub value: *const c_char,
}
#[repr(C)]
#[derive(Default)]
pub struct MessageView {
    pub kind: c_int,
    pub status: c_int,
    pub channel: c_int,
    pub version: c_int,
    pub method: *const c_char,
    pub uri: *const c_char,
    pub reason: *const c_char,
    pub body: *const u8,
    pub raw: *const u8,
    pub body_length: usize,
    pub raw_length: usize,
}
#[repr(C)]
#[derive(Default)]
pub struct TransportView {
    pub profile: i32,
    pub lower_transport: i32,
    pub mode_play: i32,
    pub mode_record: i32,
    pub rtcp_mux: i32,
    pub interleaved_first: i32,
    pub interleaved_last: i32,
    pub client_first: i32,
    pub client_last: i32,
    pub server_first: i32,
    pub server_last: i32,
    pub source: *const c_char,
    pub destination: *const c_char,
    pub src_host: [*const c_char; 2],
    pub dest_host: [*const c_char; 2],
    pub src_port: [u32; 2],
    pub dest_port: [u32; 2],
    pub src_count: u32,
    pub dest_count: u32,
    pub ssrc_count: u32,
    pub ssrcs: *const u32,
    pub generation: u64,
}
unsafe extern "C" {
    pub fn gst_runtime_initialize();
    pub fn gst_runtime_rtsp_transport(
        client: *mut c_void,
        session: *const c_char,
        uri: *const c_char,
        view: *mut TransportView,
    ) -> c_int;
    pub fn gst_runtime_rtsp_new(
        uri: *const c_char,
        fd: c_int,
        version: c_int,
        limit: u32,
        out: *mut *mut c_void,
        taken: *mut c_int,
    ) -> c_int;
    pub fn gst_runtime_rtsp_request(
        client: *mut c_void,
        method: *const c_char,
        uri: *const c_char,
        headers: *const Header,
        count: usize,
        body: *const u8,
        length: usize,
        timeout: i64,
        sequence: *mut u32,
        dispatch: *mut c_int,
    ) -> c_int;
    pub fn gst_runtime_rtsp_respond(
        client: *mut c_void,
        status: c_int,
        reason: *const c_char,
        headers: *const Header,
        count: usize,
        body: *const u8,
        length: usize,
        timeout: i64,
    ) -> c_int;
    pub fn gst_runtime_rtsp_wait(client: *mut c_void, timeout: i64) -> c_int;
    pub fn gst_runtime_rtsp_receive(
        client: *mut c_void,
        timeout: i64,
        out: *mut *mut c_void,
    ) -> c_int;
    pub fn gst_runtime_rtsp_receive_step(client: *mut c_void, out: *mut *mut c_void) -> c_int;
    pub fn gst_runtime_rtsp_message_view(message: *mut c_void, view: *mut MessageView);
    pub fn gst_runtime_rtsp_message_header(
        message: *mut c_void,
        index: u32,
        header: *mut Header,
    ) -> c_int;
    pub fn gst_runtime_rtsp_message_free(message: *mut c_void);
    pub fn gst_runtime_rtsp_state(
        client: *mut c_void,
        session: *const c_char,
        uri: *const c_char,
    ) -> c_int;
    pub fn gst_runtime_rtsp_cancel(client: *mut c_void);
    pub fn gst_runtime_rtsp_free(client: *mut c_void);
}
