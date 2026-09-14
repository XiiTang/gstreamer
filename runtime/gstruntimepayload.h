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
typedef struct _GstRuntimePayload GstRuntimePayload;
GST_API
GstElement *gst_runtime_payload_transform_new (GstRuntimePayloadFormat format, gboolean sending,
                                               guint pt, guint32 ssrc, guint16 sequence,
                                               guint32 timestamp, guint mtu);
GST_API
GstCaps *gst_runtime_payload_output_caps (GstRuntimePayloadFormat format);
/* Caps are supplied as typed native data, never as a pipeline expression.
 * This stage transforms already encoded frames; it never encodes or decodes. */
GST_API
GstRuntimePayload *gst_runtime_payload_new (GstRuntimePayloadFormat format, gboolean sending,
                                            const GstCaps *caps, guint payload_type, guint32 ssrc,
                                            guint16 sequence, guint32 timestamp, guint mtu,
                                            GError **error);
GST_API
GstFlowReturn gst_runtime_payload_push (GstRuntimePayload *payload, GstBuffer *buffer);
GST_API
GstSample *gst_runtime_payload_pull (GstRuntimePayload *payload, GstClockTime timeout);
GST_API
GstMessage *gst_runtime_payload_error (GstRuntimePayload *payload);
/* Stop is concurrent with blocked push/pull; the owner joins them before free. */
GST_API
void gst_runtime_payload_stop (GstRuntimePayload *payload);
GST_API
void gst_runtime_payload_free (GstRuntimePayload *payload);
G_END_DECLS
#endif
