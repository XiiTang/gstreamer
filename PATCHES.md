# Boundless patches

The `boundless` branch carries these patches on GStreamer 1.28.7. Each is
required by Boundless's native media runtime (`runtime/`): media engines and
their state owned by an Execution, on connections and keys Boundless supplies.
[`runtime/README.md`](runtime/README.md) gives each one's details and evidence.

| Patch | Why | Regression |
| --- | --- | --- |
| Rust lint prerequisite (issue #63) | Use the existing null-pointer idiom in RTSP message copying and the common `12 + tag` minimum in SRTP validation; behavior is unchanged | With the matching private `BOUNDLESS_MEDIA_PREFIX`, `cargo clippy --manifest-path runtime/rust/Cargo.toml --all-targets --locked -- -D warnings`: pinned base exits 101 for `cmp_null` and `if_same_then_else`; repaired source exits 0 on macOS arm64 |
| [DTLS-SRTP](runtime/README.md#dtls-srtp) | Keys are exported only after a verified peer, for the declared profiles, from one handshake per connection, and are cleansed after use | `runtime/tests/dtls_interop.c` built by `runtime/tests/build_dtls.py`, against an independent OpenSSL DTLS server; `runtime/tests/dtls_owner.c` |
| [Controlled RTSP and RTP payloads](runtime/README.md#controlled-rtsp-and-rtp-payloads) | The RTSP client and payload transforms run on a stream and frames Boundless supplies, with bounded queues and no resolving, redirects, retries or pipeline strings | `runtime/tests/full_native.py` with FFmpeg fixtures; `runtime/tests/rtsp_transport.c`; `runtime/rust/tests/rtsp.rs` and `payload.rs` |
| [Private native build](runtime/README.md#controlled-rtsp-and-rtp-payloads) | `build_native.py` builds a private gst-full artifact with only the selected plugins, from hashed wraps, for macOS 13 and Windows | macOS and Windows builds; the deployment-target check rejects newer APIs |
| [Shared RTP session and SRTP state](runtime/README.md#shared-rtp-session-and-srtp-state) | Direct RTP and RTSP tracks share one owned `rtpsession` with SRTP state, declared feedback and RTX, SDP selection checked natively and explicit keepalive cycles | `runtime/tests/rtp_session.c`, `rtp_feedback.c`, `sdp_selection.c`, `payload.c`; `runtime/rust/tests/sdp.rs` and `keepalive.rs` |
| [Typed RTP session statistics](runtime/README.md#typed-rtp-session-statistics-2026-10-10) | Statistics reach the caller as typed fields, not `gst_structure_to_string` text | `runtime/tests/rtp_session.c`, `rtp_feedback.c`; the crate's `rtp` tests |
| [RTSP authentication](runtime/README.md#rtsp-authentication-2026-10-06) | A connection's credential authenticates every request through http-auth, a 401 answered once | `runtime/rust/tests/rtsp.rs` and `keepalive.rs` |
| [Track state after the connection ends](runtime/README.md#track-state-after-the-connection-ends-2026-10-14) | A track keeps the state its last response confirmed; only the client becomes unusable | `runtime/rust/tests/rtsp.rs` `a_track_keeps_its_confirmed_state_when_its_connection_ends`; `runtime/tests/rtsp_transport.c` |
| [A reply whose proof fails changes no track](runtime/README.md#a-reply-whose-proof-fails-changes-no-track-2026-10-15) | A Digest request's final response changes a track only once its proof is accepted | `runtime/rust/tests/rtsp.rs` `a_reply_whose_digest_proof_fails_changes_no_track` |
| [H264/H265 dropped RTP extensions](runtime/README.md#h264h265-dropped-rtp-extensions-2026-10-02) | Upstream MR !12389, backported: a dropped FU or NAL clears its delayed header extensions | `runtime/tests/depay_extensions.c` in `runtime/tests/full_native.py` |
| Rename project integration to Boundless | The runtime crate and build name the project | Builds |

## DTLS timeout failure (issue #63, batch 4)

A failed `DTLSv1_handle_timeout` must publish the connection's failure, just
as a failed handshake does. All timer work runs through `handle_timeout`;
it now sets FAILED under the connection lock and uses the existing unlocked
property notification. Repeated failures do not notify twice.

`runtime/tests/build_dtls.py` builds `dtls_timeout` from the actual connection
implementation with only OpenSSL's timer operation replaced. `dtls_timeout`
fails on the previous source (exit 134: CONNECTING instead of FAILED) and
passes with the patch (exit 0). It checks the public state and notification,
unlocked delivery, repeated failure, a non-expired timer and a stopped timer.
The same helper's `dtls_interop` passes (exit 0), including independent
OpenSSL exporter checks for all three profiles, on macOS arm64.
