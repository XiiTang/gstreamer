/* Exercise the real timeout callback with a failed OpenSSL timer operation.
 * Including the implementation keeps fault injection out of the native ABI. */
#include <gst/gst.h>
#include <openssl/ssl.h>

static guint timeout_calls;
static long timeout_result;

static long
timeout (SSL *ssl)
{
  g_assert_nonnull (ssl);
  timeout_calls++;
  return timeout_result;
}

#undef DTLSv1_handle_timeout
#define DTLSv1_handle_timeout(ssl) timeout (ssl)
#include "../../subprojects/gst-plugins-bad/ext/dtls/gstdtlsconnection.c"

static void
state_changed (GObject *object, GParamSpec *property, gpointer data)
{
  GstDtlsConnection *connection = GST_DTLS_CONNECTION (object);
  GstDtlsConnectionState state;

  /* Notification must run outside the connection lock. */
  g_assert_true (g_mutex_trylock (&connection->priv->mutex));
  g_mutex_unlock (&connection->priv->mutex);
  g_object_get (object, "connection-state", &state, NULL);
  g_assert_cmpint (state, ==, GST_DTLS_CONNECTION_STATE_FAILED);
  (*(guint *) data)++;
}

int
main (int argc, char **argv)
{
  GstDtlsCertificate *certificate;
  GstDtlsAgent *agent;
  GstDtlsConnection *connection;
  GstDtlsConnectionState state;
  guint notifications = 0;

  gst_init (&argc, &argv);
  certificate = g_object_new (GST_TYPE_DTLS_CERTIFICATE, NULL);
  agent = g_object_new (GST_TYPE_DTLS_AGENT, "certificate", certificate, NULL);
  connection = g_object_new (GST_TYPE_DTLS_CONNECTION, "agent", agent, NULL);
  g_signal_connect (connection, "notify::connection-state",
      G_CALLBACK (state_changed), &notifications);
  connection->priv->connection_state = GST_DTLS_CONNECTION_STATE_CONNECTING;

  /* No work from a non-expired timer means no state change. */
  timeout_result = 0;
  handle_timeout (NULL, connection);
  g_assert_cmpuint (timeout_calls, ==, 1);
  g_assert_cmpuint (notifications, ==, 0);
  g_object_get (connection, "connection-state", &state, NULL);
  g_assert_cmpint (state, ==, GST_DTLS_CONNECTION_STATE_CONNECTING);

  timeout_result = -1;
  connection->priv->timeout_pending = TRUE;
  handle_timeout (NULL, connection);
  g_assert_false (connection->priv->timeout_pending);
  g_object_get (connection, "connection-state", &state, NULL);
  g_assert_cmpint (state, ==, GST_DTLS_CONNECTION_STATE_FAILED);
  g_assert_cmpuint (notifications, ==, 1);

  handle_timeout (NULL, connection);
  g_assert_cmpuint (notifications, ==, 1);
  g_assert_cmpuint (timeout_calls, ==, 3);
  gst_dtls_connection_stop (connection);
  handle_timeout (NULL, connection);
  g_assert_cmpuint (timeout_calls, ==, 3);

  g_object_unref (connection);
  g_object_unref (agent);
  g_object_unref (certificate);
  g_print ("PASS DTLS timeout failure: FAILED, one unlocked notification, stopped timer ignored\n");
  return 0;
}
