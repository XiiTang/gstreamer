#include "gstruntimeapi.h"
#include <gst/rtsp/gstrtspruntimeclient.h>

struct _GstRuntimeRtsp
{
  GstRTSPRuntimeClient *client;
  GstRTSPVersion version;
};
struct _GstRuntimeRtspMessage
{
  GstRTSPMessage message;
  GBytes *raw;
};

void
gst_runtime_initialize (void)
{
  gst_init (NULL, NULL);
}

int
gst_runtime_rtsp_new (const char *uri, int fd, int version, guint body_limit,
                      GstRuntimeRtsp **output, int *fd_taken)
{
  *output = NULL;
  *fd_taken = FALSE;
  if (version != 1 && version != 2)
    return GST_RTSP_EINVAL;
  GstRTSPUrl *url = NULL;
  GstRTSPResult result = gst_rtsp_url_parse (uri, &url);
  if (result != GST_RTSP_OK)
    return result;
  GError *error = NULL;
  GSocket *socket = g_socket_new_from_fd (fd, &error);
  if (!socket)
    {
      g_clear_error (&error);
      gst_rtsp_url_free (url);
      return GST_RTSP_ESYS;
    }
  *fd_taken = TRUE;
  GstRuntimeRtsp *client = g_new0 (GstRuntimeRtsp, 1);
  client->version = version == 1 ? GST_RTSP_VERSION_1_0 : GST_RTSP_VERSION_2_0;
  result = gst_rtsp_runtime_client_new (url, socket, client->version, body_limit, &client->client);
  gst_rtsp_url_free (url);
  g_object_unref (socket);
  if (result == GST_RTSP_OK)
    *output = client;
  else
    g_free (client);
  return result;
}

static GstRTSPResult
fields (GstRTSPMessage *message, const GstRuntimeHeader *headers, gsize count, const guint8 *body,
        gsize length)
{
  if (length > G_MAXUINT || (count && !headers) || (length && !body))
    return GST_RTSP_EINVAL;
  for (gsize i = 0; i < count; i++)
    {
      if (!headers[i].name || !headers[i].value)
        return GST_RTSP_EINVAL;
      GstRTSPResult result
          = gst_rtsp_message_add_header_by_name (message, headers[i].name, headers[i].value);
      if (result != GST_RTSP_OK)
        return result;
    }
  if (length)
    return gst_rtsp_message_set_body (message, body, length);
  return GST_RTSP_OK;
}

int
gst_runtime_rtsp_request (GstRuntimeRtsp *client, const char *method, const char *uri,
                          const GstRuntimeHeader *headers, gsize count, const guint8 *body,
                          gsize length, gint64 timeout, guint32 *sequence, int *dispatch)
{
  GstRTSPMessage message = { 0 };
  *sequence = 0;
  *dispatch = GST_RTSP_RUNTIME_NOT_SENT;
  GstRTSPMethod parsed = gst_rtsp_find_method (method);
  if (parsed == GST_RTSP_INVALID)
    return GST_RTSP_EINVAL;
  GstRTSPResult result = gst_rtsp_message_init_request (&message, parsed, uri);
  message.type_data.request.version = client->version;
  if (result == GST_RTSP_OK)
    result = fields (&message, headers, count, body, length);
  if (result == GST_RTSP_OK)
    {
      result = gst_rtsp_runtime_client_request (client->client, &message, timeout);
      if (result != GST_RTSP_EINVAL)
        {
          *sequence = gst_rtsp_runtime_client_cseq (client->client);
          *dispatch = gst_rtsp_runtime_client_dispatch (client->client);
        }
    }
  gst_rtsp_message_unset (&message);
  return result;
}

int
gst_runtime_rtsp_respond (GstRuntimeRtsp *client, int status, const char *reason,
                          const GstRuntimeHeader *headers, gsize count, const guint8 *body,
                          gsize length, gint64 timeout)
{
  if (status < 100 || status > 999)
    return GST_RTSP_EINVAL;
  GstRTSPMessage message = { 0 };
  GstRTSPResult result = gst_rtsp_message_init_response (&message, status, reason, NULL);
  message.type_data.response.version = client->version;
  if (result == GST_RTSP_OK)
    result = fields (&message, headers, count, body, length);
  if (result == GST_RTSP_OK)
    result = gst_rtsp_runtime_client_respond (client->client, &message, timeout);
  gst_rtsp_message_unset (&message);
  return result;
}

int
gst_runtime_rtsp_wait (GstRuntimeRtsp *client, gint64 timeout)
{
  return gst_rtsp_runtime_client_wait (client->client, timeout);
}
int
gst_runtime_rtsp_receive (GstRuntimeRtsp *client, gint64 timeout, GstRuntimeRtspMessage **output)
{
  *output = NULL;
  GstRuntimeRtspMessage *message = g_new0 (GstRuntimeRtspMessage, 1);
  int result = gst_rtsp_runtime_client_receive (client->client, &message->message, timeout);
  /* Even a malformed response is returned as raw evidence, with the error. */
  message->raw = gst_rtsp_runtime_client_received_bytes (client->client);
  *output = message;
  return result;
}
void
gst_runtime_rtsp_message_view (GstRuntimeRtspMessage *message, GstRuntimeRtspMessageView *view)
{
  *view = (GstRuntimeRtspMessageView){ 0 };
  GstRTSPMessage *value = &message->message;
  view->kind = value->type;
  if (value->type == GST_RTSP_MESSAGE_REQUEST)
    {
      view->method = gst_rtsp_method_as_text (value->type_data.request.method);
      view->uri = value->type_data.request.uri;
      view->version = value->type_data.request.version;
    }
  else if (value->type == GST_RTSP_MESSAGE_RESPONSE)
    {
      view->status = value->type_data.response.code;
      view->reason = value->type_data.response.reason;
      view->version = value->type_data.response.version;
    }
  else if (value->type == GST_RTSP_MESSAGE_DATA)
    {
      view->channel = value->type_data.data.channel;
    }
  guint length = 0;
  guint8 *body = NULL;
  gst_rtsp_message_get_body (value, &body, &length);
  view->body = body;
  /* GstRTSPConnection appends a convenience NUL and includes it in
   * GstRTSPMessage.body_size, for both text bodies and interleaved data.
   * This result is exclusively receive-created; expose only wire bytes. */
  view->body_length = length ? length - 1 : 0;
  if (message->raw)
    view->raw = g_bytes_get_data (message->raw, &view->raw_length);
}
int
gst_runtime_rtsp_message_header (GstRuntimeRtspMessage *message, guint index,
                                 GstRuntimeHeader *header)
{
  return gst_rtsp_message_header_at (&message->message, index, &header->name, &header->value);
}
void
gst_runtime_rtsp_message_free (GstRuntimeRtspMessage *message)
{
  if (!message)
    return;
  gst_rtsp_message_unset (&message->message);
  g_clear_pointer (&message->raw, g_bytes_unref);
  g_free (message);
}
int
gst_runtime_rtsp_state (GstRuntimeRtsp *client, const char *session, const char *uri)
{
  GstRTSPRuntimeState state;
  return gst_rtsp_runtime_client_track_state (client->client, session, uri, &state) ? (int)state
                                                                                    : -1;
}
void
gst_runtime_rtsp_cancel (GstRuntimeRtsp *client)
{
  gst_rtsp_runtime_client_cancel (client->client);
}
void
gst_runtime_rtsp_free (GstRuntimeRtsp *client)
{
  gst_rtsp_runtime_client_free (client->client);
  g_free (client);
}
