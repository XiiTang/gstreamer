#ifndef GST_RUNTIME_RTP_SESSION_H
#define GST_RUNTIME_RTP_SESSION_H
#include "gstruntimepayloadapi.h"
#include <gst/gst.h>
G_BEGIN_DECLS
typedef struct _GstRuntimeRtpSession GstRuntimeRtpSession;
typedef struct
{
  guint32 payload_type, ssrc, peer_ssrc, peer_rtx_ssrc;
  guint32 cache_packets, cache_time_ms;
} GstRuntimeRtxSettings;
typedef struct
{
  guint32 ssrc, payload_type, clock_rate, probation;
  guint64 rtcp_min_interval;
  gboolean reports;
  guint32 feedback; /* bit 0 NACK, bit 1 PLI, bit 2 FIR */
  const GstRuntimeRtxSettings *rtx;
  const GstRuntimePayloadSettings *payload;
  gboolean reorder;
  guint32 latency_ms;
  double bandwidth_bps;
  const GstCaps *negotiated_caps;
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
/* 1 scheduled, 0 native source/timing declined, negative invalid declaration.
 * kind is one of 1 NACK, 2 PLI, 4 FIR; sequence/delay only apply to NACK. */
GST_API
int gst_runtime_rtp_session_feedback (GstRuntimeRtpSession *, guint32 kind, guint32 ssrc,
                                      guint16 sequence, guint64 max_delay);
/* One value of the native statistics structure, with the GType it holds.
 * STRUCTURE and LIST open a nesting whose fields or elements follow until its
 * END. name is the field's name, NULL for the outer structure and for a list
 * element; text is a STRING's value or a STRUCTURE's name. Pointers are valid
 * only during the visitor's call. */
typedef enum
{
  GST_RUNTIME_STATISTIC_INT = 0,     /* G_TYPE_INT in integer */
  GST_RUNTIME_STATISTIC_UINT = 1,    /* G_TYPE_UINT in unsigned_integer */
  GST_RUNTIME_STATISTIC_INT64 = 2,   /* G_TYPE_INT64 in integer */
  GST_RUNTIME_STATISTIC_UINT64 = 3,  /* G_TYPE_UINT64 in unsigned_integer */
  GST_RUNTIME_STATISTIC_DOUBLE = 4,  /* G_TYPE_DOUBLE in number */
  GST_RUNTIME_STATISTIC_BOOLEAN = 5, /* G_TYPE_BOOLEAN in boolean */
  GST_RUNTIME_STATISTIC_STRING = 6,
  GST_RUNTIME_STATISTIC_STRUCTURE = 7,
  GST_RUNTIME_STATISTIC_LIST = 8, /* GValueArray, GstValueList or GstValueArray */
  GST_RUNTIME_STATISTIC_END = 9,
} GstRuntimeStatisticKind;
typedef struct
{
  guint32 kind;
  const gchar *name;
  gint64 integer;
  guint64 unsigned_integer;
  gdouble number;
  gboolean boolean;
  const gchar *text;
} GstRuntimeStatistic;
/* Nonzero stops the walk. */
typedef int (*GstRuntimeStatisticVisitor) (const GstRuntimeStatistic *statistic,
                                           gpointer user_data);
/* Walks the native session statistics in field order: 0 walked, 1 the visitor
 * stopped, negative no statistics or a value of a type no kind names. */
GST_API
int gst_runtime_rtp_session_statistics (GstRuntimeRtpSession *session,
                                        GstRuntimeStatisticVisitor visitor, gpointer user_data);
GST_API
void gst_runtime_rtp_session_stop (GstRuntimeRtpSession *session);
GST_API
void gst_runtime_rtp_session_free (GstRuntimeRtpSession *session);
G_END_DECLS
#endif
