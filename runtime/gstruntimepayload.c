#include "gstruntimepayload.h"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtp/gstrtpbuffer.h>

struct _GstRuntimePayload
{
  GstElement *pipeline;
  GstAppSrc *source;
  GstAppSink *sink;
  GstBus *bus;
  gboolean rtp_input;
};
static const gchar *factories[][2] = { { NULL, NULL },
                                       { "rtph264depay", "rtph264pay" },
                                       { "rtph265depay", "rtph265pay" },
                                       { "rtpjpegdepay", "rtpjpegpay" },
                                       { "rtpopusdepay", "rtpopuspay" },
                                       { "rtpmp4gdepay", "rtpmp4gpay" },
                                       { "rtppcmadepay", "rtppcmapay" },
                                       { "rtppcmudepay", "rtppcmupay" } };
GstElement *
gst_runtime_payload_transform_new (GstRuntimePayloadFormat format, gboolean sending, guint pt,
                                   guint32 ssrc, guint16 sequence, guint32 timestamp, guint mtu)
{
  if (format <= GST_RUNTIME_PAYLOAD_RAW || format > GST_RUNTIME_PAYLOAD_PCMU || pt > 127 || mtu < 28
      || mtu > 65507)
    return NULL;
  GstElement *element = gst_element_factory_make (factories[format][!!sending], NULL);
  if (element && sending)
    g_object_set (element, "pt", pt, "ssrc", ssrc, "seqnum-offset", (gint)sequence,
                  "timestamp-offset", timestamp, "mtu", mtu, NULL);
  return element;
}
GstCaps *
gst_runtime_payload_output_caps (GstRuntimePayloadFormat format)
{
  if (format != GST_RUNTIME_PAYLOAD_H264 && format != GST_RUNTIME_PAYLOAD_H265)
    return NULL;
  return gst_caps_new_simple (format == GST_RUNTIME_PAYLOAD_H264 ? "video/x-h264" : "video/x-h265",
                              "stream-format", G_TYPE_STRING, "byte-stream", "alignment",
                              G_TYPE_STRING, "au", NULL);
}
GstRuntimePayload *
gst_runtime_payload_new (GstRuntimePayloadFormat format, gboolean sending, const GstCaps *caps,
                         guint payload_type, guint32 ssrc, guint16 sequence, guint32 timestamp,
                         guint mtu, GError **error)
{
  if (format < GST_RUNTIME_PAYLOAD_RAW || format > GST_RUNTIME_PAYLOAD_PCMU || !caps
      || !gst_caps_is_fixed (caps) || payload_type > 127 || mtu < 28 || mtu > 65507)
    {
      g_set_error_literal (error, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION,
                           "Invalid declared RTP payload configuration");
      return NULL;
    }
  GstRuntimePayload *payload = g_new0 (GstRuntimePayload, 1);
  GstElement *source = gst_element_factory_make ("appsrc", NULL),
             *sink = gst_element_factory_make ("appsink", NULL);
  GstElement *transform = format == GST_RUNTIME_PAYLOAD_RAW
                              ? NULL
                              : gst_runtime_payload_transform_new (format, sending, payload_type,
                                                                   ssrc, sequence, timestamp, mtu);
  payload->pipeline = gst_pipeline_new (NULL);
  if (!source || !sink || (format != GST_RUNTIME_PAYLOAD_RAW && !transform))
    {
      if (source)
        gst_object_unref (source);
      if (sink)
        gst_object_unref (sink);
      if (transform)
        gst_object_unref (transform);
      g_set_error_literal (error, GST_CORE_ERROR, GST_CORE_ERROR_MISSING_PLUGIN,
                           "Required private RTP component is unavailable");
      gst_runtime_payload_free (payload);
      return NULL;
    }
  payload->source = GST_APP_SRC (source);
  payload->sink = GST_APP_SINK (sink);
  payload->rtp_input = !sending || format == GST_RUNTIME_PAYLOAD_RAW;
  g_object_set (source, "format", GST_FORMAT_TIME, "is-live", TRUE, "block", TRUE, "max-bytes",
                (guint64)(16 * 1024 * 1024), "max-buffers", (guint64)8, NULL);
  gst_app_src_set_caps (payload->source, (GstCaps *)caps);
  g_object_set (sink, "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, NULL);
  gst_app_sink_set_max_buffers (payload->sink, 8);
  gst_app_sink_set_max_bytes (payload->sink, 16 * 1024 * 1024);
  gst_app_sink_set_drop (payload->sink, FALSE);
  GstCaps *output = !sending ? gst_runtime_payload_output_caps (format) : NULL;
  if (output)
    {
      gst_app_sink_set_caps (payload->sink, output);
      gst_caps_unref (output);
    }

  gst_bin_add_many (GST_BIN (payload->pipeline), source, sink, NULL);
  gboolean linked;
  if (transform)
    {
      gst_bin_add (GST_BIN (payload->pipeline), transform);
      linked = gst_element_link_many (source, transform, sink, NULL);
    }
  else
    linked = gst_element_link (source, sink);
  payload->bus = gst_element_get_bus (payload->pipeline);
  if (!linked
      || gst_element_set_state (payload->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
      g_set_error_literal (error, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION,
                           "Cannot start the declared RTP payload path");
      gst_runtime_payload_free (payload);
      return NULL;
    }
  return payload;
}
GstFlowReturn
gst_runtime_payload_push (GstRuntimePayload *payload, GstBuffer *buffer)
{
  if (gst_buffer_get_size (buffer) > 16 * 1024 * 1024)
    {
      gst_buffer_unref (buffer);
      return GST_FLOW_ERROR;
    }
  if (payload->rtp_input)
    {
      if (gst_buffer_get_size (buffer) > G_MAXUINT16)
        {
          gst_buffer_unref (buffer);
          return GST_FLOW_ERROR;
        }
      GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
      if (!gst_rtp_buffer_map (buffer, GST_MAP_READ, &rtp))
        {
          gst_buffer_unref (buffer);
          return GST_FLOW_ERROR;
        }
      gst_rtp_buffer_unmap (&rtp);
    }
  return gst_app_src_push_buffer (payload->source, buffer);
}
GstSample *
gst_runtime_payload_pull (GstRuntimePayload *payload, GstClockTime timeout)
{
  return gst_app_sink_try_pull_sample (payload->sink, timeout);
}
GstMessage *
gst_runtime_payload_error (GstRuntimePayload *payload)
{
  return gst_bus_pop_filtered (payload->bus, GST_MESSAGE_ERROR);
}
void
gst_runtime_payload_stop (GstRuntimePayload *payload)
{
  if (payload && payload->pipeline)
    gst_element_set_state (payload->pipeline, GST_STATE_NULL);
}
void
gst_runtime_payload_free (GstRuntimePayload *payload)
{
  if (!payload)
    return;
  gst_runtime_payload_stop (payload);
  if (payload->bus)
    gst_object_unref (payload->bus);
  if (payload->pipeline)
    gst_object_unref (payload->pipeline);
  g_free (payload);
}
