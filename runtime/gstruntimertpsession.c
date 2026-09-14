#include "gstruntimertpsession.h"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtpdefs.h>

struct _GstRuntimeRtpSession
{
  GstElement *pipeline, *rtp, *source[3], *sink[3];
  GObject *engine;
  GstBus *bus;
  GstRuntimeRtpSettings settings;
  gint stopped;
  GMutex writers[3];
};
static GstCaps *
pt_map (GstElement *element, guint pt, GstRuntimeRtpSession *s)
{
  (void)element;
  if (pt != s->settings.payload_type)
    return NULL;
  return gst_caps_new_simple ("application/x-rtp", "payload", G_TYPE_INT, (gint)pt, "clock-rate",
                              G_TYPE_INT, (gint)s->settings.clock_rate, NULL);
}
static gboolean
link_input (GstRuntimeRtpSession *s, int port, const gchar *name)
{
  GstPad *input = gst_element_request_pad_simple (s->rtp, name);
  GstPad *output = gst_element_get_static_pad (s->source[port], "src");
  gboolean ok = input && output && gst_pad_link (output, input) == GST_PAD_LINK_OK;
  if (input)
    gst_object_unref (input);
  if (output)
    gst_object_unref (output);
  return ok;
}
static gboolean
link_output (GstRuntimeRtpSession *s, int port, const gchar *name, gboolean request)
{
  GstPad *output = request ? gst_element_request_pad_simple (s->rtp, name)
                           : gst_element_get_static_pad (s->rtp, name);
  GstPad *input = gst_element_get_static_pad (s->sink[port], "sink");
  gboolean ok = output && input && gst_pad_link (output, input) == GST_PAD_LINK_OK;
  if (input)
    gst_object_unref (input);
  if (output)
    gst_object_unref (output);
  return ok;
}
GstRuntimeRtpSession *
gst_runtime_rtp_session_new (const GstRuntimeRtpSettings *settings)
{
  if (!settings || settings->payload_type > 127 || !settings->clock_rate
      || settings->clock_rate > G_MAXINT)
    return NULL;
  GstRuntimeRtpSession *s = g_new0 (GstRuntimeRtpSession, 1);
  s->settings = *settings;
  for (int i = 0; i < 3; i++)
    g_mutex_init (&s->writers[i]);
  s->pipeline = gst_pipeline_new (NULL);
  s->rtp = gst_element_factory_make ("rtpsession", NULL);
  if (!s->pipeline || !s->rtp)
    goto failed;
  gst_bin_add (GST_BIN (s->pipeline), s->rtp);
  s->bus = gst_element_get_bus (s->pipeline);
  g_object_get (s->rtp, "internal-session", &s->engine, NULL);
  if (!s->engine)
    goto failed;
  g_object_set (s->engine, "internal-ssrc", settings->ssrc, "probation", settings->probation,
                "rtcp-min-interval", settings->rtcp_min_interval, "rtp-profile",
                settings->feedback_profile ? GST_RTP_PROFILE_AVPF : GST_RTP_PROFILE_AVP,
                "update-ntp64-header-ext", FALSE, NULL);
  g_signal_connect (s->rtp, "request-pt-map", G_CALLBACK (pt_map), s);
  for (int i = 0; i < 3; i++)
    {
      s->source[i] = gst_element_factory_make ("appsrc", NULL);
      s->sink[i] = gst_element_factory_make ("appsink", NULL);
      if (!s->source[i] || !s->sink[i])
        goto failed;
      gst_bin_add_many (GST_BIN (s->pipeline), s->source[i], s->sink[i], NULL);
      GstCaps *caps = i == 2 ? gst_caps_new_empty_simple ("application/x-rtcp")
                             : pt_map (NULL, settings->payload_type, s);
      g_object_set (s->source[i], "caps", caps, "format", GST_FORMAT_TIME, "is-live", TRUE,
                    "do-timestamp", TRUE, "block", FALSE, "max-buffers", (guint64)32, "max-bytes",
                    (guint64)(32 * 65536), "max-time", (guint64)0, NULL);
      gst_caps_unref (caps);
      g_object_set (s->sink[i], "sync", FALSE, "async", FALSE, "max-buffers", 32u, "drop", FALSE,
                    "wait-on-eos", FALSE, "enable-last-sample", FALSE, NULL);
    }
  if (!link_input (s, 0, "send_rtp_sink") || !link_input (s, 1, "recv_rtp_sink")
      || !link_input (s, 2, "recv_rtcp_sink") || !link_output (s, 0, "send_rtp_src", FALSE)
      || !link_output (s, 1, "recv_rtp_src", FALSE)
      || (settings->reports && !link_output (s, 2, "send_rtcp_src", TRUE)))
    goto failed;
  if (gst_element_set_state (s->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    goto failed;
  return s;
failed:
  gst_runtime_rtp_session_free (s);
  return NULL;
}
int
gst_runtime_rtp_session_try_write (GstRuntimeRtpSession *s, int port, const guint8 *data,
                                   gsize length)
{
  if (!s || port < 0 || port > 2 || !data || !length || length > 65535)
    return GST_FLOW_ERROR;
  if (g_atomic_int_get (&s->stopped))
    return GST_FLOW_FLUSHING;
  GstBuffer *buffer = gst_buffer_new_allocate (NULL, length, NULL);
  gst_buffer_fill (buffer, 0, data, length);
  gboolean valid;
  if (port == 2)
    valid = gst_rtcp_buffer_validate (buffer);
  else
    {
      GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
      valid = gst_rtp_buffer_map (buffer, GST_MAP_READ, &rtp);
      if (valid)
        {
          valid = gst_rtp_buffer_get_payload_type (&rtp) == s->settings.payload_type
                  && (port != 0 || gst_rtp_buffer_get_ssrc (&rtp) == s->settings.ssrc);
          gst_rtp_buffer_unmap (&rtp);
        }
    }
  if (!valid)
    {
      gst_buffer_unref (buffer);
      return GST_FLOW_ERROR;
    }
  g_mutex_lock (&s->writers[port]);
  int result;
  if (gst_app_src_get_current_level_buffers (GST_APP_SRC (s->source[port])) >= 32)
    {
      gst_buffer_unref (buffer);
      result = 1;
    }
  else
    result = gst_app_src_push_buffer (GST_APP_SRC (s->source[port]), buffer);
  g_mutex_unlock (&s->writers[port]);
  return result;
}
int
gst_runtime_rtp_session_write (GstRuntimeRtpSession *s, int port, const guint8 *data, gsize length)
{
  int result;
  while ((result = gst_runtime_rtp_session_try_write (s, port, data, length)) == 1)
    g_usleep (1000);
  return result;
}
int
gst_runtime_rtp_session_read (GstRuntimeRtpSession *s, int port, guint8 *data, gsize capacity,
                              gsize *length, guint64 timeout)
{
  if (!s || port < 0 || port > 2 || !data || !length || (port == 2 && !s->settings.reports))
    return GST_FLOW_ERROR;
  *length = 0;
  GstSample *sample = gst_app_sink_try_pull_sample (GST_APP_SINK (s->sink[port]), timeout);
  if (!sample)
    {
      GstMessage *error = gst_bus_pop_filtered (s->bus, GST_MESSAGE_ERROR);
      if (error)
        {
          gst_message_unref (error);
          gst_runtime_rtp_session_stop (s);
          return GST_FLOW_ERROR;
        }
      return g_atomic_int_get (&s->stopped) ? GST_FLOW_FLUSHING : 1;
    }
  GstBuffer *buffer = gst_sample_get_buffer (sample);
  gsize size = gst_buffer_get_size (buffer);
  if (size > capacity)
    {
      gst_sample_unref (sample);
      return GST_FLOW_ERROR;
    }
  gst_buffer_extract (buffer, 0, data, size);
  *length = size;
  gst_sample_unref (sample);
  return 0;
}
gboolean
gst_runtime_rtp_session_report (GstRuntimeRtpSession *s, guint64 max_delay)
{
  gboolean scheduled = FALSE;
  if (s && s->settings.reports && !g_atomic_int_get (&s->stopped))
    g_signal_emit_by_name (s->engine, "send-rtcp-full", max_delay, &scheduled);
  return scheduled;
}
gchar *
gst_runtime_rtp_session_stats (GstRuntimeRtpSession *s)
{
  GstStructure *stats = NULL;
  if (!s || !s->engine)
    return NULL;
  g_object_get (s->rtp, "stats", &stats, NULL);
  if (!stats)
    return NULL;
  gchar *text = gst_structure_to_string (stats);
  gst_structure_free (stats);
  return text;
}
void
gst_runtime_rtp_session_stats_free (gchar *stats)
{
  g_free (stats);
}
void
gst_runtime_rtp_session_stop (GstRuntimeRtpSession *s)
{
  if (s && g_atomic_int_compare_and_exchange (&s->stopped, FALSE, TRUE) && s->pipeline)
    gst_element_set_state (s->pipeline, GST_STATE_NULL);
}
void
gst_runtime_rtp_session_free (GstRuntimeRtpSession *s)
{
  if (!s)
    return;
  gst_runtime_rtp_session_stop (s);
  for (int i = 0; i < 3; i++)
    {
      if (s->source[i] && !GST_OBJECT_PARENT (s->source[i]))
        gst_object_unref (s->source[i]);
      if (s->sink[i] && !GST_OBJECT_PARENT (s->sink[i]))
        gst_object_unref (s->sink[i]);
    }
  if (s->rtp && !GST_OBJECT_PARENT (s->rtp))
    gst_object_unref (s->rtp);
  gst_clear_object (&s->engine);
  gst_clear_object (&s->bus);
  gst_clear_object (&s->pipeline);
  for (int i = 0; i < 3; i++)
    g_mutex_clear (&s->writers[i]);
  g_free (s);
}
