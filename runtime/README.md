# IMAPipe native media integration

This fork keeps GStreamer's media engines and native state behind IMAPipe's
execution-owned network and resource boundary. The Runtime adapter and packaging
are separate acceptance gates; the tests here are native-library evidence.

## DTLS-SRTP

- `gst_dtls_connection_set_srtp_profiles` selects only the declared profiles,
  before the first handshake. Each connection permits one handshake. A new
  connection is required for a new security session.
- Supported profiles: AES128_CM_SHA1_80 (30 bytes), AEAD_AES_128_GCM (28 bytes),
  AEAD_AES_256_GCM (44 bytes). Exporter layout is client key, server key, client
  salt, server salt. Native encoder/decoder key signals include the actual length.
- A positive peer certificate decision is required before exporting keys.
  `dtlsdec::accept-peer-certificate` defaults to rejection. The Runtime must
  install the verification decision from its frozen identity policy; observing
  `peer-pem` is insufficient.
- Temporary exporter material and retained key buffers are cleansed. Native
  decoder/encoder key logging is removed. General object/caps diagnostics still
  require the production adapter to keep key-bearing objects private.
- Timer registrations hold weak references and are replaced/cancelled explicitly.
  Stopping or releasing an incomplete handshake does not retain the connection
  until the next timer expiration and sends no close-notify.

`tests/build_dtls.py` takes explicit native dependency/configuration paths and
builds both the complete DTLS plugin and `tests/dtls_interop.c`. The latter uses
an independent OpenSSL DTLS server over isolated loopback UDP. It checks exact
exporter bytes, fresh key material, trust rejection, missing verification,
non-overlapping profile offers, restart rejection and 64 pending-timer lifetimes.
No test key is logged or saved to disk. This build helper is for native tests;
it is not the product's dependency discovery or packaging policy.

## Controlled RTSP and RTP payloads

`gst_rtsp_runtime_client_*` owns explicit RTSP 1.0/2.0 request and session/track
state on a supplied connected stream. It never resolves or connects a target,
selects a fallback version, follows a redirect, retries authentication or sends
TEARDOWN during release. Responses preserve native parsed fields and exact wire
bytes. Partial-message cancellation is interruptible; uncertain state is retained.
The opaque `gstruntimeapi` ABI and `rust` crate give the Runtime an exclusive
movable owner and a separately shareable cancellation handle.

`gstruntimepayload` transforms encoded H264/H265/JPEG/Opus/MPEG4-GENERIC AAC/PCMA/
PCMU frames and raw RTP using bounded appsrc/appsink queues. There is no encoder,
decoder, player or arbitrary pipeline expression. RFC 2435 JPEG requires standard
Huffman tables and its representable single scan; other input is rejected before
output instead of silently changing decoded pixels.

`build_native.py` selects only app, rtp, rtpmanager, srtp and dtls plugins into a
private gst-full shared artifact. Filesystem registry/plugin scanning, pipeline
parsing and native debug dumps are disabled. Native dependencies must still be
bundled and relocated by platform packaging; the gst-full artifact alone is not
a self-contained distribution.

`tests/full_native.py` links the actual private artifact and tests RTSP, all seven
payload formats, raw RTP, backpressure and joined stop. FFmpeg independently
encodes the fixtures and decodes recovered H264/H265/JPEG for exact pixel checks.
Optimized JPEG tables are a negative test. `IMAPIPE_MEDIA_PREFIX=<private-prefix>
cargo test --manifest-path runtime/rust/Cargo.toml` checks the safe owner ABI,
repeated extension headers, binary bodies and cancellation with raw evidence.

These checks do not claim Runtime media transport, SRTP persistence, complete
RTP/RTCP session behavior or platform packaging acceptance.

## Shared RTP session and SRTP state

`gstruntimertpsession` wraps the native `rtpsession` engine on bounded appsrc/
appsink ports. It preserves raw RTP fields, checks declared outbound SSRC/PT,
exposes native source statistics, and emits RTCP reports only when enabled.
Nonblocking admission lets one Runtime actor continue draining all output ports
while a native input queue is full. Stop joins native tasks without injecting EOS
or BYE. Direct RTP and RTSP tracks use this same owner. Feedback/RTX and Runtime
transport integration have separate acceptance gates.

`gstruntimesrtpapi` and `rust::srtp` own the complete versioned libSRTP state.
`native-lock.json` pins the patched library; `build_native.py --srtp-prefix ...`
checks its committed source and artifact hashes before building. Every Runtime
packet must commit encrypted state before sending ciphertext or delivering
verified plaintext. Native/Rust tests do not replace Runtime crash/lease tests.
