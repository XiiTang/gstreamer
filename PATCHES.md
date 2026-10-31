# Boundless patches

The `boundless` branch carries these patches on GStreamer 1.28.7. Each is
required by Boundless's native media runtime (`runtime/`): media engines and
their state owned by an Execution, on connections and keys Boundless supplies.
[`runtime/README.md`](runtime/README.md) gives each one's details and evidence.

| Patch | Why | Regression |
| --- | --- | --- |
| [DTLS first failure](#dtls-first-failure) | Keep each originating GError before FAILED is observable, with a construction-only callback and runtime first-failure slot | `dtls_timeout.c` timer/consequence, registration and callback/free fences; `dtls_owner.c` real profile mismatch, zero text and close-notify |
| [DTLS final failure read](#dtls-final-failure-read-w3-d5) | Read the first failure after the callback fence and before free: free destroys the slot a running timer callback may still fill | `dtls_timeout.c` real gated timer callback across quiesce, and the output cap fact after quiesce; Rust `quiesce_keeps_the_final_failure_readable_until_free` |
| [RTP first failure](#rtp-stop-outcome-issue-63-batch-4) | Offer every consumed bus error before release, including failed construction, without reconstructing errors from counts | `rtp_stop.c` delayed/in-flight posts, active/stop consumers and construction; Rust RTP and payload callers |
| [RTSP credential generations](#rtsp-credential-generations) | A connection can acquire current credentials for each authentication exchange while keeping one generation through a Digest challenge | `rtsp::authentication::generation_tests`, the Rust RTSP and keepalive suites |
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
no reader takes an error already owned by stop. A borrowed DiagnosticSink receives each error domain, code and message before release,
including while a failed construction cleans up. Active bus failures return
GST_FLOW_CUSTOM_ERROR; bus_errors is only a count, never a reconstructed Error(-5).

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

## RTSP credential generations

A fixed connection credential prevented a caller's renewed value from reaching
later authentication exchanges. `configure_authentication_callback` supplies a
credential at `Authentication::request`, the checkpoint used by ordinary
requests and native keepalives. A Digest request owns that acquired credential
through its challenge answer; nonce state stays with the authentication owner.
No callback is invoked for the answer. Without a callback, fixed-credential
behavior and the existing transport restrictions are unchanged.

On macOS arm64, the regression fixture using db8bcea522's existing fixed-source
constructor fails (exit 101): the next request carries `Basic first` after the
issuing value changes to `Basic second`. The callback fixture
`a_committed_generation_reaches_the_next_exchange` passes, and
`each_exchange_acquires_the_current_generation_and_its_answer_keeps_it` proves
one acquisition through a challenge and a new generation on the next exchange.
The occupied-writer regression `busy_admission_does_not_acquire_an_extra_generation`
fails before writer admission is moved ahead of callback acquisition (exit 101):
two unadmitted attempts acquire two extra generations. It covers both Basic and
Digest. The callback path now shares native `request_mode`'s writer-first
admission check through `gst_runtime_rtsp_request_admission`; busy attempts do
not acquire, and invalid connections retain their native error. Fixed-source
validation order is unchanged. This adds the admission query to the private
native ABI. The Rust library, RTSP and keepalive suites and library/test Clippy
pass on macOS arm64.

## DTLS first failure

The connection reports every originating timer, SSL and SRTP export failure under
its lock before FAILED can be observed, even when the caller supplied no GError
output. The construction-only failure callback cannot be replaced or installed
after start/stop. The runtime's sole `fail_locked` setter retains the first
GError domain/code/message; consequences cannot replace it. The accessor lends
an immortal domain and copies at most the requested message bytes. Runtime-owned
output, key, application-data, close and fallback failures have distinct codes.
`gst_runtime_dtls_free` fences callbacks before freeing their data.

Regression evidence: `dtls_timeout.c` forces a timer failure before its later
READ consequence, checks the runtime slot, registration and an in-flight
callback across free. `dtls_owner.c` checks a real no-common-profile GError,
zero-capacity text and peer close-notify (CLOSED before generic PROCESS).
`rtp_stop.c` forces queue/handler ordering and the stop/free fence, both consumers
of a queued error and a failed construction. Rust RTP tests exercise the sink
lifetime through construction, cancellation and consuming close. These are
native-owner checks; they do not claim Boundless transport acceptance.

The W5-D5 native-owner regressions above pass on macOS arm64, including the
independent OpenSSL DTLS exporter/close-notify checks, deterministic callback
and bus-handler fences, and H264/H265 capacity failures. All seven payload
round trips preserve independent FFmpeg pixels for H264/H265/JPEG. The Rust
RTP/DTLS and payload/SDP owner tests and strict library/test Clippy pass; the
construction/stop fixture also passes with `--cfg rtp_stop_test`. The direct
construction fixture initializes the library explicitly, independently of test
ordering. These checks retain no optional Boundless diagnostic text.

## DTLS final failure read (W3-D5)

`gst_runtime_dtls_free` joins the timer callbacks with
`gst_dtls_connection_stop_and_join` and then destroys the first-failure slot.
A timer callback still running when the owner decides to dispose can record
the association's first failure during that join; a read before free misses
it and no read is possible after. Boundless's media close must carry that
failure, so the library needs a point after the fence and before free.

`gst_runtime_dtls_quiesce` calls the existing `stop_and_join` fence without
holding the runtime mutex (a running callback may take it) and frees nothing;
free is unchanged and its own call to the fence finds the pool already taken.
Rust `Association::quiesce(self) -> Quiesced` consumes the association, so a
stopped association cannot be started or used again; `Quiesced` offers only
`failure` and frees on drop. No flag, per-operation guard, close-notify or
second failure record is added.

Upstream (e55686cc) has no runtime layer: `GstDtlsConnection` stop and close
run under the connection mutex and its liveness owner
(`gstdtlsconnection.c:525–595`) and elements free the connection by unref. The
fence this patch reuses is the earlier fork patch's `stop_and_join`.

Regression: `dtls_timeout.c` holds a real timer callback in its failure
report, quiesces from another thread, and checks that quiesce waits for it,
that the fact is readable afterwards and that free then releases the native
state; a second case checks the output-cap fact survives quiesce and a healthy
association quiesces with no fact. The Rust test reads a verifier rejection
through `Quiesced` and a healthy handshake's empty slot.

On macOS arm64, from this patch's private static build: `dtls_timeout` exits
0 (both fence arms and the cap case) and `dtls_interop` exits 0; the Rust DTLS
tests (4) and strict library/test Clippy pass. These are native-owner checks;
Boundless's media close and establishment results are its own tests.
