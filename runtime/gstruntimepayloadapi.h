#ifndef GST_RUNTIME_PAYLOAD_API_H
#define GST_RUNTIME_PAYLOAD_API_H
#include "gstruntimepayload.h"
G_BEGIN_DECLS
typedef struct
{
  int format, sending;
  guint32 payload_type, ssrc, sequence, timestamp, mtu;
  guint32 clock_rate, channels, width, height;
  const guint8 *codec_data;
  gsize codec_data_length;
  const gchar *h264_parameter_sets, *h265_vps, *h265_sps, *h265_pps;
} GstRuntimePayloadSettings;
typedef struct _GstRuntimePayloadFrame GstRuntimePayloadFrame;
typedef struct
{
  const guint8 *data;
  gsize length;
  guint64 pts, duration;
} GstRuntimePayloadFrameView;
GST_API
GstRuntimePayload *gst_runtime_payload_create (const GstRuntimePayloadSettings *settings);
GST_API
int gst_runtime_payload_write (GstRuntimePayload *payload, const guint8 *data, gsize length,
                               guint64 pts, guint64 duration);
/* 0=sample, 1=no sample within timeout, negative=native flow/error. */
GST_API
int gst_runtime_payload_read (GstRuntimePayload *payload, guint64 timeout,
                              GstRuntimePayloadFrame **frame);
GST_API
void gst_runtime_payload_frame_view (GstRuntimePayloadFrame *frame,
                                     GstRuntimePayloadFrameView *view);
GST_API
void gst_runtime_payload_frame_free (GstRuntimePayloadFrame *frame);
G_END_DECLS
#endif
