/* Controlled RTSP transport: no DNS, TLS, retry, downgrade or implicit requests.
 * The socket pair stands in for the Runtime's already protected byte stream. */
#include "gstrtspconnection.h"
#include "gstrtspruntimeclient.h"
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
  operation (client, pair[1], version, GST_RTSP_SETUP, two, session, seq++, 200, setup);
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

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  roundtrip (GST_RTSP_VERSION_1_0, "1.0");
  roundtrip (GST_RTSP_VERSION_2_0, "2.0");
  truncated_cancel ();
  malformed ();
  explicit_state (GST_RTSP_VERSION_1_0);
  explicit_state (GST_RTSP_VERSION_2_0);
  g_print ("PASS RTSP 1.0/2.0: supplied protected stream, exact negative response and binary "
           "frame, partial-body cancel, strict lengths, no extra requests\n");
  return 0;
}
