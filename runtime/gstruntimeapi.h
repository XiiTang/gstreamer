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
typedef struct
{
  int profile, lower_transport, mode_play, mode_record, rtcp_mux;
  int interleaved_first, interleaved_last;
  int client_first, client_last, server_first, server_last;
  const char *source, *destination;
  const char *src_host[2], *dest_host[2];
  guint32 src_port[2], dest_port[2];
  guint32 src_count, dest_count, ssrc_count;
  const guint32 *ssrcs;
  guint64 generation;
} GstRuntimeRtspTransportView;
/* View pointers are borrowed until the next serialized client operation. */
GST_API
int gst_runtime_rtsp_transport (GstRuntimeRtsp *client, const char *session, const char *uri,
                                GstRuntimeRtspTransportView *view);
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
int gst_runtime_rtsp_receive_step (GstRuntimeRtsp *client, GstRuntimeRtspMessage **result);
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
