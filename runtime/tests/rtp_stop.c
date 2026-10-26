/* Native stop checkpoint and a test-only fixture for the safe Rust owner. */
#ifdef RTP_STOP_FIXTURE
#include <gst/gst.h>
#undef GST_API
#define GST_API __attribute__((visibility("hidden")))
#endif
#define GST_RUNTIME_RTP_TEST_HOOKS
#include "../gstruntimertpsession.c"
#include "test_sink.h"
#include <gmodule.h>

typedef struct { GMutex lock; GCond changed; gboolean reached, release; } Gate;
static void gate_hold (Gate *g) {
  g_mutex_lock (&g->lock); g->reached = TRUE; g_cond_broadcast (&g->changed);
  while (!g->release) g_cond_wait (&g->changed, &g->lock);
  g_mutex_unlock (&g->lock);
}
static void gate_wait (Gate *g) {
  g_mutex_lock (&g->lock);
  while (!g->reached) g_cond_wait (&g->changed, &g->lock);
  g_mutex_unlock (&g->lock);
}
static void gate_release (Gate *g) {
  g_mutex_lock (&g->lock); g->release = TRUE; g_cond_broadcast (&g->changed); g_mutex_unlock (&g->lock);
}
typedef struct
{
  GstElement parent;
  Gate *null_gate;
  gboolean fail_up;
  gboolean fail;
  guint errors;
} StopElement;
typedef GstElementClass StopElementClass;
G_DEFINE_TYPE (StopElement, stop_element, GST_TYPE_ELEMENT);

static void
post_error (GstElement *element)
{
  GError *error = g_error_new_literal (GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, "stop fixture");
  gst_element_post_message (element, gst_message_new_error (GST_OBJECT (element), error, NULL));
  g_error_free (error);
}
static GstStateChangeReturn
change_state (GstElement *element, GstStateChange transition)
{
  StopElement *self = (StopElement *)element;
  if (transition == GST_STATE_CHANGE_NULL_TO_READY && self->fail_up)
    {
      GError *error = g_error_new_literal (GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT, "construction fixture");
      gst_element_post_message (element, gst_message_new_error (GST_OBJECT (element), error, NULL));
      g_error_free (error);
      return GST_STATE_CHANGE_FAILURE;
    }
  if (transition == GST_STATE_CHANGE_READY_TO_NULL)
    {
      if (self->null_gate) gate_hold (self->null_gate);
      for (guint i = 0; i < self->errors; i++)
        post_error (element);
      if (self->fail)
        return GST_STATE_CHANGE_FAILURE;
    }
  return GST_ELEMENT_CLASS (stop_element_parent_class)->change_state (element, transition);
}
static void
stop_element_class_init (StopElementClass *klass)
{
  klass->change_state = change_state;
}
static void
stop_element_init (StopElement *self)
{
}

/* Linked only into the regression fixture, never the private media artifact. */
G_MODULE_EXPORT GstRuntimeRtpSession *
test_rtp_stop_new (gboolean failure, guint queued, guint during, const GstRuntimeDiagnosticSink *sink)
{
  gst_init (NULL, NULL);
  GstRuntimeRtpSession *s = g_new0 (GstRuntimeRtpSession, 1);
  s->diagnostic_sink = *sink;
  g_mutex_init (&s->stop_mutex);
  g_cond_init (&s->stop_done);
  for (guint i = 0; i < 3; i++)
    g_mutex_init (&s->writers[i]);
  s->stop_outcome.state_change = GST_STATE_CHANGE_SUCCESS;
  s->pipeline = gst_pipeline_new (NULL);
  s->bus = gst_element_get_bus (s->pipeline);
  StopElement *child = g_object_new (stop_element_get_type (), NULL);
  child->fail = failure;
  child->errors = during;
  gst_bin_add (GST_BIN (s->pipeline), GST_ELEMENT (child));
  g_assert_cmpint (gst_element_set_state (s->pipeline, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
  for (guint i = 0; i < queued; i++)
    {
      GError *error = g_error_new_literal (GST_STREAM_ERROR, GST_STREAM_ERROR_DECODE, "queued fixture");
      gst_element_post_message (s->pipeline, gst_message_new_error (GST_OBJECT (s->pipeline), error, NULL));
      g_error_free (error);
    }
  return s;
}

G_MODULE_EXPORT void
test_rtp_stop_allow_free (GstRuntimeRtpSession *s)
{
  /* Permit disposal after the deliberately failing state transition. */
  GstIterator *children = gst_bin_iterate_elements (GST_BIN (s->pipeline));
  GValue value = G_VALUE_INIT;
  if (gst_iterator_next (children, &value) == GST_ITERATOR_OK)
    ((StopElement *)g_value_get_object (&value))->fail = FALSE;
  g_value_unset (&value);
  gst_iterator_free (children);
  gst_element_set_state (s->pipeline, GST_STATE_NULL);
}

static void fail_before_playing (GstElement *pipeline)
{
  StopElement *child = g_object_new (stop_element_get_type (), NULL);
  child->fail_up = TRUE;
  gst_bin_add (GST_BIN (pipeline), GST_ELEMENT (child));
}
G_MODULE_EXPORT GstRuntimeRtpSession *test_rtp_failing_new (const GstRuntimeDiagnosticSink *sink)
{
  GstRuntimeRtpSettings settings = { .ssrc = 7, .payload_type = 8, .clock_rate = 8000 };
  test_before_playing = fail_before_playing;
  GstRuntimeRtpSession *s = gst_runtime_rtp_session_new (&settings, sink);
  test_before_playing = NULL;
  return s;
}

#ifndef RTP_STOP_FIXTURE
static StopElement *child_of (GstRuntimeRtpSession *s)
{
  GstIterator *children = gst_bin_iterate_elements (GST_BIN (s->pipeline));
  GValue value = G_VALUE_INIT;
  g_assert_cmpint (gst_iterator_next (children, &value), ==, GST_ITERATOR_OK);
  StopElement *child = g_value_get_object (&value);
  g_value_unset (&value);
  gst_iterator_free (children);
  return child; /* pipeline owns it */
}
static gpointer stop_thread (gpointer data) { gst_runtime_rtp_session_stop (data); return NULL; }
static gpointer post_decode (gpointer data)
{
  GstRuntimeRtpSession *s = data;
  GError *error = g_error_new_literal (GST_STREAM_ERROR, GST_STREAM_ERROR_DECODE, "delayed fixture");
  gst_bus_post (s->bus, gst_message_new_error (GST_OBJECT (s->pipeline), error, NULL));
  g_error_free (error);
  return NULL;
}
static void hold_first (GstBus *bus, GstMessage *message, gpointer data)
{
  Gate *g = data;
  g_mutex_lock (&g->lock);
  gboolean first = !g->reached;
  g_mutex_unlock (&g->lock);
  if (first) gate_hold (g);
}
static void delayed_poster_is_first (void)
{
  TestSink facts = {0}; GstRuntimeDiagnosticSink sink = test_sink (&facts);
  GstRuntimeRtpSession *s = test_rtp_stop_new (FALSE, 0, 1, &sink);
  Gate posted = {0}, in_null = {0}; child_of (s)->null_gate = &in_null;
  gst_bus_enable_sync_message_emission (s->bus);
  g_signal_connect (s->bus, "sync-message::error", G_CALLBACK (hold_first), &posted);
  GThread *p = g_thread_new ("delayed-post", post_decode, s); gate_wait (&posted);
  GThread *stop = g_thread_new ("stop", stop_thread, s); gate_wait (&in_null);
  gate_release (&posted); g_thread_join (p);
  gate_release (&in_null); g_thread_join (stop);
  GstRuntimeRtpStopOutcome outcome;
  g_assert_cmpint (gst_runtime_rtp_session_close (s, &outcome), ==, 0);
  g_assert_cmpint (facts.first_code, ==, GST_STREAM_ERROR_DECODE);
  g_assert_cmpuint (facts.count, ==, 2); g_assert_cmpuint (outcome.bus_errors, ==, 2);
  gst_runtime_rtp_session_free (s); g_free (facts.first_domain);
}
typedef struct {
  GMutex lock; GCond changed; GThread *poster;
  gboolean p_entered, p_release, s_waiting, s_completed;
  gint seq, offer_seq, close_seq;
  TestSink facts; GstRuntimeRtpSession *s; GstRuntimeRtpStopOutcome outcome;
} Probe;
static Probe *probe;
static void handler_entered (GstMessage *message)
{
  g_mutex_lock (&probe->lock);
  if (g_thread_self () == probe->poster) {
    probe->p_entered = TRUE; g_cond_broadcast (&probe->changed);
    while (!probe->p_release) g_cond_wait (&probe->changed, &probe->lock);
  }
  g_mutex_unlock (&probe->lock);
}
static void release_waiting (void) {
  g_mutex_lock (&probe->lock); probe->s_waiting = TRUE;
  g_cond_broadcast (&probe->changed); g_mutex_unlock (&probe->lock);
}
static void stop_completed (void) {
  g_mutex_lock (&probe->lock); probe->s_completed = TRUE;
  g_cond_broadcast (&probe->changed); g_mutex_unlock (&probe->lock);
}
static void probe_offer (gpointer data, const gchar *domain, gint code, const gchar *message, gsize length) {
  Probe *p = data; test_sink_offer (&p->facts, domain, code, message, length);
  p->offer_seq = g_atomic_int_add (&p->seq, 1) + 1;
}
static gpointer held_post (gpointer data) {
  Probe *p = data; g_mutex_lock (&p->lock); p->poster = g_thread_self (); g_mutex_unlock (&p->lock);
  return post_decode (p->s);
}
static gpointer close_thread (gpointer data) {
  Probe *p = data;
  g_assert_cmpint (gst_runtime_rtp_session_close (p->s, &p->outcome), ==, 0);
  p->close_seq = g_atomic_int_add (&p->seq, 1) + 1; return NULL;
}
static void close_waits_for_an_in_flight_handler_post (void)
{
  Probe p = {0}; probe = &p;
  GstRuntimeDiagnosticSink sink = { probe_offer, &p };
  p.s = test_rtp_stop_new (FALSE, 0, 0, &sink);
  Gate in_null = {0}; child_of (p.s)->null_gate = &in_null;
  test_handler_entered = handler_entered; test_release_waiting = release_waiting; test_stop_completed = stop_completed;
  GThread *stop = g_thread_new ("stop", stop_thread, p.s); gate_wait (&in_null);
  GThread *post = g_thread_new ("held-post", held_post, &p);
  g_mutex_lock (&p.lock);
  while (!p.p_entered) g_cond_wait (&p.changed, &p.lock);
  g_mutex_unlock (&p.lock);
  gate_release (&in_null);
  g_mutex_lock (&p.lock);
  while (!p.s_waiting && !p.s_completed) g_cond_wait (&p.changed, &p.lock);
  g_assert_true (p.s_waiting); g_assert_false (p.s_completed);
  g_mutex_unlock (&p.lock);
  GThread *close = g_thread_new ("close", close_thread, &p);
  g_mutex_lock (&p.lock); p.p_release = TRUE; g_cond_broadcast (&p.changed); g_mutex_unlock (&p.lock);
  g_thread_join (post); g_thread_join (stop); g_thread_join (close);
  g_assert_cmpint (p.offer_seq, <, p.close_seq); g_assert_true (p.s_completed);
  g_assert_cmpuint (p.outcome.bus_errors, ==, 1); g_assert_cmpuint (p.facts.count, ==, 1);
  g_assert_cmpint (p.facts.first_code, ==, GST_STREAM_ERROR_DECODE);
  test_handler_entered = NULL; test_release_waiting = NULL; test_stop_completed = NULL;
  gst_runtime_rtp_session_free (p.s); g_free (p.facts.first_domain); probe = NULL;
}
static void queued_error_one_consumer (gboolean stop_first)
{
  TestSink facts = {0}; GstRuntimeDiagnosticSink sink = test_sink (&facts);
  GstRuntimeRtpSession *s = test_rtp_stop_new (FALSE, 1, 0, &sink);
  s->sink[1] = gst_element_factory_make ("appsink", NULL);
  gst_element_set_state (s->sink[1], GST_STATE_READY);
  if (stop_first) gst_runtime_rtp_session_stop (s);
  GstRuntimePayloadFrame *frame = NULL;
  g_assert_cmpint (gst_runtime_rtp_session_pull (s, 1, 0, &frame), ==,
                   stop_first ? GST_FLOW_FLUSHING : GST_FLOW_CUSTOM_ERROR);
  GstRuntimeRtpStopOutcome outcome;
  g_assert_cmpint (gst_runtime_rtp_session_close (s, &outcome), ==, 0);
  g_assert_cmpuint (facts.count, ==, 1); g_assert_cmpint (facts.first_code, ==, GST_STREAM_ERROR_DECODE);
  g_assert_cmpuint (outcome.bus_errors, ==, stop_first ? 1 : 0);
  gst_element_set_state (s->sink[1], GST_STATE_NULL);
  gst_runtime_rtp_session_free (s); g_free (facts.first_domain);
}
static void pull_takes_a_queued_error (void) { queued_error_one_consumer (FALSE); }
static void stop_takes_a_queued_error (void) { queued_error_one_consumer (TRUE); }
static void failed_construction_offers_its_bus_error (void)
{
  TestSink facts = {0}; GstRuntimeDiagnosticSink sink = test_sink (&facts);
  g_assert_null (test_rtp_failing_new (&sink));
  g_assert_cmpuint (facts.count, ==, 1);
  g_assert_cmpstr (facts.first_domain, ==, "gst-stream-error-quark");
  g_assert_cmpint (facts.first_code, ==, GST_STREAM_ERROR_FORMAT);
  g_free (facts.first_domain);
}
int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  for (guint failure = 0; failure < 2; failure++)
    for (guint cancel_first = 0; cancel_first < 2; cancel_first++)
      {
        TestSink facts = {0}; GstRuntimeDiagnosticSink sink = test_sink (&facts);
        GstRuntimeRtpSession *s = test_rtp_stop_new (failure, 2, 3, &sink);
        GstRuntimeRtpStopOutcome outcome;
        if (cancel_first)
          gst_runtime_rtp_session_stop (s);
        g_assert_cmpint (gst_runtime_rtp_session_close (s, &outcome), ==, 0);
        g_assert_cmpint (outcome.state_change, ==,
                        failure ? GST_STATE_CHANGE_FAILURE : GST_STATE_CHANGE_SUCCESS);
        g_assert_cmpuint (outcome.bus_errors, ==, 5);
        g_assert_cmpuint (facts.count, ==, 5);
        g_assert_cmpint (facts.first_code, ==, GST_STREAM_ERROR_DECODE);
        g_assert_cmpint (gst_runtime_rtp_session_close (s, &outcome), ==, GST_FLOW_ERROR);
        test_rtp_stop_allow_free (s);
        gst_runtime_rtp_session_free (s);
        g_free (facts.first_domain);
      }
  delayed_poster_is_first ();
  close_waits_for_an_in_flight_handler_post ();
  pull_takes_a_queued_error ();
  stop_takes_a_queued_error ();
  failed_construction_offers_its_bus_error ();
  g_print ("PASS RTP stop: NULL result and every queued/during-stop error, stop first and close first\n");
  return 0;
}
#endif
