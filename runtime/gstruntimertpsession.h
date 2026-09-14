#ifndef GST_RUNTIME_RTP_SESSION_H
#define GST_RUNTIME_RTP_SESSION_H
#include "gstruntimepayloadapi.h"
#include <gst/gst.h>
G_BEGIN_DECLS
typedef struct _GstRuntimeRtpSession GstRuntimeRtpSession;
typedef struct
{
  guint32 ssrc, payload_type, clock_rate, probation;
  guint64 rtcp_min_interval;
  gboolean reports, feedback_profile;
  const GstRuntimePayloadSettings *payload;
  gboolean reorder;
  guint32 latency_ms;
  double bandwidth_bps;
} GstRuntimeRtpSettings;
/* One transport RTP session. The same owner is used by direct RTP and a
 * negotiated RTSP track. No sockets, keys, encoders, or arbitrary pipelines. */
GST_API
GstRuntimeRtpSession *gst_runtime_rtp_session_new (const GstRuntimeRtpSettings *settings);
/* Input ports: 0 outbound RTP, 1 inbound RTP, 2 inbound RTCP. */
GST_API
int gst_runtime_rtp_session_write (GstRuntimeRtpSession *session, int port, const guint8 *data,
                                   gsize length);
/* Nonblocking admission: 1 means the bounded native input queue is full. */
GST_API
int gst_runtime_rtp_session_try_write (GstRuntimeRtpSession *session, int port, const guint8 *data,
                                       gsize length);
GST_API
int gst_runtime_rtp_session_try_write_frame (GstRuntimeRtpSession *session, const guint8 *data,
                                             gsize length, guint64 pts, guint64 duration);
GST_API
int gst_runtime_rtp_session_pull (GstRuntimeRtpSession *session, int port, guint64 timeout,
                                  GstRuntimePayloadFrame **frame);
/* Output ports: 0 outbound RTP, 1 accepted inbound RTP or encoded frames, 2 generated RTCP.
 * 0=packet, 1=timeout, negative=flow/error; native errors stop this session. */
GST_API
int gst_runtime_rtp_session_read (GstRuntimeRtpSession *session, int port, guint8 *data,
                                  gsize capacity, gsize *length, guint64 timeout);
/* Native RTCP scheduling result; permitted only when reports were declared. */
GST_API
gboolean gst_runtime_rtp_session_report (GstRuntimeRtpSession *session, guint64 max_delay);
GST_API
gchar *gst_runtime_rtp_session_stats (GstRuntimeRtpSession *session);
GST_API
void gst_runtime_rtp_session_stats_free (gchar *stats);
GST_API
void gst_runtime_rtp_session_stop (GstRuntimeRtpSession *session);
GST_API
void gst_runtime_rtp_session_free (GstRuntimeRtpSession *session);
G_END_DECLS
#endif
