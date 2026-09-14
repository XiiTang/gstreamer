#include "gstrtspruntimeclient.h"
#include "gstrtsptransport.h"
#include <string.h>

typedef struct
{
  GstRTSPRuntimeState state;
  GstRTSPTransport *transport;
  guint64 generation;
} RuntimeTrack;
static void
track_free (gpointer value)
{
  RuntimeTrack *track = value;
  gst_rtsp_transport_free (track->transport);
  g_free (track);
}
typedef struct
{
  gchar *aggregate;
  GHashTable *tracks;
} RuntimeSession;
struct _GstRTSPRuntimeClient
{
  GstRTSPConnection *connection;
  GstRTSPVersion version;
  gchar *root;
  GHashTable *sessions;
  gboolean pending, unknown;
  guint32 cseq;
  GstRTSPMethod method;
  gchar *uri, *session;
  GstRTSPRuntimeDispatch dispatch;
};
static void
session_free (gpointer data)
{
  RuntimeSession *session = data;
  g_free (session->aggregate);
  g_hash_table_unref (session->tracks);
  g_free (session);
}
static void
pending_clear (GstRTSPRuntimeClient *client)
{
  client->pending = FALSE;
  g_clear_pointer (&client->uri, g_free);
  g_clear_pointer (&client->session, g_free);
}
static RuntimeSession *
lookup (GstRTSPRuntimeClient *client, const gchar *id)
{
  return id ? g_hash_table_lookup (client->sessions, id) : NULL;
}
static void
transition (GstRTSPRuntimeClient *client, RuntimeSession *session, GstRTSPRuntimeState state)
{
  if (!session)
    return;
  if (!strcmp (client->uri, session->aggregate))
    {
      GHashTableIter iter;
      gpointer key, value;
      g_hash_table_iter_init (&iter, session->tracks);
      while (g_hash_table_iter_next (&iter, &key, &value))
        if (((RuntimeTrack *)value)->state != GST_RTSP_RUNTIME_CLOSED)
          ((RuntimeTrack *)value)->state = state;
    }
  else if (g_hash_table_contains (session->tracks, client->uri))
    {
      ((RuntimeTrack *)g_hash_table_lookup (session->tracks, client->uri))->state = state;
    }
}
static GstRTSPResult
unknown (GstRTSPRuntimeClient *client, GstRTSPResult result)
{
  client->unknown = TRUE;
  if (client->pending)
    transition (client, lookup (client, client->session), GST_RTSP_RUNTIME_UNKNOWN);
  return result;
}
static gboolean
valid_uri (const gchar *value)
{
  if (!value || !value[0])
    return FALSE;
  for (const guchar *p = (const guchar *)value; *p; p++)
    if (*p <= 0x20 || *p == 0x7f)
      return FALSE;
  return TRUE;
}
static gchar *
session_header (GstRTSPMessage *message)
{
  gchar *value = NULL;
  if (gst_rtsp_message_get_header (message, GST_RTSP_HDR_SESSION, &value, 0) != GST_RTSP_OK)
    return NULL;
  if (gst_rtsp_message_get_header (message, GST_RTSP_HDR_SESSION, NULL, 1) == GST_RTSP_OK)
    return NULL;
  gsize length = strcspn (value, ";");
  if (!length)
    return NULL;
  for (gsize i = 0; i < length; i++)
    if (!g_ascii_isalnum (value[i]) && !strchr ("-_.+", value[i]))
      return NULL;
  return g_strndup (value, length);
}
GstRTSPResult
gst_rtsp_runtime_client_new (const GstRTSPUrl *url, GSocket *socket, GstRTSPVersion version,
                             guint limit, GstRTSPRuntimeClient **output)
{
  g_return_val_if_fail (output && url, GST_RTSP_EINVAL);
  *output = NULL;
  if (version != GST_RTSP_VERSION_1_0 && version != GST_RTSP_VERSION_2_0)
    return GST_RTSP_EINVAL;
  GstRTSPRuntimeClient *client = g_new0 (GstRTSPRuntimeClient, 1);
  GstRTSPResult result
      = gst_rtsp_connection_create_runtime_client (url, socket, limit, &client->connection);
  if (result != GST_RTSP_OK)
    {
      g_free (client);
      return result;
    }
  client->version = version;
  client->root = gst_rtsp_url_get_request_uri (url);
  client->sessions = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, session_free);
  *output = client;
  return GST_RTSP_OK;
}
GstRTSPResult
gst_rtsp_runtime_client_request (GstRTSPRuntimeClient *client, GstRTSPMessage *request,
                                 gint64 timeout)
{
  g_return_val_if_fail (client && request, GST_RTSP_EINVAL);
  if (client->pending || client->unknown || !gst_rtsp_message_is_safe_to_serialize (request)
      || request->type != GST_RTSP_MESSAGE_REQUEST
      || request->type_data.request.version != client->version
      || !valid_uri (request->type_data.request.uri))
    return GST_RTSP_EINVAL;
  GstRTSPMethod method = request->type_data.request.method;
  switch (method)
    {
    case GST_RTSP_OPTIONS:
    case GST_RTSP_DESCRIBE:
    case GST_RTSP_SETUP:
    case GST_RTSP_PLAY:
    case GST_RTSP_PAUSE:
    case GST_RTSP_TEARDOWN:
    case GST_RTSP_GET_PARAMETER:
    case GST_RTSP_SET_PARAMETER:
      break;
    case GST_RTSP_ANNOUNCE:
    case GST_RTSP_RECORD:
      if (client->version != GST_RTSP_VERSION_1_0)
        return GST_RTSP_EINVAL;
      break;
    default:
      return GST_RTSP_EINVAL;
    }
  if (gst_rtsp_message_get_header (request, GST_RTSP_HDR_CSEQ, NULL, 0) == GST_RTSP_OK
      || gst_rtsp_message_get_header (request, GST_RTSP_HDR_CONTENT_LENGTH, NULL, 0) == GST_RTSP_OK)
    return GST_RTSP_EINVAL;
  gchar *id = session_header (request);
  RuntimeSession *session = lookup (client, id);
  if ((id && !session)
      || (!id
          && gst_rtsp_message_get_header (request, GST_RTSP_HDR_SESSION, NULL, 0) == GST_RTSP_OK))
    {
      g_free (id);
      return GST_RTSP_EINVAL;
    }
  if (method == GST_RTSP_PLAY || method == GST_RTSP_PAUSE || method == GST_RTSP_RECORD
      || method == GST_RTSP_TEARDOWN)
    {
      if (!session
          || (strcmp (request->type_data.request.uri, session->aggregate)
              && !g_hash_table_contains (session->tracks, request->type_data.request.uri)))
        {
          g_free (id);
          return GST_RTSP_EINVAL;
        }
    }
  if (session && method != GST_RTSP_SETUP)
    {
      RuntimeTrack *track = g_hash_table_lookup (session->tracks, request->type_data.request.uri);
      if (track && track->state == GST_RTSP_RUNTIME_CLOSED)
        {
          g_free (id);
          return GST_RTSP_EINVAL;
        }
    }
  guint32 sequence = gst_rtsp_connection_next_cseq (client->connection);
  if (!sequence)
    {
      g_free (id);
      return GST_RTSP_EINVAL;
    }
  client->method = method;
  client->uri = g_strdup (request->type_data.request.uri);
  client->session = id;
  client->cseq = sequence;
  client->pending = TRUE;
  client->dispatch = GST_RTSP_RUNTIME_NOT_SENT;
  GstRTSPResult result = gst_rtsp_connection_send_usec (client->connection, request, timeout);
  if (result == GST_RTSP_OK || gst_rtsp_connection_written_bytes (client->connection) > 0)
    client->dispatch = GST_RTSP_RUNTIME_MAYBE_SENT;
  if (result != GST_RTSP_OK)
    {
      if (client->dispatch == GST_RTSP_RUNTIME_NOT_SENT)
        pending_clear (client);
      else
        unknown (client, result);
    }
  return result;
}
static GstRTSPResult
receive_result (GstRTSPRuntimeClient *client, GstRTSPMessage *message, GstRTSPResult result)
{
  if (result != GST_RTSP_OK)
    return unknown (client, result);
  if (message->type == GST_RTSP_MESSAGE_DATA)
    return GST_RTSP_OK;
  if (message->type == GST_RTSP_MESSAGE_REQUEST)
    return message->type_data.request.version == client->version
               ? GST_RTSP_OK
               : unknown (client, GST_RTSP_EPARSE);
  if (message->type != GST_RTSP_MESSAGE_RESPONSE || !client->pending
      || message->type_data.response.version != client->version)
    return unknown (client, GST_RTSP_EPARSE);
  gchar *sequence = NULL;
  if (gst_rtsp_message_get_header (message, GST_RTSP_HDR_CSEQ, &sequence, 0) != GST_RTSP_OK
      || g_ascii_strtoull (sequence, NULL, 10) != client->cseq)
    return unknown (client, GST_RTSP_EPARSE);
  guint code = message->type_data.response.code;
  if (code < 200)
    return GST_RTSP_OK;
  RuntimeSession *session = lookup (client, client->session);
  if (code >= 200 && code < 300)
    {
      gchar *id = session_header (message);
      if ((client->method == GST_RTSP_SETUP && !id)
          || (id && client->session && strcmp (id, client->session)))
        {
          g_free (id);
          return unknown (client, GST_RTSP_EPARSE);
        }
      if (client->method == GST_RTSP_SETUP)
        {
          gchar *transport_value = NULL;
          GstRTSPTransport *transport = NULL;
          gst_rtsp_transport_new (&transport);
          gboolean valid
              = gst_rtsp_message_get_header (message, GST_RTSP_HDR_TRANSPORT, &transport_value, 0)
                    == GST_RTSP_OK
                && gst_rtsp_message_get_header (message, GST_RTSP_HDR_TRANSPORT, NULL, 1)
                       != GST_RTSP_OK
                && gst_rtsp_transport_parse_version (transport_value, client->version, transport)
                       == GST_RTSP_OK;
          if (!valid)
            {
              gst_rtsp_transport_free (transport);
              g_free (id);
              return unknown (client, GST_RTSP_EPARSE);
            }
          if (transport->interleaved.min >= 0)
            {
              GHashTableIter sessions, tracks;
              gpointer session_key, session_value, track_key, track_value;
              g_hash_table_iter_init (&sessions, client->sessions);
              while (g_hash_table_iter_next (&sessions, &session_key, &session_value))
                {
                  g_hash_table_iter_init (&tracks, ((RuntimeSession *)session_value)->tracks);
                  while (g_hash_table_iter_next (&tracks, &track_key, &track_value))
                    {
                      RuntimeTrack *other = track_value;
                      if (other->state == GST_RTSP_RUNTIME_CLOSED
                          || (!strcmp (session_key, id) && !strcmp (track_key, client->uri)))
                        continue;
                      GstRTSPRange occupied = other->transport->interleaved;
                      gint last = transport->interleaved.max < 0 ? transport->interleaved.min
                                                                 : transport->interleaved.max;
                      gint occupied_last = occupied.max < 0 ? occupied.min : occupied.max;
                      if (occupied.min >= 0 && transport->interleaved.min <= occupied_last
                          && occupied.min <= last)
                        {
                          gst_rtsp_transport_free (transport);
                          g_free (id);
                          return unknown (client, GST_RTSP_EPARSE);
                        }
                    }
                }
            }
          session = lookup (client, id);
          if (!session)
            {
              session = g_new0 (RuntimeSession, 1);
              session->aggregate = g_strdup (client->root);
              session->tracks = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, track_free);
              g_hash_table_insert (client->sessions, g_strdup (id), session);
            }
          RuntimeTrack *track = g_hash_table_lookup (session->tracks, client->uri);
          if (track && track->generation == G_MAXUINT64)
            {
              gst_rtsp_transport_free (transport);
              g_free (id);
              return unknown (client, GST_RTSP_EPARSE);
            }
          if (!track)
            {
              track = g_new0 (RuntimeTrack, 1);
              track->state = GST_RTSP_RUNTIME_READY;
              g_hash_table_insert (session->tracks, g_strdup (client->uri), track);
            }
          else
            {
              if (track->state == GST_RTSP_RUNTIME_CLOSED)
                track->state = GST_RTSP_RUNTIME_READY;
              gst_rtsp_transport_free (track->transport);
            }
          track->transport = transport;
          track->generation++;
        }
      else if (client->method == GST_RTSP_PLAY)
        transition (client, session, GST_RTSP_RUNTIME_PLAYING);
      else if (client->method == GST_RTSP_RECORD)
        transition (client, session, GST_RTSP_RUNTIME_RECORDING);
      else if (client->method == GST_RTSP_PAUSE)
        transition (client, session, GST_RTSP_RUNTIME_READY);
      else if (client->method == GST_RTSP_TEARDOWN)
        transition (client, session, GST_RTSP_RUNTIME_CLOSED);
      g_free (id);
    }
  else if (code == 454 && session)
    {
      GHashTableIter iter;
      gpointer key, value;
      g_hash_table_iter_init (&iter, session->tracks);
      while (g_hash_table_iter_next (&iter, &key, &value))
        ((RuntimeTrack *)value)->state = GST_RTSP_RUNTIME_CLOSED;
    }
  client->dispatch = GST_RTSP_RUNTIME_RESPONSE_RECEIVED;
  pending_clear (client);
  return GST_RTSP_OK;
}
GstRTSPResult
gst_rtsp_runtime_client_receive (GstRTSPRuntimeClient *client, GstRTSPMessage *message,
                                 gint64 timeout)
{
  g_return_val_if_fail (client && message && !client->unknown, GST_RTSP_EINVAL);
  return receive_result (client, message,
                         gst_rtsp_connection_receive_usec (client->connection, message, timeout));
}
int
gst_rtsp_runtime_client_receive_step (GstRTSPRuntimeClient *client, GstRTSPMessage *message)
{
  g_return_val_if_fail (client && message && !client->unknown, GST_RTSP_EINVAL);
  int result = gst_rtsp_connection_receive_step (client->connection, message);
  return result == 1 ? 1 : receive_result (client, message, result);
}
GstRTSPResult
gst_rtsp_runtime_client_respond (GstRTSPRuntimeClient *client, GstRTSPMessage *response,
                                 gint64 timeout)
{
  g_return_val_if_fail (client && response, GST_RTSP_EINVAL);
  if (client->unknown || response->type != GST_RTSP_MESSAGE_RESPONSE
      || response->type_data.response.version != client->version)
    return GST_RTSP_EINVAL;
  return gst_rtsp_connection_send_usec (client->connection, response, timeout);
}
GBytes *
gst_rtsp_runtime_client_received_bytes (GstRTSPRuntimeClient *client)
{
  return gst_rtsp_connection_received_bytes (client->connection);
}
GstRTSPRuntimeDispatch
gst_rtsp_runtime_client_dispatch (GstRTSPRuntimeClient *client)
{
  return client->dispatch;
}
gboolean
gst_rtsp_runtime_client_track_state (GstRTSPRuntimeClient *client, const gchar *id,
                                     const gchar *uri, GstRTSPRuntimeState *state)
{
  RuntimeSession *session = lookup (client, id);
  gpointer value = session ? g_hash_table_lookup (session->tracks, uri) : NULL;
  if (!value)
    return FALSE;
  *state = ((RuntimeTrack *)value)->state;
  return TRUE;
}
const GstRTSPTransport *
gst_rtsp_runtime_client_track_transport (GstRTSPRuntimeClient *client, const gchar *id,
                                         const gchar *uri)
{
  RuntimeSession *session = lookup (client, id);
  RuntimeTrack *track = session ? g_hash_table_lookup (session->tracks, uri) : NULL;
  return track ? track->transport : NULL;
}
guint64
gst_rtsp_runtime_client_track_generation (GstRTSPRuntimeClient *client, const gchar *id,
                                          const gchar *uri)
{
  RuntimeSession *session = lookup (client, id);
  RuntimeTrack *track = session ? g_hash_table_lookup (session->tracks, uri) : NULL;
  return track ? track->generation : 0;
}
void
gst_rtsp_runtime_client_cancel (GstRTSPRuntimeClient *client)
{
  gst_rtsp_connection_flush (client->connection, TRUE);
}
void
gst_rtsp_runtime_client_free (GstRTSPRuntimeClient *client)
{
  if (!client)
    return;
  gst_rtsp_runtime_client_cancel (client);
  gst_rtsp_connection_free (client->connection);
  g_hash_table_unref (client->sessions);
  pending_clear (client);
  g_free (client->root);
  g_free (client);
}

GstRTSPResult
gst_rtsp_runtime_client_wait (GstRTSPRuntimeClient *client, gint64 timeout)
{
  GstRTSPEvent events;
  return gst_rtsp_connection_poll_usec (client->connection, GST_RTSP_EV_READ, &events, timeout);
}
guint32
gst_rtsp_runtime_client_cseq (GstRTSPRuntimeClient *client)
{
  return client->cseq;
}
