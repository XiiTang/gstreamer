/* Real appsrc/appsink readiness, retained errors and callback lifetime. */
#define GST_RUNTIME_RTP_TEST_HOOKS
#include "../gstruntimertpsession.c"
#include "test_sink.h"
typedef struct {
  GMutex lock; GCond changed;
  guint generation, destroyed;
  gboolean hold, entered, release;
  GThread *owner, *stop_thread;
  gboolean stop_woke;
} Wake;
static void notify (gpointer data)
{
  Wake *w = data;
  g_mutex_lock (&w->lock);
  g_assert_cmpuint (w->destroyed, ==, 0);
  w->generation++;
  if (g_thread_self () == w->stop_thread) { w->stop_woke = TRUE; g_cond_broadcast (&w->changed); }
  if (w->hold && g_thread_self () != w->owner) {
    w->entered = TRUE; g_cond_broadcast (&w->changed);
    while (!w->release) g_cond_wait (&w->changed, &w->lock);
  }
  g_cond_broadcast (&w->changed); g_mutex_unlock (&w->lock);
}
static void destroyed (gpointer data)
{
  Wake *w = data; g_mutex_lock (&w->lock);
  /* The blocked native callback must have returned before its box is freed. */
  g_assert_true (!w->entered || w->release);
  g_assert_cmpuint (++w->destroyed, ==, 1);
  g_cond_broadcast (&w->changed); g_mutex_unlock (&w->lock);
}
static GstRuntimeNotify registration (Wake *w)
{ w->owner = g_thread_self (); return (GstRuntimeNotify) { notify, w, destroyed }; }
static guint generation (Wake *w)
{ g_mutex_lock (&w->lock); guint n = w->generation; g_mutex_unlock (&w->lock); return n; }
static void changed (Wake *w, guint before)
{
  g_mutex_lock (&w->lock);
  gint64 stall = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  while (before == w->generation)
    g_assert_true (g_cond_wait_until (&w->changed, &w->lock, stall));
  g_mutex_unlock (&w->lock);
}
static const guint8 packet[] = {128, 96, 0, 1, 0, 0, 0, 1, 0, 0, 0, 7, 9};
static GstRuntimeRtpSettings settings = { .ssrc = 7, .payload_type = 96, .clock_rate = 90000 };
static void samples_and_eos (void)
{
  Wake w = {0}; TestSink facts = {0}; GstRuntimeDiagnosticSink sink = test_sink (&facts);
  GstRuntimeRtpSession *s = gst_runtime_rtp_session_new (&settings, &sink, registration (&w));
  g_assert_nonnull (s);
  for (guint i = 0; i < 4; i++)
    g_assert_cmpint (gst_runtime_rtp_session_try_write (s, 0, packet, sizeof (packet)), ==, 0);
  g_assert_cmpint (gst_runtime_rtp_session_end_input (s, 0), ==, 0);
  guint count = 0;
  while (TRUE) {
    /* Predicate before wait, with a generation that stores earlier wakes. */
    guint before = generation (&w); GstRuntimePayloadFrame *frame = NULL;
    int result = gst_runtime_rtp_session_pull (s, 0, 0, &frame);
    if (result == 0) {
      GstRuntimePayloadFrameView view; gst_runtime_payload_frame_view (frame, &view);
      g_assert_cmpmem (view.data, view.length, packet, sizeof (packet));
      gst_runtime_payload_frame_free (frame); count++; continue;
    }
    g_assert_cmpint (result, ==, 1);
    if (gst_runtime_rtp_session_output_ended (s, 0)) break;
    changed (&w, before);
  }
  g_assert_cmpuint (count, ==, 4);
  gst_runtime_rtp_session_free (s); g_assert_cmpuint (w.destroyed, ==, 1);
  test_sink_assert_none (&facts);
}
typedef struct { GMutex lock; GCond changed; gboolean entered, release; } Gate;
static GstPadProbeReturn hold_buffer (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  Gate *g = data; g_mutex_lock (&g->lock); g->entered = TRUE; g_cond_broadcast (&g->changed);
  while (!g->release) g_cond_wait (&g->changed, &g->lock);
  g_mutex_unlock (&g->lock); return GST_PAD_PROBE_REMOVE;
}
static GstAppSrc *watched_source;
static gint partial_removals;
static void removed (GstAppSrc *source)
{ if (source == watched_source && gst_app_src_get_current_level_buffers (source) > 0)
    g_atomic_int_inc (&partial_removals); }
static void capacity_after_removal (gboolean encoded)
{
  Wake w = {0}; TestSink facts = {0}; GstRuntimeDiagnosticSink sink = test_sink (&facts);
  GstRuntimeRtpSettings config = settings;
  GstRuntimePayloadSettings payload = { .format = GST_RUNTIME_PAYLOAD_PCMU, .ssrc = 7,
    .payload_type = 96, .clock_rate = 8000, .mtu = 1200, .channels = 1 };
  if (encoded) { config.payload = &payload; config.clock_rate = 8000; }
  GstRuntimeRtpSession *s = gst_runtime_rtp_session_new (&config, &sink, registration (&w));
  g_assert_nonnull (s);
  Gate g = {0}; GstPad *pad = gst_element_get_static_pad (s->source[0], "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, hold_buffer, &g, NULL);
  guint8 *bytes = g_malloc0 (16 * 1024 * 1024);
  g_assert_cmpint (encoded ? gst_runtime_rtp_session_try_write_frame (s, bytes, 1, 0, GST_SECOND)
                          : gst_runtime_rtp_session_try_write (s, 0, packet, sizeof (packet)), ==, 0);
  g_mutex_lock (&g.lock); while (!g.entered) g_cond_wait (&g.changed, &g.lock); g_mutex_unlock (&g.lock);
  if (encoded) {
    /* Bytes, rather than the eight-buffer limit, hold this admission. */
    g_assert_cmpint (gst_runtime_rtp_session_try_write_frame (s, bytes, 8*1024*1024, 1, GST_SECOND), ==, 0);
    g_assert_cmpint (gst_runtime_rtp_session_try_write_frame (s, bytes, 8*1024*1024, 2, GST_SECOND), ==, 0);
    g_assert_cmpint (gst_runtime_rtp_session_try_write_frame (s, bytes, 2, 2, GST_SECOND), ==, 1);
  } else {
    for (guint i = 0; i < 32; i++)
      g_assert_cmpint (gst_runtime_rtp_session_try_write (s, 0, packet, sizeof (packet)), ==, 0);
    g_assert_cmpint (gst_runtime_rtp_session_try_write (s, 0, packet, sizeof (packet)), ==, 1);
  }
  guint before = generation (&w), at_release = before;
  watched_source = GST_APP_SRC (s->source[0]); partial_removals = 0; test_need_data = removed;
  g_mutex_lock (&g.lock); g.release = TRUE; g_cond_broadcast (&g.changed); g_mutex_unlock (&g.lock);
  for (;;) {
    int result = encoded ? gst_runtime_rtp_session_try_write_frame (s, bytes, 2, 2, GST_SECOND)
                         : gst_runtime_rtp_session_try_write (s, 0, packet, sizeof (packet));
    if (!result) break;
    g_assert_cmpint (result, ==, 1); changed (&w, before); before = generation (&w);
  }
  /* Observe native removal even if admission raced ahead of its callback. */
  changed (&w, at_release);
  for (;;) {
    guint observed = generation (&w);
    if (g_atomic_int_get (&partial_removals) > 0) break;
    changed (&w, observed);
  }
  gst_runtime_rtp_session_free (s); test_need_data = NULL; watched_source = NULL; gst_object_unref (pad); g_free (bytes);
  g_assert_cmpuint (w.destroyed, ==, 1); test_sink_assert_none (&facts);
}
static void active_error (gboolean stop_first)
{
  Wake w = {0}; TestSink facts = {0}; GstRuntimeDiagnosticSink sink = test_sink (&facts);
  GstRuntimeRtpSession *s = gst_runtime_rtp_session_new (&settings, &sink, registration (&w));
  guint before = generation (&w);
  GError *error = g_error_new_literal (GST_STREAM_ERROR, GST_STREAM_ERROR_DECODE, "wake fixture");
  gst_bus_post (s->bus, gst_message_new_error (GST_OBJECT (s->pipeline), error, NULL)); g_error_free (error);
  g_assert_cmpuint (generation (&w), >, before);
  if (stop_first) gst_runtime_rtp_session_stop (s);
  GstRuntimePayloadFrame *frame = NULL;
  g_assert_cmpint (gst_runtime_rtp_session_pull (s, 0, 0, &frame), ==,
    stop_first ? GST_FLOW_FLUSHING : GST_FLOW_CUSTOM_ERROR);
  GstRuntimeRtpStopOutcome outcome; g_assert_cmpint (gst_runtime_rtp_session_close (s, &outcome), ==, 0);
  g_assert_cmpuint (outcome.bus_errors, ==, stop_first ? 1 : 0);
  g_assert_cmpuint (facts.count, ==, 1); g_assert_cmpint (facts.first_code, ==, GST_STREAM_ERROR_DECODE);
  gst_runtime_rtp_session_free (s); g_assert_cmpuint (w.destroyed, ==, 1); g_free (facts.first_domain);
}
typedef struct { GstRuntimeRtpSession *session; Wake *wake; } Stopping;
static gpointer stop (gpointer data) {
  Stopping *s = data;
  g_mutex_lock (&s->wake->lock); s->wake->stop_thread = g_thread_self (); g_mutex_unlock (&s->wake->lock);
  gst_runtime_rtp_session_stop (s->session); return NULL;
}
static void pending_wake_stop (void)
{
  Wake w = {0}; TestSink facts = {0}; GstRuntimeDiagnosticSink sink = test_sink (&facts);
  GstRuntimeRtpSession *s = gst_runtime_rtp_session_new (&settings, &sink, registration (&w));
  g_mutex_lock (&w.lock); w.hold = TRUE; g_mutex_unlock (&w.lock);
  g_assert_cmpint (gst_runtime_rtp_session_try_write (s, 0, packet, sizeof (packet)), ==, 0);
  g_mutex_lock (&w.lock); while (!w.entered) g_cond_wait (&w.changed, &w.lock); g_mutex_unlock (&w.lock);
  Stopping stopping = {s, &w};
  GThread *thread = g_thread_new ("stop", stop, &stopping);
  g_mutex_lock (&w.lock);
  while (!w.stop_woke) g_cond_wait (&w.changed, &w.lock);
  g_assert_cmpuint (w.destroyed, ==, 0);
  w.release = TRUE; g_cond_broadcast (&w.changed); g_mutex_unlock (&w.lock);
  g_thread_join (thread);
  GstRuntimeRtpStopOutcome outcome; g_assert_cmpint (gst_runtime_rtp_session_close (s, &outcome), ==, 0);
  g_assert_cmpuint (w.destroyed, ==, 1); gst_runtime_rtp_session_free (s);
  g_assert_cmpuint (w.destroyed, ==, 1); test_sink_assert_none (&facts);
}
int main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  samples_and_eos (); capacity_after_removal (FALSE); capacity_after_removal (TRUE);
  active_error (FALSE); active_error (TRUE); pending_wake_stop ();
  g_print ("PASS RTP readiness: 6 real sample/EOS, capacity, error and pending-wake stop cases\n");
}
