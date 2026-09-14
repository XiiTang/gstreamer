/* Controlled RTSP transport: no DNS, TLS, retry, downgrade or implicit requests.
 * The socket pair stands in for the Runtime's already protected byte stream. */
#include "gstrtspconnection.h"
#include "gstrtspruntimeclient.h"
#include <errno.h>
#include <fcntl.h>
#include <gst/gst.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct
{
  GstRTSPConnection *connection;
  GstRTSPResult result;
} Read;
static gpointer
read_message (gpointer value)
{
  Read *read = value;
  GstRTSPMessage message = { 0 };
  read->result = gst_rtsp_connection_receive_usec (read->connection, &message, 0);
  gst_rtsp_message_unset (&message);
  return NULL;
}
static GstRTSPConnection *
connection (int *peer, guint limit)
{
  int pair[2];
  g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, pair), ==, 0);
  GError *error = NULL;
  GSocket *socket = g_socket_new_from_fd (pair[0], &error);
  g_assert_no_error (error);
  GstRTSPUrl *url = NULL;
  g_assert_cmpint (gst_rtsp_url_parse ("rtsps://never-resolve.invalid:443/live", &url), ==,
                   GST_RTSP_OK);
  GstRTSPConnection *conn = NULL;
  g_assert_cmpint (gst_rtsp_connection_create_runtime_client (url, socket, limit, &conn), ==,
                   GST_RTSP_OK);
  g_object_unref (socket);
  gst_rtsp_url_free (url);
  *peer = pair[1];
  return conn;
}
static void
raw_equals (GstRTSPConnection *conn, const guint8 *raw, gsize length)
{
  GBytes *bytes = gst_rtsp_connection_received_bytes (conn);
  gsize got_length;
  const guint8 *got = g_bytes_get_data (bytes, &got_length);
  g_assert_cmpmem (raw, length, got, got_length);
  g_bytes_unref (bytes);
}
static void
roundtrip (GstRTSPVersion version, const char *text)
{
  int peer;
  GstRTSPConnection *conn = connection (&peer, 1024);
  GstRTSPMessage request = { 0 }, response = { 0 };
  g_assert_cmpint (gst_rtsp_connection_connect_usec (conn, 1), ==, GST_RTSP_EINVAL);
  g_assert_cmpuint (gst_rtsp_connection_next_cseq (conn), ==, 1);
  gst_rtsp_message_init_request (&request, GST_RTSP_OPTIONS,
                                 "rtsps://never-resolve.invalid:443/live");
  gst_rtsp_message_add_header_by_name (&request, "X-Test", "x\r\nInjected: value");
  g_assert_cmpint (gst_rtsp_connection_send_usec (conn, &request, 1000000), ==, GST_RTSP_EINVAL);
  g_assert_cmpuint (gst_rtsp_connection_written_bytes (conn), ==, 0);
  g_assert_cmpuint (gst_rtsp_connection_next_cseq (conn), ==, 1);
  gst_rtsp_message_remove_header_by_name (&request, "X-Test", -1);
  request.type_data.request.version = version;
  g_assert_cmpint (gst_rtsp_connection_send_usec (conn, &request, 1000000), ==, GST_RTSP_OK);
  gchar wire[4096];
  ssize_t count = read (peer, wire, sizeof (wire) - 1);
  g_assert_cmpint (count, >, 0);
  wire[count] = 0;
  g_assert_cmpuint (gst_rtsp_connection_written_bytes (conn), ==, count);
  gchar *expected = g_strdup_printf (
      "OPTIONS rtsps://never-resolve.invalid:443/live RTSP/%s\r\nCSeq: 1\r\n", text);
  g_assert_true (g_str_has_prefix (wire, expected));
  g_free (expected);
  gchar *negative = g_strdup_printf ("RTSP/%s 461 Unsupported Transport\r\nCSeq: 1\r\nX-Original:  "
                                     "a  b\r\nContent-Length: 4\r\n\r\n",
                                     text);
  GByteArray *frame = g_byte_array_new ();
  g_byte_array_append (frame, (guint8 *)negative, strlen (negative));
  const guint8 body[4] = { 0, 0xff, '$', 0 };
  g_byte_array_append (frame, body, 4);
  const guint8 data[] = { '$', 7, 0, 4, 0, 0xff, '$', 0 };
  g_assert_cmpint (write (peer, frame->data, frame->len), ==, frame->len);
  g_assert_cmpint (write (peer, data, sizeof (data)), ==, sizeof (data));
  g_assert_cmpint (gst_rtsp_connection_receive_usec (conn, &response, 1000000), ==, GST_RTSP_OK);
  g_assert_cmpint (response.type_data.response.code, ==, 461);
  g_assert_cmpint (response.type_data.response.version, ==, version);
  raw_equals (conn, frame->data, frame->len);
  gst_rtsp_message_unset (&response);
  g_assert_cmpint (gst_rtsp_connection_receive_usec (conn, &response, 1000000), ==, GST_RTSP_OK);
  g_assert_cmpint (response.type, ==, GST_RTSP_MESSAGE_DATA);
  raw_equals (conn, data, sizeof (data));
  gst_rtsp_message_unset (&response);
  gst_rtsp_message_unset (&request);
  gst_rtsp_connection_free (conn);
  /* Closing native ownership sends no TEARDOWN or HTTP tunnel response. */
  g_assert_cmpint (read (peer, wire, sizeof (wire)), ==, 0);
  close (peer);
  g_free (negative);
  g_byte_array_unref (frame);
}
static void
truncated_cancel (void)
{
  for (guint run = 0; run < 8; run++)
    {
      int peer;
      GstRTSPConnection *conn = connection (&peer, 1024);
      const char partial[] = "RTSP/2.0 200 OK\r\nCSeq: 0\r\nContent-Length: 8\r\n\r\nx";
      write (peer, partial, sizeof (partial) - 1);
      Read read = { conn, GST_RTSP_OK };
      GThread *thread = g_thread_new ("rtsp-read", read_message, &read);
      /* The native reader must remain cancellable after it entered a body. */
      g_usleep (10000);
      gint64 before = g_get_monotonic_time ();
      gst_rtsp_connection_flush (conn, TRUE);
      g_thread_join (thread);
      g_assert_cmpint (read.result, ==, GST_RTSP_EINTR);
      g_assert_cmpint (g_get_monotonic_time () - before, <, 100000);
      gst_rtsp_connection_free (conn);
      close (peer);
    }
}
static void
malformed (void)
{
  const char *cases[]
      = { "RTSP/1.0 200 OK\r\nCSeq: 0junk\r\n\r\n", "RTSP/1.0 200 OK\r\nCSeq: 0\r\nCSeq: 0\r\n\r\n",
          "RTSP/1.0 200 OK\r\nCSeq: 0\r\nContent-Length: 1junk\r\n\r\nx",
          "RTSP/1.0 200 OK\r\nCSeq: 0\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
          "RTSP/1.0 200 OK\r\nCSeq: 0\r\nContent-Length: 1048576\r\n\r\n" };
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      int peer;
      GstRTSPConnection *conn = connection (&peer, 1024);
      GstRTSPMessage message = { 0 };
      write (peer, cases[i], strlen (cases[i]));
      GstRTSPResult result = gst_rtsp_connection_receive_usec (conn, &message, 1000000);
      g_assert_true (result == GST_RTSP_EPARSE || result == GST_RTSP_ENOMEM);
      gst_rtsp_message_unset (&message);
      gst_rtsp_connection_free (conn);
      close (peer);
    }
}
static void
operation (GstRTSPRuntimeClient *client, int peer, GstRTSPVersion version, GstRTSPMethod method,
           const char *uri, const char *session, guint sequence, guint code, const char *headers)
{
  GstRTSPMessage request = { 0 }, response = { 0 };
  gst_rtsp_message_init_request (&request, method, uri);
  request.type_data.request.version = version;
  if (session)
    gst_rtsp_message_add_header (&request, GST_RTSP_HDR_SESSION, session);
  if (method == GST_RTSP_SETUP)
    gst_rtsp_message_add_header (&request, GST_RTSP_HDR_TRANSPORT,
                                 "RTP/AVP/TCP;unicast;interleaved=0-1");
  g_assert_cmpint (gst_rtsp_runtime_client_request (client, &request, 1000000), ==, GST_RTSP_OK);
  g_assert_cmpint (gst_rtsp_runtime_client_dispatch (client), ==, GST_RTSP_RUNTIME_MAYBE_SENT);
  gchar wire[8192];
  ssize_t length = read (peer, wire, sizeof (wire) - 1);
  g_assert_cmpint (length, >, 0);
  wire[length] = 0;
  gchar *cseq = g_strdup_printf ("CSeq: %u\r\n", sequence);
  g_assert_nonnull (strstr (wire, cseq));
  g_free (cseq);
  gchar *reply = g_strdup_printf ("RTSP/%s %u Response\r\nCSeq: %u\r\n%s\r\n",
                                  version == GST_RTSP_VERSION_2_0 ? "2.0" : "1.0", code, sequence,
                                  headers ? headers : "");
  g_assert_cmpint (write (peer, reply, strlen (reply)), ==, strlen (reply));
  g_assert_cmpint (gst_rtsp_runtime_client_receive (client, &response, 1000000), ==, GST_RTSP_OK);
  g_assert_cmpint (response.type_data.response.code, ==, code);
  g_assert_cmpint (gst_rtsp_runtime_client_dispatch (client), ==,
                   GST_RTSP_RUNTIME_RESPONSE_RECEIVED);
  GBytes *raw = gst_rtsp_runtime_client_received_bytes (client);
  gsize count;
  gconstpointer bytes = g_bytes_get_data (raw, &count);
  g_assert_cmpmem (reply, strlen (reply), bytes, count);
  g_bytes_unref (raw);
  g_free (reply);
  gst_rtsp_message_unset (&response);
  gst_rtsp_message_unset (&request);
}
static void
state_is (GstRTSPRuntimeClient *client, const char *track, GstRTSPRuntimeState expected)
{
  GstRTSPRuntimeState state;
  g_assert_true (gst_rtsp_runtime_client_track_state (client, "retained-session", track, &state));
  g_assert_cmpint (state, ==, expected);
}
static void
explicit_state (GstRTSPVersion version)
{
  int pair[2];
  g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, pair), ==, 0);
  GSocket *socket = g_socket_new_from_fd (pair[0], NULL);
  GstRTSPUrl *url = NULL;
  gst_rtsp_url_parse ("rtsp://never-resolve.invalid/live", &url);
  GstRTSPRuntimeClient *client = NULL;
  g_assert_cmpint (gst_rtsp_runtime_client_new (url, socket, version, 1024, &client), ==,
                   GST_RTSP_OK);
  g_object_unref (socket);
  gst_rtsp_url_free (url);
  const char *root = "rtsp://never-resolve.invalid/live",
             *one = "rtsp://never-resolve.invalid/live/one",
             *two = "rtsp://never-resolve.invalid/live/two";
  const char *
      session = "retained-session",
     *setup
     = "Session: retained-session;timeout=30\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n";
  guint seq = 1;
  operation (client, pair[1], version, GST_RTSP_OPTIONS, root, NULL, seq++, 200, NULL);
  operation (client, pair[1], version, GST_RTSP_DESCRIBE, root, NULL, seq++, 302,
             "Location: rtsp://must-not-connect.invalid/new\r\n");
  operation (client, pair[1], version, GST_RTSP_SETUP, one, NULL, seq++, 200, setup);
  operation (
      client, pair[1], version, GST_RTSP_SETUP, two, session, seq++, 200,
      "Session: retained-session;timeout=30\r\nTransport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n");
  const GstRTSPTransport *negotiated
      = gst_rtsp_runtime_client_track_transport (client, session, one);
  g_assert_nonnull (negotiated);
  g_assert_cmpint (negotiated->interleaved.min, ==, 0);
  g_assert_cmpint (negotiated->interleaved.max, ==, 1);
  state_is (client, one, GST_RTSP_RUNTIME_READY);
  state_is (client, two, GST_RTSP_RUNTIME_READY);
  operation (client, pair[1], version, GST_RTSP_PLAY, one, session, seq++, 200, NULL);
  state_is (client, one, GST_RTSP_RUNTIME_PLAYING);
  state_is (client, two, GST_RTSP_RUNTIME_READY);
  operation (client, pair[1], version, GST_RTSP_PAUSE, root, session, seq++, 455, NULL);
  state_is (client, one, GST_RTSP_RUNTIME_PLAYING);
  state_is (client, two, GST_RTSP_RUNTIME_READY);
  operation (client, pair[1], version, GST_RTSP_PLAY, root, session, seq++, 200, NULL);
  state_is (client, one, GST_RTSP_RUNTIME_PLAYING);
  state_is (client, two, GST_RTSP_RUNTIME_PLAYING);
  operation (client, pair[1], version, GST_RTSP_TEARDOWN, one, session, seq++, 200, NULL);
  state_is (client, one, GST_RTSP_RUNTIME_CLOSED);
  state_is (client, two, GST_RTSP_RUNTIME_PLAYING);
  GstRTSPMessage request = { 0 }, reply = { 0 };
  gst_rtsp_message_init_request (&request, GST_RTSP_PLAY, one);
  request.type_data.request.version = version;
  gst_rtsp_message_add_header (&request, GST_RTSP_HDR_SESSION, session);
  g_assert_cmpint (gst_rtsp_runtime_client_request (client, &request, 1000000), ==,
                   GST_RTSP_EINVAL);
  gst_rtsp_message_unset (&request);
  gst_rtsp_message_init_request (&request, GST_RTSP_PAUSE, two);
  request.type_data.request.version = version;
  gst_rtsp_message_add_header (&request, GST_RTSP_HDR_SESSION, session);
  g_assert_cmpint (gst_rtsp_runtime_client_request (client, &request, 1000000), ==, GST_RTSP_OK);
  gchar wire[4096];
  g_assert_cmpint (read (pair[1], wire, sizeof (wire)), >, 0);
  /* A response for a different request cannot establish a local state change. */
  gchar *wrong = g_strdup_printf ("RTSP/%s 200 OK\r\nCSeq: %u\r\n\r\n",
                                  version == GST_RTSP_VERSION_2_0 ? "2.0" : "1.0", seq - 1);
  write (pair[1], wrong, strlen (wrong));
  g_free (wrong);
  g_assert_cmpint (gst_rtsp_runtime_client_receive (client, &reply, 1000000), ==, GST_RTSP_EPARSE);
  state_is (client, two, GST_RTSP_RUNTIME_UNKNOWN);
  state_is (client, one, GST_RTSP_RUNTIME_CLOSED);
  g_assert_cmpint (gst_rtsp_runtime_client_request (client, &request, 1000000), ==,
                   GST_RTSP_EINVAL);
  gst_rtsp_message_unset (&reply);
  gst_rtsp_message_unset (&request);
  gst_rtsp_runtime_client_free (client);
  g_assert_cmpint (read (pair[1], wire, sizeof (wire)), ==, 0);
  close (pair[1]);
}

static void
versioned_transport (void)
{
  GstRTSPTransport *t = NULL, *copy = NULL;
  gst_rtsp_transport_new (&t);
  gst_rtsp_transport_new (&copy);
  const gchar *v2 = "RTP/AVP/UDP; unicast;dest_addr=\":8000\"/\"[2001:db8::1]:8001\";"
                    "src_addr=\"media.example:9000\"/\"media.example:9001\";ssrc=00000000/12345678";
  g_assert_cmpint (gst_rtsp_transport_parse_version (v2, GST_RTSP_VERSION_2_0, t), ==, GST_RTSP_OK);
  g_assert_cmpuint (t->dest_addr_count, ==, 2);
  g_assert_cmpstr (t->dest_addr[0].host, ==, "");
  g_assert_cmpuint (t->dest_addr[0].port, ==, 8000);
  g_assert_cmpstr (t->dest_addr[1].host, ==, "2001:db8::1");
  g_assert_cmpuint (t->src_addr_count, ==, 2);
  g_assert_cmpstr (t->src_addr[1].host, ==, "media.example");
  g_assert_cmpuint (t->ssrcs->len, ==, 2);
  g_assert_cmpuint (g_array_index (t->ssrcs, guint32, 0), ==, 0);
  gchar *encoded = gst_rtsp_transport_as_text (t);
  g_assert_nonnull (encoded);
  g_assert_cmpint (gst_rtsp_transport_parse_version (encoded, GST_RTSP_VERSION_2_0, copy), ==,
                   GST_RTSP_OK);
  g_assert_cmpstr (copy->dest_addr[1].host, ==, "2001:db8::1");
  g_assert_cmpuint (copy->ssrcs->len, ==, 2);
  g_free (encoded);
  g_assert_cmpint (gst_rtsp_transport_parse_version (v2, GST_RTSP_VERSION_1_0, t), <, 0);
  const gchar *v1
      = "RTP/"
        "AVP;unicast;client_port=8000-8001;server_port=9000-9001;source=127.0.0.1;mode=\"RECORD\"";
  g_assert_cmpint (gst_rtsp_transport_parse_version (v1, GST_RTSP_VERSION_1_0, t), ==, GST_RTSP_OK);
  g_assert_cmpuint (t->server_port.max, ==, 9001);
  g_assert_cmpint (gst_rtsp_transport_parse_version (v1, GST_RTSP_VERSION_2_0, t), <, 0);
  const gchar *bad[] = {
    "RTP/AVP;unicast;dest_addr=\":65536\"",
    "RTP/AVP;unicast;dest_addr=\":8\"/",
    "RTP/AVP;unicast;dest_addr=\":8\"/\":9\"/\":10\"",
    "RTP/AVP;unicast;dest_addr=\"[invalid]:8\"",
    "RTP/AVP;unicast;dest_addr=\"host:8junk\"",
    "RTP/AVP;unicast;dest_addr=\"host/path:8\"",
    "RTP/AVP;unicast;dest_addr=\":8\";dest_addr=\":9\"",
    "RTP/AVP;unicast;x-required=unimplemented",
    "RTP/AVP/UDP/extra;unicast",
    "RTP/AVP;dest_addr=\":8\"",
    "RTP/AVP;unicast;ssrc=1",
    "RTP/AVP/TCP;unicast;interleaved=0-1;src_addr=\":9\"",
    "RTP/AVP/TCP;unicast;interleaved=1-",
    "RTP/AVP/TCP;unicast;interleaved=2-1",
    "RTP/AVP/TCP;unicast;interleaved=0-1;mode=\"PLAYBACK\"",
    "RTP/AVP/TCP;unicast;interleaved=0-1;mode=\"PLAY,BOGUS\"",
  };
  for (guint i = 0; i < G_N_ELEMENTS (bad); i++)
    g_assert_cmpint (gst_rtsp_transport_parse_version (bad[i], GST_RTSP_VERSION_2_0, t), <, 0);
  g_assert_cmpint (
      gst_rtsp_transport_parse_version ("RTP/AVP;unicast;client_port=no", GST_RTSP_VERSION_1_0, t),
      <, 0);
  gst_rtsp_transport_free (t);
  gst_rtsp_transport_free (copy);
  g_print ("PASS version-specific native Transport tuples, IPv6, SSRC list and strict rejection\n");
}

static void
incremental_duplex_write (void)
{
  int peer;
  const guint size = 2 * 1024 * 1024;
  GstRTSPConnection *conn = connection (&peer, size);
  GstRTSPMessage request = { 0 }, response = { 0 };
  guint8 *payload = g_malloc (size);
  memset (payload, 0xab, size);
  gst_rtsp_message_init_request (&request, GST_RTSP_SET_PARAMETER,
                                 "rtsp://never-resolve.invalid/media");
  gst_rtsp_message_set_body (&request, payload, size);
  g_free (payload);
  g_assert_cmpint (gst_rtsp_connection_write_begin (conn, &request), ==, 0);
  g_assert_cmpint (gst_rtsp_connection_write_begin (conn, &request), ==, 1);
  gst_rtsp_message_unset (&request);
  gint64 start = g_get_monotonic_time ();
  g_assert_cmpint (gst_rtsp_connection_write_step (conn), ==, 1);
  g_assert_cmpint (g_get_monotonic_time () - start, <, 200000);
  g_assert_cmpuint (gst_rtsp_connection_written_bytes (conn), >, 0);
  const gchar *incoming = "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 3\r\n\r\nabc";
  g_assert_cmpint (write (peer, incoming, strlen (incoming)), ==, strlen (incoming));
  g_assert_cmpint (gst_rtsp_connection_receive_step (conn, &response), ==, 0);
  g_assert_cmpint (response.type_data.response.code, ==, 200);
  gst_rtsp_message_unset (&response);
  g_assert_cmpint (fcntl (peer, F_SETFL, O_NONBLOCK), ==, 0);
  GByteArray *wire = g_byte_array_new ();
  gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  while (TRUE)
    {
      g_assert_cmpint (g_get_monotonic_time (), <, deadline);
      guint8 bytes[65536];
      ssize_t count = read (peer, bytes, sizeof (bytes));
      if (count > 0)
        g_byte_array_append (wire, bytes, count);
      else
        g_assert_true (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
      gboolean pending = gst_rtsp_connection_write_pending (conn);
      if (pending)
        g_assert_cmpint (gst_rtsp_connection_write_step (conn), >=, 0);
      if (!pending && count < 0)
        break;
    }
  const gchar *body = g_strstr_len ((gchar *)wire->data, wire->len, "\r\n\r\n");
  g_assert_nonnull (body);
  body += 4;
  g_assert_cmpuint (wire->len - (body - (gchar *)wire->data), ==, size);
  for (guint i = 0; i < size; i++)
    g_assert_cmpuint ((guint8)body[i], ==, 0xab);
  g_assert_cmpuint (gst_rtsp_connection_written_bytes (conn), ==, wire->len);
  g_byte_array_unref (wire);
  payload = g_malloc0 (size);
  gst_rtsp_message_init_request (&request, GST_RTSP_SET_PARAMETER,
                                 "rtsp://never-resolve.invalid/media");
  gst_rtsp_message_take_body (&request, payload, size);
  g_assert_cmpint (gst_rtsp_connection_write_begin (conn, &request), ==, 0);
  gst_rtsp_message_unset (&request);
  g_assert_cmpint (gst_rtsp_connection_write_step (conn), ==, 1);
  gst_rtsp_connection_flush (conn, TRUE);
  g_assert_cmpint (gst_rtsp_connection_write_step (conn), ==, GST_RTSP_EINTR);
  g_assert_false (gst_rtsp_connection_write_pending (conn));
  gst_rtsp_connection_free (conn);
  close (peer);
  g_print ("PASS native partial-write duplex progress, owned serialization, exact bytes and "
           "cancellation\n");
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  roundtrip (GST_RTSP_VERSION_1_0, "1.0");
  roundtrip (GST_RTSP_VERSION_2_0, "2.0");
  versioned_transport ();
  incremental_duplex_write ();
  truncated_cancel ();
  malformed ();
  explicit_state (GST_RTSP_VERSION_1_0);
  explicit_state (GST_RTSP_VERSION_2_0);
  g_print ("PASS RTSP 1.0/2.0: supplied protected stream, exact negative response and binary "
           "frame, partial-body cancel, strict lengths, no extra requests\n");
  return 0;
}
