#ifndef GST_RUNTIME_PAYLOAD_H
#define GST_RUNTIME_PAYLOAD_H
#include <gst/gst.h>
G_BEGIN_DECLS

typedef enum
{
  GST_RUNTIME_PAYLOAD_RAW,
  GST_RUNTIME_PAYLOAD_H264,
  GST_RUNTIME_PAYLOAD_H265,
  GST_RUNTIME_PAYLOAD_JPEG,
  GST_RUNTIME_PAYLOAD_OPUS,
  GST_RUNTIME_PAYLOAD_AAC,
  GST_RUNTIME_PAYLOAD_PCMA,
  GST_RUNTIME_PAYLOAD_PCMU
} GstRuntimePayloadFormat;
GST_API
GstElement *gst_runtime_payload_transform_new (GstRuntimePayloadFormat format, gboolean sending,
                                               guint pt, guint32 ssrc, guint16 sequence,
                                               guint32 timestamp, guint mtu);
GST_API
GstCaps *gst_runtime_payload_output_caps (GstRuntimePayloadFormat format);
G_END_DECLS
#endif
