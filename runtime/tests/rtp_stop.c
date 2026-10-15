/* Native stop checkpoint and a test-only fixture for the safe Rust owner. */
#ifdef RTP_STOP_FIXTURE
#include <gst/gst.h>
#undef GST_API
#define GST_API __attribute__((visibility("hidden")))
#endif
#include "../gstruntimertpsession.c"
#include <gmodule.h>

typedef struct
{
  GstElement parent;
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
  if (transition == GST_STATE_CHANGE_READY_TO_NULL)
    {
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
test_rtp_stop_new (gboolean failure, guint queued, guint during)
{
  gst_init (NULL, NULL);
  GstRuntimeRtpSession *s = g_new0 (GstRuntimeRtpSession, 1);
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
    post_error (s->pipeline);
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

#ifndef RTP_STOP_FIXTURE
int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  for (guint failure = 0; failure < 2; failure++)
    for (guint cancel_first = 0; cancel_first < 2; cancel_first++)
      {
        GstRuntimeRtpSession *s = test_rtp_stop_new (failure, 2, 3);
        GstRuntimeRtpStopOutcome outcome;
        if (cancel_first)
          gst_runtime_rtp_session_stop (s);
        g_assert_cmpint (gst_runtime_rtp_session_close (s, &outcome), ==, 0);
        g_assert_cmpint (outcome.state_change, ==,
                        failure ? GST_STATE_CHANGE_FAILURE : GST_STATE_CHANGE_SUCCESS);
        g_assert_cmpuint (outcome.bus_errors, ==, 5);
        g_assert_cmpint (gst_runtime_rtp_session_close (s, &outcome), ==, GST_FLOW_ERROR);
        test_rtp_stop_allow_free (s);
        gst_runtime_rtp_session_free (s);
      }
  g_print ("PASS RTP stop: NULL result and every queued/during-stop error, stop first and close first\n");
  return 0;
}
#endif
