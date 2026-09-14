/* Explicit RTSP endpoint state over a Runtime-supplied protected byte stream. */
#ifndef GST_RTSP_RUNTIME_CLIENT_H
#define GST_RTSP_RUNTIME_CLIENT_H
#include <gst/rtsp/gstrtspconnection.h>
G_BEGIN_DECLS

typedef struct _GstRTSPRuntimeClient GstRTSPRuntimeClient;
typedef enum
{
  GST_RTSP_RUNTIME_READY,
  GST_RTSP_RUNTIME_PLAYING,
  GST_RTSP_RUNTIME_RECORDING,
  GST_RTSP_RUNTIME_CLOSED,
  GST_RTSP_RUNTIME_UNKNOWN
} GstRTSPRuntimeState;
typedef enum
{
  GST_RTSP_RUNTIME_NOT_SENT,
  GST_RTSP_RUNTIME_MAYBE_SENT,
  GST_RTSP_RUNTIME_RESPONSE_RECEIVED
} GstRTSPRuntimeDispatch;

GST_RTSP_API
GstRTSPResult gst_rtsp_runtime_client_new (const GstRTSPUrl *url, GSocket *socket,
                                           GstRTSPVersion version, guint body_limit,
                                           GstRTSPRuntimeClient **client);
/* These operations are serialized by the native owner. No request, retry,
 * authentication replay, version fallback, reconnect or cleanup is implicit. */
GST_RTSP_API
GstRTSPResult gst_rtsp_runtime_client_request (GstRTSPRuntimeClient *client,
                                               GstRTSPMessage *request, gint64 timeout);
GST_RTSP_API
GstRTSPResult gst_rtsp_runtime_client_receive (GstRTSPRuntimeClient *client,
                                               GstRTSPMessage *message, gint64 timeout);
GST_RTSP_API
GstRTSPResult gst_rtsp_runtime_client_respond (GstRTSPRuntimeClient *client,
                                               GstRTSPMessage *response, gint64 timeout);
/* Polls without consuming bytes or changing protocol state. */
GST_RTSP_API
GstRTSPResult gst_rtsp_runtime_client_wait (GstRTSPRuntimeClient *client, gint64 timeout);
GST_RTSP_API
guint32 gst_rtsp_runtime_client_cseq (GstRTSPRuntimeClient *client);
GST_RTSP_API
GBytes *gst_rtsp_runtime_client_received_bytes (GstRTSPRuntimeClient *client);
GST_RTSP_API
GstRTSPRuntimeDispatch gst_rtsp_runtime_client_dispatch (GstRTSPRuntimeClient *client);
GST_RTSP_API
gboolean gst_rtsp_runtime_client_track_state (GstRTSPRuntimeClient *client, const gchar *session,
                                              const gchar *uri, GstRTSPRuntimeState *state);
/* Cancellation is the only operation allowed concurrently with receive/write.
 * The owner must join those operations before free. Neither sends TEARDOWN. */
GST_RTSP_API
void gst_rtsp_runtime_client_cancel (GstRTSPRuntimeClient *client);
GST_RTSP_API
void gst_rtsp_runtime_client_free (GstRTSPRuntimeClient *client);
G_END_DECLS
#endif
