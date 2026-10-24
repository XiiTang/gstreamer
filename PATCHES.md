# Boundless patches

The `boundless` branch carries these patches on GStreamer 1.28.7. Each is
required by Boundless's native media runtime (`runtime/`): media engines and
their state owned by an Execution, on connections and keys Boundless supplies.
[`runtime/README.md`](runtime/README.md) gives each one's details and evidence.

| Patch | Why | Regression |
| --- | --- | --- |
| [Payload transforms through the RTP Session](#payload-transforms-through-the-rtp-session) | Split eac7272d01: keep shared typed settings, factories and frames; retire the unused standalone owner and ABI | Production Session frame/raw/pressure regressions and seven native format round trips, with independent FFmpeg pixels |
| [SDP metadata expansion](#sdp-metadata-expansion-driver-plan-rule-20) | Remove b5bc00f5f8's aggregate quotas; the native parser has no aggregate caps-field or byte capacity | `runtime/rust/tests/sdp.rs` feedback expansion regressions, plus the retained independent FFmpeg fixture |
| [Remove unused RTSP dispatch query](#unused-rtsp-dispatch-query-driver-plan-fork-audit) | Owner-approved removal of b6d8c35a9c; write admission and progress retain their returned `Dispatch` | Rust API and native export probes fail before removal and pass after; native RTSP transport and the whole Rust crate suite |
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

## RTP stop outcome (issue #63, batch 4)

The first stop retains its state-change result and every queued/during-stop
bus error for the exclusive owner's consuming `Session::close`. Cancellation
remains void, cloneable and unable to consume that result. NULL bus auto-flush
is disabled; the stop checkpoint captures synchronous bus errors and waits
for in-flight handlers before publishing. Read and close share its lock, so
no reader takes an error already owned by stop. The current generic error
representation is preserved.

`runtime/tests/build_rtp_stop.py` and the Rust
`close_reads_errors_after_a_cancellation_clone_stopped_first` regression
cover a failing NULL transition and queued/during-stop bus errors when
cancellation wins. The native regression cannot build on the original
source, which has no retained outcome or close API (exit 1). The private
native build and `build_rtp_stop.py` pass (exit 0); the consuming Rust
regression and all ten library tests pass (exit 0) on macOS arm64. Strict
all-target Clippy remains blocked by the same two diagnostics in unmodified
RTSP/SRTP code reproduced on the original pinned source (exit 101).

## Jitterbuffer fatal flow results (issue #63, batch 4)

When its source task pauses, a NOT_LINKED or fatal flow result below EOS
must reach the owner's bus. `gst_rtp_jitter_buffer_loop` is the sole source
loop and handles these results after unlocking, using `GST_ELEMENT_FLOW_ERROR`
as upstream's base parser does. EOS and FLUSHING retain their normal handling.

`GST_CHECKS=test_downstream_flow_error_reaches_bus meson test -C "$GOOD_BUILD"
--no-rebuild --print-errorlogs elements_rtpjitterbuffer` fails without the
patch (exit 1: no bus error for NOT_LINKED), and passes with it (exit 0).
The test synchronizes with the actual source task and checks five fatal
results, plus EOS and FLUSHING, through the bus. The whole affected suite,
`meson test -C "$GOOD_BUILD" --no-rebuild --print-errorlogs
elements_rtpjitterbuffer`, also passes (exit 0) on macOS arm64.

## Unused RTSP dispatch query (driver plan fork audit)

The owner-approved fork audit retires b6d8c35a9c under driver-plan Rules 8/18:
only the unused `Rtsp::pending_dispatch` and its C/FFI bridge are removed.
`request_begin` and `write_step` still return `Dispatch`; their existing C
checkpoints, `request_mode` and `gst_runtime_rtsp_write_step`, keep the native
sequence and actual write progress. No API or new checkpoint is added.
Upstream likewise returns the send result at the send call
(`subprojects/gst-plugins-good/gst/rtsp/gstrtspsrc.c:3125-3141`).

On macOS arm64, a temporary Rust API probe compiled with `rustc --emit=metadata`
calls `client.pending_dispatch()`: parent 5526f1ed00 compiles (the absence check
exits 1); the removal produces E0599 (the check exits 0). A native `nm -gU`
export probe likewise exits 1 on that parent's private artifact and 0 after
removal, while requiring both write admission and progress exports to remain.
These probes are temporary: the retired API shape gets no permanent test.

With the matching private native artifact, `cargo test --manifest-path
runtime/rust/Cargo.toml --locked -- --test-threads=2 --include-ignored` passes
all 32 tests (exit 0), including
`incremental_owned_request_and_declared_data_progress_while_read_is_partial`
and the independent FFmpeg test. The native `runtime/tests/rtsp_transport.c`
test passes (exit 0), including partial-write duplex progress and cancellation.
All-target strict Clippy and Rust fmt also pass (exit 0).

Landing must run Boundless `protocols::rtsp::` and `protocols::media`, including
RTSP-IO's `rtsp_inbound_and_state_progress_while_outbound_peer_is_not_draining`,
`bridge_preserves_io_cause_and_later_native_partial_response` and
`rtsp_close_interrupts_partial_response_and_joins_native_owner`, and RTSP-LATE's
`a_response_its_operation_stopped_waiting_for_is_an_event`.

## SDP metadata expansion (driver plan Rule 20)

A valid description could become `INVALID` when media × formats × fields
crossed b5bc00f5f8's aggregate entry or byte quotas. The owner-approved removal
uses the existing `gst_runtime_sdp_new` checkpoint: the Rust description reader
always constructs its opaque native description there. The added input metadata
counter and aggregate caps counters are removed together; the independent FFmpeg
fixture stays.

Upstream builds a separate caps structure for each format
(`subprojects/gst-plugins-base/gst-libs/gst/sdp/gstsdpmessage.c:3630-3818`).
`GstStructure` grows its own field array (`subprojects/gstreamer/gst/gststructure.c:169-228`);
the pinned GLib 2.86.3 `glib/garray.c:g_ptr_array_maybe_expand` limits a pointer
array's length, not the total fields or string bytes in its elements. Neither
provides an aggregate media × formats × fields capacity. The retained input
bounds already keep format indices below the native array/index capacities;
removing the cumulative counters leaves no aggregate arithmetic to overflow.
No replacement quota or failure API is introduced.

`media_feedback_is_retained_for_every_format` and
`long_feedback_names_are_retained_for_every_format` each exercise four media,
32 distinct payload types per media, and wildcard feedback attributes copied
to every format. Both fail with `INVALID` on 2dced6022d (exit 101), rather than
returning each format's Boolean fields; their assertions retain the complete
ordered fields and values. `independent_ffmpeg_pcmu_description_retains_actual_payload_mapping`
continues to check an independently generated description.

## Payload transforms through the RTP Session

The owner-approved split of eac7272d01 keeps `payload::Configuration`, `Format`
and `Frame`, their internal settings/frame conversion, and the native caps,
transform and frame helpers used by the production `rtp::Session` and
58029c7101. The helper error type becomes crate-private because no public
payload operation remains. The unused `payload::open`, Control/Input/Output
and their native create/write/read/stop/free owner path are removed; native
standalone new/push/pull/error/stop/free and the pipeline they owned have no
remaining production caller either.

Upstream's `rtpbin::create_session` creates and owns each RTP session
(`subprojects/gst-plugins-good/gst/rtpmanager/gstrtpbin.c:753-788`). The production
`gst_runtime_rtp_session_new` already owns its codec elements, built by the
retained shared `gst_runtime_payload_transform_new`; no second pipeline owner
is needed. Shared
caps and `gst_runtime_payload_frame_take/view/free` stay unchanged.

`runtime/rust/tests/payload.rs` now checks encoded PCMA/PCMU round trips,
sequence wrap, exact raw RTP, malformed input, pressure and cancellation through
`rtp::Session`. Both production Session regressions pass before removal on
2dced6022d (exit 0). `runtime/tests/payload.c` keeps seven Session codec round
trips, timestamps, MTU/SSRC/sequence checks, access-unit rejection, jitter pressure,
optimized-JPEG rejection before output and raw RTP/stop checks. Shared checks
formerly exercised by the standalone owner move to that Session path. The full
native harness retains independent FFmpeg pixel checks for its recovered frames.
