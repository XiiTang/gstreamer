/* Scalar/opaque ABI for the Rust owner. No GObject layout crosses this API. */
#ifndef GST_RUNTIME_API_H
#define GST_RUNTIME_API_H
#include <gst/gst.h>
G_BEGIN_DECLS
typedef struct _GstRuntimeRtsp GstRuntimeRtsp;
typedef struct _GstRuntimeRtspMessage GstRuntimeRtspMessage;
typedef struct
{
  const char *name;
  const char *value;
} GstRuntimeHeader;
typedef struct
{
  int kind, status, channel, version;
  const char *method, *uri, *reason;
  const guint8 *body, *raw;
  gsize body_length, raw_length;
} GstRuntimeRtspMessageView;
GST_API
void gst_runtime_initialize (void);
/* Takes ownership of fd after g_socket_new_from_fd succeeds. The Rust owner
 * transfers a duplicated, connected local stream, never a remote address. */
GST_API
int gst_runtime_rtsp_new (const char *uri, int fd, int version, guint body_limit,
                          GstRuntimeRtsp **result, int *fd_taken);
GST_API
int gst_runtime_rtsp_request (GstRuntimeRtsp *client, const char *method, const char *uri,
                              const GstRuntimeHeader *headers, gsize header_count,
                              const guint8 *body, gsize length, gint64 timeout, guint32 *sequence,
                              int *dispatch);
GST_API
int gst_runtime_rtsp_respond (GstRuntimeRtsp *client, int status, const char *reason,
                              const GstRuntimeHeader *headers, gsize header_count,
                              const guint8 *body, gsize length, gint64 timeout);
GST_API
int gst_runtime_rtsp_wait (GstRuntimeRtsp *client, gint64 timeout);
GST_API
int gst_runtime_rtsp_receive (GstRuntimeRtsp *client, gint64 timeout,
                              GstRuntimeRtspMessage **result);
GST_API
void gst_runtime_rtsp_message_view (GstRuntimeRtspMessage *message,
                                    GstRuntimeRtspMessageView *view);
GST_API
int gst_runtime_rtsp_message_header (GstRuntimeRtspMessage *message, guint index,
                                     GstRuntimeHeader *header);
GST_API
void gst_runtime_rtsp_message_free (GstRuntimeRtspMessage *message);
GST_API
int gst_runtime_rtsp_state (GstRuntimeRtsp *client, const char *session, const char *uri);
GST_API
void gst_runtime_rtsp_cancel (GstRuntimeRtsp *client);
GST_API
void gst_runtime_rtsp_free (GstRuntimeRtsp *client);
G_END_DECLS
#endif
