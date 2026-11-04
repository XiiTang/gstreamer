## Functional RTP repair fixture startup (#403, #348)

The runtime adds `gst_runtime_rtp_session_new_with_test_faststart` in the ordinary
native artifact and `Session::new_with_test_faststart` behind Rust's
`test-support` feature. It requires reordering and sets the existing
`faststart-min-packets=1` before PLAYING. The normal constructor retains 0;
settings ABI, configured latency, native loss/RTX rules, diagnostic/notify
ownership (including failed construction), and stop/fence behavior are unchanged.

Reason and regression evidence: the real NACK/RTX fixture previously declared
200 ms playout, omitted primary 2, and routed native feedback/repair on host
scheduling. A loaded host can route repair after native has emitted 3; upstream
then drops late 2 (`gstrtpjitterbuffer.c` chain's too-late branch). Boundless #403
has the matching real-driver trace and a held-repair reproduction. This is an
unexercised product window, not an assertion about deadline expiry. The fixture
now uses a 900 s window with one 60 s whole-phase hang budget, observes output A
before sending C, waits through retained/coalescing native notify predicates,
and still requires exact RTX mapping, repaired B, ordered A/B/C and PLI/FIR.
The independent real-socket Boundless RTP/RTSP fixtures use the same policy.

Upstream comparison (GStreamer 1.28.7): default jitter startup schedules DEADLINE
at PTS plus latency; increasing latency alone therefore holds initial output.
`gst_rtp_jitter_buffer_fast_start` reschedules only that initial DEADLINE after
`faststart-min-packets` consecutive packets. Once started, `handle_next_buffer`
pushes contiguous packets immediately and waits at gaps. The existing
`get_rtx_delay` requests missing packets from spacing/jitter (capped by latency
minus RTT); retry period and default request deadline still depend on latency.
This constructor changes no upstream algorithm, clock or timer; it separates
fixture startup from the unexercised window. Exact expiry/retry-count tests
remain upstream's GstHarness/GstTestClock tests. No GstCheck build flavor or
staging/provenance change is needed.

Regression entry: `runtime/tests/rtp_feedback.c`, built against this commit's
ordinary gst-full artifact (`runtime/tests/full_native.py` or the same compiler
and pkg-config inputs for that one fixture). Execution results are recorded by
the integrating revision/platform; this record does not claim an unrun pass.

# Boundless patches

## Runtime native readiness (#42)

The Boundless-written `runtime/` now owns one construction-time
`GstRuntimeNotify` per DTLS association or RTP session. It transfers callback,
data and destroy ownership even on failed construction. Notifications run on
DTLS caller/timeout threads and RTP streaming/message-posting/stop threads;
they must not block, panic or re-enter native state. Rust closures carry only
caller-owned readiness, with no Tokio dependency here. Read predicates before
waiting and retain a permit: notifications coalesce and may precede the wait.
DTLS output supports a non-consuming NULL-buffer length query, so the safe
owner allocates nothing for an empty queue and exactly the known packet length
otherwise. Failure precedence and non-consuming short-buffer refusal remain.

Upstream comparison (GStreamer 1.28.7): appsink invokes sample/EOS callbacks
with its committed queue state and holds a callback-table reference during
calls; appsrc invokes need-data after removal and likewise retains callback
data. We use these unchanged, with min-percent=100 for both packet and encoded
byte admission. gstreamer-rs AppSinkStream/AppSrcSink use these same readiness
points. Native DTLS already owns single-shot GstClock retransmission and its
timeout pool; start/input/BIO re-arm it. This patch leaves that scheduling,
upstream flow results, queue capacities, EOS and stop-without-BYE unchanged.
The fork's existing DTLS stop_and_join fence is retained before disconnecting
key/send hooks and destroying registration data. Key wakes cover CONNECTED:
state is published under the same connection lock; no additional property hook.

Upstream GstBus calls its sync handler before enqueueing PASS messages. A wake
there alone can precede an observable error. The runtime therefore retains
ERROR messages in its existing stop-mutex domain before waking and returns
DROP, owning each message until the active reader or stop offers and releases
it once. All unused non-ERROR messages are released with DROP at this same
handler, including SDES ELEMENT messages and shutdown state messages. There is
no asynchronous bus consumer. Passing them on would grow the queue while
connected and retain source objects through a message/pipeline/bus cycle after
free (review B1). The former upstream `gst_bus_pop_filtered(ERROR)` consumer
discarded unmatched messages; the lifetime handler preserves that duty without
polling. The same handler counts/offers errors during NULL. Upstream NULL
normally flushes the bus specifically to release messages and their source
references (`gstpipeline.c` READY_TO_NULL and `gst_bus_set_flushing`); the
runtime continues disabling that flush to retain cleanup failures, so it must
consume every message through the end of NULL. Stop detaches the handler and all six app callbacks,
waits for their destroy notifications (including failed construction/NULL),
then destroys registration data before native objects. Replacement alone would
not fence in-flight callbacks. No upstream source, timeout, recovery or retry
policy is changed; the C fixture-only blocking writer keeps its existing wait.

Regression: `tests/rtp_wake.c` drives real sample/EOS, packet/encoded-byte
capacity, active bus failure and pending-wake stop. `tests/rtp_bus.c` admits 64
valid compound RTCP RR/changed-SDES packets through the real input, waits for
each native SDES signal, checks the bus without draining it, and requires both
an empty bus through close and pipeline weak finalization after free. On
be349d8b it observes retained messages and an unfinalized pipeline. Existing
`tests/rtp_stop.c` once-only error/stop/fence assertions remain required.
`tests/dtls_timeout.c`
retains timer-failure/fence coverage and tests query/short-buffer and registration
destruction. `rust::dtls::tests::empty_output_allocates_nothing_and_a_stored_wake_delivers_the_exact_flight`
counts the output allocation checkpoint and checks a real flight. The full
native payload/session suite, joined-stop suite and safe Rust runtime suite
remain required against this commit's artifact. Boundless's production idle
worker/handshake regressions count native entries under a driven Tokio clock
and then observe actual delivery; independent peers check native retransmission.
Results are recorded per revision/platform by the caller; this record does not
claim unrun platforms or distribution acceptance.


The `boundless` branch carries these patches on GStreamer 1.28.7. Each is
required by Boundless's native media runtime (`runtime/`): media engines and
their state owned by an Execution, on connections and keys Boundless supplies.
[`runtime/README.md`](runtime/README.md) gives each one's details and evidence.

| Patch | Why | Regression |
| --- | --- | --- |
| [DTLS first failure](#dtls-first-failure) | Keep each originating GError before FAILED is observable, with a construction-only callback and runtime first-failure slot | `dtls_timeout.c` timer/consequence, registration and callback/free fences; `dtls_owner.c` real profile mismatch, zero text and close-notify |
| [DTLS final failure read](#dtls-final-failure-read-w3-d5) | Read the first failure after the callback fence and before free: free destroys the slot a running timer callback may still fill | `dtls_timeout.c` real gated timer callback across quiesce, and the output cap fact after quiesce; Rust `quiesce_keeps_the_final_failure_readable_until_free` |
| [RTP first failure](#rtp-stop-outcome-issue-63-batch-4) | Offer every consumed bus error before release, including failed construction, without reconstructing errors from counts | `rtp_stop.c` delayed/in-flight posts, active/stop consumers and construction; Rust RTP and payload callers |
| [RTSP credential generations](#rtsp-credential-generations) | A connection acquires one generation per exchange; nonblocking maintenance preparation keeps media/control live and preserves that generation through a Digest challenge | `rtsp::authentication::generation_tests`, `rtsp::keepalive::tests`, the Rust RTSP and keepalive suites |
| [Payload transforms through the RTP Session](#payload-transforms-through-the-rtp-session) | Split eac7272d01: keep shared typed settings, factories and frames; retire the unused standalone owner and ABI | Production Session frame/raw/pressure regressions and seven native format round trips, with independent FFmpeg pixels |
| [SDP metadata expansion](#sdp-metadata-expansion-driver-plan-rule-20) | Remove b5bc00f5f8's aggregate quotas; the native parser has no aggregate caps-field or byte capacity | `runtime/rust/tests/sdp.rs` feedback expansion regressions, plus the retained independent FFmpeg fixture |
| [Remove unused RTSP dispatch query](#unused-rtsp-dispatch-query-driver-plan-fork-audit) | Owner-approved removal of b6d8c35a9c; write admission and progress retain their returned `Dispatch` | Rust API and native export probes fail before removal and pass after; native RTSP transport and the whole Rust crate suite |
| Rust lint prerequisite (issue #63) | Use the existing null-pointer idiom in RTSP message copying and the common `12 + tag` minimum in SRTP validation; behavior is unchanged | With the matching private `BOUNDLESS_MEDIA_PREFIX`, `cargo clippy --manifest-path runtime/rust/Cargo.toml --all-targets --locked -- -D warnings`: pinned base exits 101 for `cmp_null` and `if_same_then_else`; repaired source exits 0 on macOS arm64 |
| [DTLS-SRTP](runtime/README.md#dtls-srtp) | Keys are exported only after a verified peer, for the declared profiles, from one handshake per connection, and are cleansed after use | `runtime/tests/dtls_interop.c` built by `runtime/tests/build_dtls.py`, against an independent OpenSSL DTLS server; `runtime/tests/dtls_owner.c` |
| [Controlled RTSP and RTP payloads](runtime/README.md#controlled-rtsp-and-rtp-payloads) | The RTSP client and payload transforms run on a stream and frames Boundless supplies, with bounded queues and no resolving, redirects, retries or pipeline strings | `runtime/tests/full_native.py` with FFmpeg fixtures; `runtime/tests/rtsp_transport.c`; `runtime/rust/tests/rtsp.rs` and `payload.rs` |
| [Private native build](runtime/README.md#controlled-rtsp-and-rtp-payloads) | `build_native.py` builds a private gst-full artifact with only the selected plugins, from hashed wraps, for macOS 13 and Windows | macOS and Windows builds; the deployment-target check rejects newer APIs |
| [Shared RTP session and SRTP state](runtime/README.md#shared-rtp-session-and-srtp-state) | Direct RTP and RTSP tracks share one owned `rtpsession` with SRTP state, declared feedback and RTX, SDP selection checked natively and explicit keepalive cycles with the nonblocking supply in [RTSP credential generations](#rtsp-credential-generations) | `runtime/tests/rtp_session.c`, `rtp_feedback.c`, `sdp_selection.c`, `payload.c`; `runtime/rust/tests/sdp.rs` and `keepalive.rs` |
| [Typed RTP session statistics](runtime/README.md#typed-rtp-session-statistics-2026-10-10) | Statistics reach the caller as typed fields, not `gst_structure_to_string` text | `runtime/tests/rtp_session.c`, `rtp_feedback.c`; the crate's `rtp` tests |
| [RTSP authentication](runtime/README.md#rtsp-authentication-2026-10-06) | A connection's credential authenticates every request through http-auth, a 401 answered once. The runtime follows Boundless's http-auth pin, whose Digest `uri` alone admits obs-text, so a raw non-ASCII request URI is answered as sent | `runtime/rust/tests/rtsp.rs` and `keepalive.rs` |
| [Track state after the connection ends](runtime/README.md#track-state-after-the-connection-ends-2026-10-14) | A track keeps the state its last response confirmed; only the client becomes unusable | `runtime/rust/tests/rtsp.rs` `a_track_keeps_its_confirmed_state_when_its_connection_ends`; `runtime/tests/rtsp_transport.c` |
| [A reply whose proof fails changes no track](runtime/README.md#a-reply-whose-proof-fails-changes-no-track-2026-10-15) | A Digest request's final response changes a track only once its proof is accepted | `runtime/rust/tests/rtsp.rs` `a_reply_whose_digest_proof_fails_changes_no_track` |
| [H264/H265 dropped RTP extensions](runtime/README.md#h264h265-dropped-rtp-extensions-2026-10-02) | Upstream MR !12389, backported: a dropped FU or NAL clears its delayed header extensions | `runtime/tests/depay_extensions.c` in `runtime/tests/full_native.py` |
| [RTP session end of input](#rtp-session-end-of-input-boundless-453) | A receive failure ends the session's input instead of abandoning it: what native already accepted (appsrc, `rtpsession`, jitterbuffer) still reaches the reader, and the reader learns when it has all of it | `runtime/tests/rtp_session.c` end-of-input drain in `runtime/tests/full_native.py`; Boundless `rtp::tests::relayed_packets_precede_the_relays_broken_datagram` |
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
credential for ordinary requests at `Authentication::request`. Maintenance's
nonblocking supplier enters that same authentication checkpoint with prepared
material. A Digest request owns that acquired credential
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

The owned keepalive scheduler formerly invoked that synchronous callback on its
exclusive actor. A held Access Refresh therefore stopped unrelated media and
control progress. `maintain` now offers one selected `KeepaliveDue` to a
nonblocking supplier; pending preparation holds no writer or CSeq. Identity is
only the existing cancellation allocation plus scheduled instant, and the
original selection-time deadline covers preparation, write and response.
Cancellation/replacement before final admission discards preparation. Unadmitted
expiry/acquisition failure rejects only the cycle; admitted failures keep actual
dispatch and native invalidation. Prepared requests reuse the native admission
and authentication construction; their Digest answer keeps that same material.

This changes Boundless's own Rust scheduler, not upstream `rtspsrc`:
`gst_rtspsrc_send_keep_alive` constructs/sends its own keepalive and warns on
failure, with no Access Refresh. Its C transport, cancellation/flush, response
correlation and retry behavior are unchanged. The owned runtime keeps its
write-first admission, earliest due selection, active-session checks, response
events and dispatched timeout handling; it adds the distinct unstarted rejection
without restarting the original allowance.

`rtsp::keepalive::tests::pending_supply_keeps_reads_live_and_one_generation_through_challenge`
drives both versions' paired-stream reads, pending identity, one supplied Digest
generation and a fresh next cycle. `selected_supply_obeys_cancellation_replacement_and_original_deadline`
drives disable/replacement, final cancellation and error precedence, pre-dispatch
rejection and admitted timeout through a private native-clock seam. Boundless's
held-Refresh regressions cover the shared Request/maintenance preparation owner
and Program-visible media/control progress in the consumer's pin update.

On macOS arm64 with Rust 1.97, this work revision (based on `81e61dd4`) passes
both new native test families, existing keepalive integration tests, the
in-flight 401/CSeq regression and strict library/test Clippy (all exit 0).
The original `rtsp::authentication::tests::` filter selected zero tests and
provided no authentication unit-test evidence. A follow-up run of
`rtsp::authentication::generation_tests` selected and passed both credential
generation tests (exit 0). A negative control that restores the former synchronous
maintenance-source route fails the pending-supply regression (exit 101): the
ordinary source is called once while preparation is pending, where zero calls
are required. Restoring the seam passes. These results establish this native
checkpoint, not Linux/Windows or full Boundless acceptance.

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

## RTP session end of input (Boundless #453)

When a Runtime's receive socket fails, the packets it already wrote into the
session's inbound port may still be inside native: queued in appsrc, in
`rtpsession`, or held by the jitterbuffer for its latency. Boundless must hand
them to the program before it reports the failure, and must know when there
is nothing more to read. The runtime had no way to say "no more input" for one
port, and no way to see that an output port had delivered its last sample.

`gst_runtime_rtp_session_end_input(session, port)` (Rust
`rtp::Session::end_input`) calls `gst_app_src_end_of_stream` on that input
port's appsrc under the port's writer lock;
`gst_runtime_rtp_session_output_ended(session, port)` (Rust
`rtp::Session::output_ended`) returns `gst_app_sink_is_eos` for that output
port. Both are thin uses of upstream behaviour, with no change to any element:
appsrc queues EOS behind the buffers it already accepted
(`gstappsrc.c`, `gst_app_src_end_of_stream`); `rtpsession` forwards EOS on
`recv_rtp_src` after the buffers before it (`gstrtpsession.c`,
`gst_rtp_session_event_recv_rtp_sink`); the jitterbuffer queues EOS as an item
after the packets it holds, stops buffering, keeps pushing across gaps once EOS
is set and sends EOS only when its queue is empty (`gstrtpjitterbuffer.c`,
`queue_event` and `handle_next_buffer`); and appsink reports EOS only once EOS
has arrived and every queued sample was pulled (`gstappsink.c`,
`gst_app_sink_is_eos`). A packet the jitterbuffer has not yet released for its
first deadline still waits for that deadline, so a reader drains within the
session's latency; Boundless bounds the drain by its worker's own deadline.

`runtime/tests/rtp_session.c` writes three packets into an inbound port with
reordering on (latency 500 ms), checks none is out yet and the output has not
ended, ends the input, reads the three in order, and then sees the output end.
With `end_input` reduced to a no-op the test fails (exit 134: the output never
ends within 5 s); with the patch it passes (exit 0) in
`runtime/tests/full_native.py` on macOS arm64.
