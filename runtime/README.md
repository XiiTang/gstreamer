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

Session maintenance observations: the native RTSP client parses the response-only
Session timeout parameter (default 60 seconds) without narrowing its integer
value. Requests cannot send timeout parameters. The session view exposes the
negotiated value, whether it was explicit, and age of the last confirmed control
response. These observations do not imply server expiry: RTCP can also provide
liveness, and a local timer neither closes the track nor sends a keepalive.
OPTIONS/GET_PARAMETER remain explicit requests with the selected Session ID.
Validation covers both versions, zero and u64 maximum, missing/default values,
overflow, duplicate/invalid parameters and no implicit requests.

SDP media selection is validated natively against the already negotiated track:
resolved control URI (including single-media session inheritance), RTP profile and
optional lower transport, media direction, payload/clock, codec framing, and the
declared feedback/RTX `apt` mapping. Selection owns immutable caps. RTP session
construction copies them into the receive mapping and outgoing caps negotiation;
callers cannot submit arbitrary caps or pipeline strings. Opus sender hints are
not treated as receive compatibility limits. H264 supports packetization mode 1,
H265 supports no DON fields, and MPEG4-GENERIC supports the declared AAC-hbr AU
header layout and matching AudioSpecificConfig. Other SDP fields remain available
in the original parsed description; this does not claim encoder constraints are
validated against every future encoded frame. `sdp_selection.c` covers acceptance
and rejection and `payload.c` binds all seven actual payload round trips through
SDP, with independent FFmpeg pixel checks. RFC 7826 Appendix D and RFC 7587 define
the inherited control/direction and Opus hint interpretation respectively.

Explicit periodic RTSP maintenance is owned by the Rust native adapter on the
same serialized client. Configure one OPTIONS or GET_PARAMETER cycle per session,
with a positive interval, write/response deadline, URI and optional already chosen
authentication challenge. The adapter does not derive an interval from the remote
Session timeout. The host polls `keepalive_step` and `receive_step`, defers other
requests while a maintenance request is outstanding, and continues reading media
and server requests. A negative response or unavailable authentication challenge
ends that cycle without retry/fallback. Original responses still use the ordinary
message path. A partial-write/response timeout preserves dispatch uncertainty and
cancels the control owner. The cancellation handle stops future cycles while an
already dispatched request retains its deadline; this also covers an abandoned
configuration handoff. Local timer passage never asserts remote session expiry.
An already selected Digest cycle follows verified `Authentication-Info` nonce
continuation within that same context, preserving qop and frozen identity. A new
401 challenge still stops the cycle and requires an explicit selection. Native
tests independently verify SHA-256 request and response proofs, nonce-count reset
and subsequent increment for both RTSP versions and auth/auth-int.
