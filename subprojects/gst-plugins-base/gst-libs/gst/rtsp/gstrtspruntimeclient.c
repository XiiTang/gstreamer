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
  guint64 timeout_seconds;
  gboolean timeout_explicit;
  gint64 last_control_response;
} RuntimeSession;
struct _GstRTSPRuntimeClient
{
  GstRTSPConnection *connection;
  GstRTSPVersion version;
  gchar *root;
  GHashTable *sessions;
  gboolean pending, unknown, writing_request;
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
session_header (GstRTSPMessage *message, guint64 *timeout_seconds, gboolean *explicit_timeout)
{
  gchar *value = NULL;
  if (timeout_seconds)
    *timeout_seconds = 60;
  if (explicit_timeout)
    *explicit_timeout = FALSE;
  if (gst_rtsp_message_get_header (message, GST_RTSP_HDR_SESSION, &value, 0) != GST_RTSP_OK)
    return NULL;
  if (gst_rtsp_message_get_header (message, GST_RTSP_HDR_SESSION, NULL, 1) == GST_RTSP_OK)
    return NULL;
  gchar **parts = g_strsplit (value, ";", 2);
  gchar *id = g_strstrip (parts[0]);
  gboolean valid = *id != 0;
  for (const gchar *p = id; valid && *p; p++)
    valid = g_ascii_isalnum (*p) || strchr ("-_.+", *p);
  if (valid && parts[1])
    {
      /* RFC 2326 12.37 / RFC 7826 10.5: timeout is response-only. */
      gchar *parameter = g_strstrip (parts[1]);
      gchar *equal = strchr (parameter, '=');
      valid = message->type == GST_RTSP_MESSAGE_RESPONSE && equal;
      if (valid)
        {
          *equal = 0;
          valid = !g_ascii_strcasecmp (g_strstrip (parameter), "timeout");
          gchar *number = g_strstrip (equal + 1);
          guint64 seconds = 0;
          valid = valid && *number;
          for (const gchar *p = number; valid && *p; p++)
            {
              if (!g_ascii_isdigit (*p) || seconds > (G_MAXUINT64 - (*p - '0')) / 10)
                valid = FALSE;
              else
                seconds = seconds * 10 + (*p - '0');
            }
          if (valid)
            {
              if (timeout_seconds)
                *timeout_seconds = seconds;
              if (explicit_timeout)
                *explicit_timeout = TRUE;
            }
        }
    }
  gchar *result = valid ? g_strdup (id) : NULL;
  g_strfreev (parts);
  return result;
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
static int
request_mode (GstRTSPRuntimeClient *client, GstRTSPMessage *request, gint64 timeout,
              gboolean incremental)
{
  if (client && gst_rtsp_connection_write_pending (client->connection))
    return incremental ? 1 : GST_RTSP_EINVAL;
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
  gchar *id = session_header (request, NULL, NULL);
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
  int result = incremental ? gst_rtsp_connection_write_begin (client->connection, request)
                           : gst_rtsp_connection_send_usec (client->connection, request, timeout);
  client->writing_request = incremental && result == GST_RTSP_OK;
  if (!incremental
      && (result == GST_RTSP_OK || gst_rtsp_connection_written_bytes (client->connection) > 0))
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
GstRTSPResult
gst_rtsp_runtime_client_request (GstRTSPRuntimeClient *client, GstRTSPMessage *request,
                                 gint64 timeout)
{
  return request_mode (client, request, timeout, FALSE);
}
int
gst_rtsp_runtime_client_request_begin (GstRTSPRuntimeClient *client, GstRTSPMessage *request)
{
  return request_mode (client, request, 0, TRUE);
}
int
gst_rtsp_runtime_client_write_step (GstRTSPRuntimeClient *client)
{
  g_return_val_if_fail (client && !client->unknown, GST_RTSP_EINVAL);
  int result = gst_rtsp_connection_write_step (client->connection);
  if (client->writing_request && gst_rtsp_connection_written_bytes (client->connection) > 0
      && client->dispatch == GST_RTSP_RUNTIME_NOT_SENT)
    client->dispatch = GST_RTSP_RUNTIME_MAYBE_SENT;
  if (result < 0)
    return unknown (client, result);
  if (result == 0)
    client->writing_request = FALSE;
  return result;
}
int
gst_rtsp_runtime_client_respond_begin (GstRTSPRuntimeClient *client, GstRTSPMessage *response)
{
  g_return_val_if_fail (client && response, GST_RTSP_EINVAL);
  if (client->unknown || response->type != GST_RTSP_MESSAGE_RESPONSE
      || response->type_data.response.version != client->version)
    return GST_RTSP_EINVAL;
  int result = gst_rtsp_connection_write_begin (client->connection, response);
  if (result == 0)
    client->writing_request = FALSE;
  return result;
}
int
gst_rtsp_runtime_client_send_data_begin (GstRTSPRuntimeClient *client, guint8 channel,
                                         const guint8 *bytes, gsize length)
{
  g_return_val_if_fail (client && bytes && length && length <= 65535 && !client->unknown,
                        GST_RTSP_EINVAL);
  if (gst_rtsp_connection_write_pending (client->connection))
    return 1;
  gboolean found = FALSE;
  GHashTableIter sessions, tracks;
  gpointer key, value, track_key, track_value;
  g_hash_table_iter_init (&sessions, client->sessions);
  while (g_hash_table_iter_next (&sessions, &key, &value))
    {
      g_hash_table_iter_init (&tracks, ((RuntimeSession *)value)->tracks);
      while (g_hash_table_iter_next (&tracks, &track_key, &track_value))
        {
          RuntimeTrack *track = track_value;
          GstRTSPRange range = track->transport->interleaved;
          if (track->state != GST_RTSP_RUNTIME_CLOSED && track->state != GST_RTSP_RUNTIME_UNKNOWN
              && range.min >= 0 && channel >= range.min
              && channel <= (range.max < 0 ? range.min : range.max))
            found = TRUE;
        }
    }
  if (!found)
    return GST_RTSP_EINVAL;
  GstRTSPMessage message = { 0 };
  gst_rtsp_message_init_data (&message, channel);
  gst_rtsp_message_set_body (&message, bytes, length);
  int result = gst_rtsp_connection_write_begin (client->connection, &message);
  gst_rtsp_message_unset (&message);
  if (result == 0)
    client->writing_request = FALSE;
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
      || client->dispatch == GST_RTSP_RUNTIME_NOT_SENT
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
      guint64 timeout_seconds;
      gboolean timeout_explicit;
      gchar *id = session_header (message, &timeout_seconds, &timeout_explicit);
      if ((client->method == GST_RTSP_SETUP && !id)
          || (!id
              && gst_rtsp_message_get_header (message, GST_RTSP_HDR_SESSION, NULL, 0)
                     == GST_RTSP_OK)
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
          session->timeout_seconds = timeout_seconds;
          session->timeout_explicit = timeout_explicit;
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
      if (session)
        {
          if (timeout_explicit)
            {
              session->timeout_seconds = timeout_seconds;
              session->timeout_explicit = TRUE;
            }
          session->last_control_response = g_get_monotonic_time ();
        }
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
gsize
gst_rtsp_runtime_client_written_bytes (GstRTSPRuntimeClient *client)
{
  return gst_rtsp_connection_written_bytes (client->connection);
}
GstRTSPRuntimeDispatch
gst_rtsp_runtime_client_dispatch (GstRTSPRuntimeClient *client)
{
  return client->dispatch;
}
gboolean
gst_rtsp_runtime_client_session_info (GstRTSPRuntimeClient *client, const gchar *id,
                                      guint64 *seconds, gboolean *explicit_timeout,
                                      guint64 *control_response_age_us)
{
  RuntimeSession *session = lookup (client, id);
  if (!session)
    return FALSE;
  *seconds = session->timeout_seconds;
  *explicit_timeout = session->timeout_explicit;
  *control_response_age_us = MAX (g_get_monotonic_time () - session->last_control_response, 0);
  return TRUE;
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
  GstRTSPEvent requested = GST_RTSP_EV_READ;
  if (gst_rtsp_connection_write_pending (client->connection))
    requested |= GST_RTSP_EV_WRITE;
  return gst_rtsp_connection_poll_usec (client->connection, requested, &events, timeout);
}
guint32
gst_rtsp_runtime_client_cseq (GstRTSPRuntimeClient *client)
{
  return client->cseq;
}

void
gst_rtsp_runtime_client_invalidate (GstRTSPRuntimeClient *client)
{
  if (!client)
    return;
  client->unknown = TRUE;
  GHashTableIter sessions, tracks;
  gpointer session, track;
  g_hash_table_iter_init (&sessions, client->sessions);
  while (g_hash_table_iter_next (&sessions, NULL, &session))
    {
      g_hash_table_iter_init (&tracks, ((RuntimeSession *)session)->tracks);
      while (g_hash_table_iter_next (&tracks, NULL, &track))
        ((RuntimeTrack *)track)->state = GST_RTSP_RUNTIME_UNKNOWN;
    }
  gst_rtsp_connection_flush (client->connection, TRUE);
}
