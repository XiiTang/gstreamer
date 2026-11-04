/* Real first-packet startup while serialized setup is still being delivered. */
#define GST_RUNTIME_RTP_TEST_HOOKS
/* Do not interpose the unused feedback wrapper on the installed artifact. Its
 * private RTPSession entry points are not exported; strip this unused copy. */
#define gst_runtime_rtp_session_feedback faststart_unused_feedback
#include "../gstruntimertpsession.c"
#include "test_sink.h"

#define STALL_US (60 * G_USEC_PER_SEC)
#define TEST_STALL_MS (900 * 1000)
typedef struct {
  GMutex lock;
  GCond changed;
  gboolean caps_entered, chained, release;
  guint generation;
  GstPadChainFunction chain;
  GstPadChainListFunction chain_list;
} Startup;
static Startup *startup;

static void
notify (gpointer data)
{
  Startup *s = data;
  g_mutex_lock (&s->lock);
  s->generation++;
  g_cond_broadcast (&s->changed);
  g_mutex_unlock (&s->lock);
}

static GstPadProbeReturn
hold_caps (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  Startup *s = data;
  if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) != GST_EVENT_CAPS)
    return GST_PAD_PROBE_OK;
  g_mutex_lock (&s->lock);
  s->caps_entered = TRUE;
  g_cond_broadcast (&s->changed);
  while (!s->release)
    g_cond_wait (&s->changed, &s->lock);
  g_mutex_unlock (&s->lock);
  return GST_PAD_PROBE_REMOVE;
}

static void
chain_completed (Startup *s, GstFlowReturn result)
{
  g_assert_cmpint (result, ==, GST_FLOW_OK);
  g_mutex_lock (&s->lock);
  s->chained = TRUE;
  g_cond_broadcast (&s->changed);
  g_mutex_unlock (&s->lock);
}

static GstFlowReturn
chain (GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
  Startup *s = g_object_get_data (G_OBJECT (pad), "faststart-startup");
  GstFlowReturn result = s->chain (pad, parent, buffer);
  chain_completed (s, result);
  return result;
}

static GstFlowReturn
chain_list (GstPad *pad, GstObject *parent, GstBufferList *buffers)
{
  Startup *s = g_object_get_data (G_OBJECT (pad), "faststart-startup");
  GstFlowReturn result = s->chain_list (pad, parent, buffers);
  chain_completed (s, result);
  return result;
}

static void
before_playing (GstElement *pipeline)
{
  GstIterator *it = gst_bin_iterate_elements (GST_BIN (pipeline));
  GValue value = G_VALUE_INIT;
  GstElement *jitter = NULL;
  while (gst_iterator_next (it, &value) == GST_ITERATOR_OK) {
    GstElement *element = g_value_get_object (&value);
    GstElementFactory *factory = gst_element_get_factory (element);
    if (factory && !g_strcmp0 (gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)),
                              "rtpjitterbuffer"))
      jitter = gst_object_ref (element);
    g_value_reset (&value);
  }
  g_value_unset (&value);
  gst_iterator_free (it);
  g_assert_nonnull (jitter);
  GstPad *src = gst_element_get_static_pad (jitter, "src");
  GstPad *sink = gst_element_get_static_pad (jitter, "sink");
  gst_pad_add_probe (src, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, hold_caps, startup, NULL);
  startup->chain = GST_PAD_CHAINFUNC (sink);
  startup->chain_list = GST_PAD_CHAINLISTFUNC (sink);
  g_object_set_data (G_OBJECT (sink), "faststart-startup", startup);
  gst_pad_set_chain_function (sink, chain);
  gst_pad_set_chain_list_function (sink, chain_list);
  gst_object_unref (src);
  gst_object_unref (sink);
  gst_object_unref (jitter);
}

int
main (void)
{
  extern void gst_runtime_initialize (void);
  gst_runtime_initialize ();
  Startup state = {0};
  startup = &state;
  test_before_playing = before_playing;
  TestSink facts = {0};
  GstRuntimeDiagnosticSink sink = test_sink (&facts);
  GstRuntimeRtpSettings settings = { .ssrc = 7, .payload_type = 96, .clock_rate = 90000,
    .probation = 0, .reorder = TRUE, .latency_ms = TEST_STALL_MS };
  gint64 deadline = g_get_monotonic_time () + STALL_US;
  GstRuntimeRtpSession *session = gst_runtime_rtp_session_new_with_test_faststart (
      &settings, &sink, (GstRuntimeNotify) { notify, &state, NULL });
  g_assert_nonnull (session);
  test_before_playing = NULL;

  /* CAPS precedes the first input. Appsrc emits SEGMENT with A; holding CAPS
   * keeps that serialized event ahead of A during its original sink chain. */
  g_mutex_lock (&state.lock);
  while (!state.caps_entered)
    g_assert_true (g_cond_wait_until (&state.changed, &state.lock, deadline));
  g_mutex_unlock (&state.lock);
  const guint8 a[] = { 128, 96, 0, 1, 0, 0, 11, 184, 0, 0, 0, 8, 1 };
  g_assert_cmpint (gst_runtime_rtp_session_write (session, 1, a, sizeof (a)), ==, 0);
  g_mutex_lock (&state.lock);
  while (!state.chained)
    g_assert_true (g_cond_wait_until (&state.changed, &state.lock, deadline));
  g_print ("FORCED: CAPS held while A's original chain returned GST_FLOW_OK\n");
  state.release = TRUE;
  g_cond_broadcast (&state.changed);
  g_mutex_unlock (&state.lock);

  guint8 bytes[64];
  gsize length = 0;
  for (;;) {
    g_mutex_lock (&state.lock);
    guint before = state.generation;
    g_mutex_unlock (&state.lock);
    int result = gst_runtime_rtp_session_read (session, 1, bytes, sizeof (bytes), &length, 0);
    if (!result)
      break;
    g_assert_cmpint (result, ==, 1);
    g_mutex_lock (&state.lock);
    while (state.generation == before)
      g_assert_true (g_cond_wait_until (&state.changed, &state.lock, deadline));
    g_mutex_unlock (&state.lock);
  }
  g_assert_cmpmem (bytes, length, a, sizeof (a));
  gst_runtime_rtp_session_free (session);
  test_sink_assert_none (&facts);
  startup = NULL;
  g_cond_clear (&state.changed);
  g_mutex_clear (&state.lock);
  g_print ("PASS: exact A before any C after forced serialized startup\n");
  return 0;
}
