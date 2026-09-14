/* Native DTLS client against an independent OpenSSL DTLS server.
 * Synthetic keys stay in process and are never printed or written to disk. */
#include "gstdtlsagent.h"
#include "gstdtlscertificate.h"
#include "gstdtlsconnection.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct
{
  int socket;
  gboolean trust, verified;
  guint count, enc_length, dec_length, cipher, auth;
  guint8 expected_digest[32], encoder[44], decoder[44];
} Client;

static gboolean
send_packet (GstDtlsConnection *connection, gconstpointer data, gsize length, gpointer value)
{
  Client *client = value;
  client->count++;
  return send (client->socket, data, length, 0) == (ssize_t)length;
}
static gboolean
verify_peer (GstDtlsConnection *connection, const gchar *pem, Client *client)
{
  BIO *bio = BIO_new_mem_buf (pem, -1);
  X509 *cert = PEM_read_bio_X509 (bio, NULL, NULL, NULL);
  guint8 digest[32];
  unsigned length = 0;
  client->verified = cert && X509_digest (cert, EVP_sha256 (), digest, &length) && length == 32
                     && !CRYPTO_memcmp (digest, client->expected_digest, 32) && client->trust;
  X509_free (cert);
  BIO_free (bio);
  return client->verified;
}
static void
encoder_key (GstDtlsConnection *connection, gconstpointer key, guint length, guint cipher,
             guint auth, Client *client)
{
  g_assert_true (client->verified);
  g_assert_cmpuint (length, <=, 44);
  g_assert_cmpuint (client->enc_length, ==, 0);
  memcpy (client->encoder, key, length);
  client->enc_length = length;
  client->cipher = cipher;
  client->auth = auth;
}
static void
decoder_key (GstDtlsConnection *connection, gconstpointer key, guint length, guint cipher,
             guint auth, Client *client)
{
  g_assert_true (client->verified);
  g_assert_cmpuint (length, <=, 44);
  g_assert_cmpuint (client->dec_length, ==, 0);
  memcpy (client->decoder, key, length);
  client->dec_length = length;
}
static int
udp_socket (struct sockaddr_in *address)
{
  int fd = socket (AF_INET, SOCK_DGRAM, 0);
  g_assert_cmpint (fd, >=, 0);
  *address
      = (struct sockaddr_in){ .sin_family = AF_INET, .sin_addr.s_addr = htonl (INADDR_LOOPBACK) };
  g_assert_cmpint (bind (fd, (struct sockaddr *)address, sizeof (*address)), ==, 0);
  socklen_t length = sizeof (*address);
  g_assert_cmpint (getsockname (fd, (struct sockaddr *)address, &length), ==, 0);
  g_assert_cmpint (fcntl (fd, F_SETFL, O_NONBLOCK), ==, 0);
  return fd;
}
static void
finalized (gpointer data, GObject *object)
{
  *(gboolean *)data = TRUE;
}

static void
exchange (GstDtlsAgent *agent, GstDtlsCertificate *server_certificate, const char *profile,
          const char *server_profile, guint key_length, guint salt_length, gboolean trust,
          gboolean install_verifier, guint8 *previous)
{
  struct sockaddr_in client_address, server_address;
  Client client = { .trust = trust };
  client.socket = udp_socket (&client_address);
  int server_socket = udp_socket (&server_address);
  g_assert_cmpint (
      connect (client.socket, (struct sockaddr *)&server_address, sizeof (server_address)), ==, 0);
  g_assert_cmpint (
      connect (server_socket, (struct sockaddr *)&client_address, sizeof (client_address)), ==, 0);
  X509 *server_cert = _gst_dtls_certificate_get_internal_certificate (server_certificate);
  unsigned digest_length;
  g_assert_cmpint (X509_digest (server_cert, EVP_sha256 (), client.expected_digest, &digest_length),
                   ==, 1);
  SSL_CTX *context = SSL_CTX_new (DTLS_server_method ());
  g_assert_cmpint (SSL_CTX_set_min_proto_version (context, DTLS1_2_VERSION), ==, 1);
  g_assert_cmpint (SSL_CTX_use_certificate (context, server_cert), ==, 1);
  g_assert_cmpint (
      SSL_CTX_use_PrivateKey (context, _gst_dtls_certificate_get_internal_key (server_certificate)),
      ==, 1);
  g_assert_cmpint (SSL_CTX_set_tlsext_use_srtp (context, server_profile), ==, 0);
  SSL *server = SSL_new (context);
  BIO *bio = BIO_new_dgram (server_socket, BIO_NOCLOSE);
  BIO_ctrl (bio, BIO_CTRL_DGRAM_SET_CONNECTED, 0, &client_address);
  SSL_set_bio (server, bio, bio);
  SSL_set_accept_state (server);
  SSL_set_mtu (server, 1200);
  GstDtlsConnection *connection = g_object_new (GST_TYPE_DTLS_CONNECTION, "agent", agent, NULL);
  g_assert_true (gst_dtls_connection_set_srtp_profiles (connection, profile));
  g_assert_false (gst_dtls_connection_set_srtp_profiles (connection, "SRTP_AES128_CM_SHA1_32"));
  if (install_verifier)
    g_signal_connect (connection, "on-peer-certificate", G_CALLBACK (verify_peer), &client);
  g_signal_connect (connection, "on-encoder-key", G_CALLBACK (encoder_key), &client);
  g_signal_connect (connection, "on-decoder-key", G_CALLBACK (decoder_key), &client);
  gst_dtls_connection_set_send_callback (connection, send_packet, &client, NULL);
  GError *error = NULL;
  g_assert_true (gst_dtls_connection_start (connection, TRUE, &error));
  g_assert_no_error (error);
  g_assert_false (gst_dtls_connection_set_srtp_profiles (connection, profile));
  gst_dtls_connection_check_timeout (connection);
  gboolean server_done = FALSE, failed = FALSE;
  gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  while (g_get_monotonic_time () < deadline && !(client.enc_length && server_done))
    {
      if (!server_done)
        {
          int result = SSL_do_handshake (server);
          if (result == 1)
            server_done = TRUE;
          else
            {
              int code = SSL_get_error (server, result);
              if (code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE)
                {
                  failed = TRUE;
                  break;
                }
            }
        }
      guint8 packet[65536];
      ssize_t size;
      while ((size = recv (client.socket, packet, sizeof (packet), 0)) > 0)
        {
          gsize written = 0;
          GstFlowReturn result
              = gst_dtls_connection_process (connection, packet, size, &written, &error);
          if (result < 0)
            {
              failed = TRUE;
              break;
            }
        }
      if (failed)
        break;
      struct pollfd fds[2] = { { client.socket, POLLIN, 0 }, { server_socket, POLLIN, 0 } };
      poll (fds, 2, 2);
    }
  gboolean success = trust && install_verifier && !strcmp (profile, server_profile);
  if (success)
    {
      g_assert_false (failed);
      g_assert_true (server_done);
      g_assert_no_error (error);
      guint length = key_length + salt_length;
      g_assert_cmpuint (client.enc_length, ==, length);
      g_assert_cmpuint (client.dec_length, ==, length);
      guint8 exported[88];
      const char label[] = "EXTRACTOR-dtls_srtp";
      g_assert_cmpint (SSL_export_keying_material (server, exported, 2 * length, label,
                                                   sizeof (label) - 1, NULL, 0, 0),
                       ==, 1);
      g_assert_cmpmem (client.encoder, key_length, exported, key_length);
      g_assert_cmpmem (client.decoder, key_length, exported + key_length, key_length);
      g_assert_cmpmem (client.encoder + key_length, salt_length, exported + 2 * key_length,
                       salt_length);
      g_assert_cmpmem (client.decoder + key_length, salt_length,
                       exported + 2 * key_length + salt_length, salt_length);
      if (previous[0])
        g_assert_cmpint (CRYPTO_memcmp (previous + 1, client.encoder, length), !=, 0);
      memcpy (previous + 1, client.encoder, length);
      previous[0] = 1;
      OPENSSL_cleanse (exported, sizeof (exported));
      GError *restart_error = NULL;
      g_assert_false (gst_dtls_connection_start (connection, TRUE, &restart_error));
      g_assert_nonnull (restart_error);
      g_clear_error (&restart_error);
    }
  else
    {
      g_assert_cmpuint (client.enc_length, ==, 0);
      g_assert_cmpuint (client.dec_length, ==, 0);
    }
  g_clear_error (&error);
  guint sent = client.count;
  gboolean destroyed = FALSE;
  g_object_weak_ref (G_OBJECT (connection), finalized, &destroyed);
  gint64 before = g_get_monotonic_time ();
  gst_dtls_connection_stop (connection);
  g_object_unref (connection);
  g_assert_true (destroyed);
  g_assert_cmpint (g_get_monotonic_time () - before, <, 100000);
  g_assert_cmpuint (client.count, ==, sent);
  SSL_free (server);
  SSL_CTX_free (context);
  close (client.socket);
  close (server_socket);
  OPENSSL_cleanse (&client, sizeof (client));
}

static gboolean
drop_packet (GstDtlsConnection *connection, gconstpointer data, gsize length, gpointer value)
{
  (*(guint *)value)++;
  return TRUE;
}

static void
pending_timer_shutdown (GstDtlsAgent *agent)
{
  for (guint run = 0; run < 64; run++)
    {
      GstDtlsConnection *connection = g_object_new (GST_TYPE_DTLS_CONNECTION, "agent", agent, NULL);
      guint sent = 0;
      gboolean destroyed = FALSE;
      gst_dtls_connection_set_send_callback (connection, drop_packet, &sent, NULL);
      g_assert_true (gst_dtls_connection_set_srtp_profiles (connection, "SRTP_AEAD_AES_256_GCM"));
      GError *error = NULL;
      g_assert_true (gst_dtls_connection_start (connection, TRUE, &error));
      g_assert_no_error (error);
      for (guint reschedule = 0; reschedule < 8; reschedule++)
        gst_dtls_connection_check_timeout (connection);
      g_object_weak_ref (G_OBJECT (connection), finalized, &destroyed);
      guint before = sent;
      gint64 start = g_get_monotonic_time ();
      if (run % 2 == 0)
        gst_dtls_connection_stop (connection);
      g_object_unref (connection);
      g_assert_true (destroyed);
      g_assert_cmpint (g_get_monotonic_time () - start, <, 100000);
      g_assert_cmpuint (before, ==, sent);
    }
  g_print ("PASS pending timer: repeated replacement, explicit stop and final release, no retained "
           "connection\n");
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  GstDtlsCertificate *certificate = g_object_new (GST_TYPE_DTLS_CERTIFICATE, NULL);
  GstDtlsCertificate *server = g_object_new (GST_TYPE_DTLS_CERTIFICATE, NULL);
  GstDtlsAgent *agent = g_object_new (GST_TYPE_DTLS_AGENT, "certificate", certificate, NULL);
  const char *profiles[]
      = { "SRTP_AES128_CM_SHA1_80", "SRTP_AEAD_AES_128_GCM", "SRTP_AEAD_AES_256_GCM" };
  const guint key_lengths[] = { 16, 16, 32 }, salt_lengths[] = { 14, 12, 12 };
  for (guint i = 0; i < 3; i++)
    {
      guint8 previous[45] = { 0 };
      for (guint repeat = 0; repeat < 2; repeat++)
        exchange (agent, server, profiles[i], profiles[i], key_lengths[i], salt_lengths[i], TRUE,
                  TRUE, previous);
      exchange (agent, server, profiles[i], profiles[i], key_lengths[i], salt_lengths[i], FALSE,
                TRUE, previous);
      exchange (agent, server, profiles[i], profiles[i], key_lengths[i], salt_lengths[i], TRUE,
                FALSE, previous);
      exchange (agent, server, profiles[i], profiles[(i + 1) % 3], key_lengths[i], salt_lengths[i],
                TRUE, TRUE, previous);
      OPENSSL_cleanse (previous, sizeof (previous));
      g_print ("PASS %s: exact exporter, fresh keys, rejected identity, missing verifier, no "
               "common profile, joined stop\n",
               profiles[i]);
    }
  pending_timer_shutdown (agent);
  g_object_unref (agent);
  g_object_unref (certificate);
  g_object_unref (server);
  return 0;
}
