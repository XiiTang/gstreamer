#include "gstruntimepayload.h"

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
