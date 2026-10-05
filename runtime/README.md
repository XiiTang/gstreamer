# Boundless native media integration

This fork keeps GStreamer's media engines and native state behind Boundless's
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

The build takes GLib, libffi, PCRE2 and proxy-libintl from this commit's hashed
Meson wraps, compiling them into gst-full instead of loading developer-installed
base libraries. macOS compilation targets 13.0. OpenSSL and the pinned libSRTP
artifact are explicit private prefixes; platform packaging verifies their actual
architecture and minimum OS before relocating and signing the complete closure.
macOS builds reject unguarded calls to APIs newer than that deployment target.
The GLib wrap patch checks the annotated `pipe2` declaration: a newer SDK's
linkable symbol must not enable calls unavailable on the deployment OS.

`tests/full_native.py` links the actual private artifact and tests RTSP, all seven
payload formats, raw RTP, backpressure and joined stop. FFmpeg independently
encodes the fixtures and decodes recovered H264/H265/JPEG for exact pixel checks.
Optimized JPEG tables are a negative test. `BOUNDLESS_MEDIA_PREFIX=<private-prefix>
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
with a positive interval, write/response deadline and URI. The adapter does not
derive an interval from the remote Session timeout. The host polls `maintain`
and `receive_step`, defers its own requests while `maintenance_pending` and its
other writes while `maintenance_writing`, and continues reading media and server
requests. A negative response ends that cycle without retry/fallback. Original
responses still use the ordinary message path. A partial-write/response timeout
preserves dispatch uncertainty and cancels the control owner. The cancellation
handle stops future cycles while an already dispatched request retains its
deadline; this also covers an abandoned configuration handoff. Local timer
passage never asserts remote session expiry.

## RTSP authentication (2026-10-06)

A connection configured with a credential authenticates every request the
adapter sends for it, keepalives included; the native client itself still never
retries. Basic is sent with every request, and on RTSP 2.0 only over TLS
(RFC 7826 section 19.1). Digest uses http-auth's `DigestSession`, which
Boundless's HTTP and proxy answers use too: a 401 offering a challenge is
answered once by sending the request again. CSeq advances only as a request is
sent and nothing else is sent before that one, so the 401 is delivered at once
naming the CSeq it goes out as, also when interleaved data still occupies the
writer; the request goes out as that CSeq or the client is invalidated. The
connection's later requests answer from the start with the adopted nonce,
counted, until the server challenges again or proves a `nextnonce`. `auth` is
answered where offered, `auth-int` where it is all that is offered, since the
adapter holds whole bodies. A reply whose proof does not verify invalidates the
client; a reply with none is taken at its word. Only a Digest connection keeps a
copy of the request in flight. The caller chooses no challenge, algorithm or qop
and cannot set `Authorization`. `request` is `request_begin` and `write_step`, so
one path authenticates; `receive` keeps the native read, whose cancellation
returns the raw bytes so far, and writes a request sent again before it returns.
Before this, the caller selected each answer explicitly and repeated
http-auth's choice of challenge and proof check here, so each fix to either
landed twice.

`tests/rtsp.rs` checks both versions' automatic answer with the request and
reply proofs recomputed independently with SHA-256, an unreadable challenge
passed over, the count across requests, one answer per request, a 401 read
while data occupies the writer, a failing proof, an unproven `nextnonce` not
adopted and a proven one adopted, and Basic only over TLS on RTSP 2.0;
`tests/keepalive.rs` checks keepalives answering from the start and a stale 401
to one answered with the new nonce.

## H264/H265 dropped RTP extensions (2026-10-02)

Backport the three commits from MR !12389: bb387b3c7bea6d275d20b13af8c622bd465f7c69,
9e627c3d881fedafb6400ac60fb94404d7305145 and a7e6f79e3b5d17fc077c01552663a1e09ab0c280.
Clear delayed/cached extensions when FU/NALs are dropped, including waiting for
a keyframe. Keep the 1.28.7 baseline, native 16 MiB materialization limits and
DTLS key ownership.

`tests/depay_extensions.c` exercises both native depayloaders synchronously.
It observes exact extension callbacks across 1024 interrupted-FU/empty-NAL
transitions plus 1024 consecutive empty aggregation drops, then clean output.
The original pinned artifact fails (2047 extension callbacks versus 1024); the
repaired artifact passes and carries no stale extension into the next output.
`tests/full_native.py` includes this test and the existing independent FFmpeg
pixel, seven payload formats, bounded access-unit/backpressure and stop checks.
