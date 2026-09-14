/* Native DTLS client against an independent OpenSSL DTLS server.
 * Synthetic keys stay in process and are never printed or written to disk. */
#include "gstdtlsconnection.h"
#include "gstruntimedtls.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct
{
  X509 *certificate;
  EVP_PKEY *key;
  X509 *intermediate;
  X509 *root;
} ServerIdentity;

typedef struct
{
  X509 *root;
  int socket;
  gboolean trust, verified;
  guint count, enc_length, dec_length, cipher, auth;
  guint8 expected_digest[32], encoder[44], decoder[44];
} Client;

static gboolean
verify_peer (const GstDtlsCertificateDer *chain, gsize count, gpointer data)
{
  Client *client = data;
  g_assert_cmpuint (count, ==, 2);
  const unsigned char *cursor = chain[0].data;
  X509 *cert = d2i_X509 (NULL, &cursor, chain[0].length);
  guint8 digest[32];
  unsigned length = 0;
  client->verified = cert && X509_digest (cert, EVP_sha256 (), digest, &length) && length == 32
                     && !CRYPTO_memcmp (digest, client->expected_digest, 32) && client->trust;
  STACK_OF (X509) *intermediates = sk_X509_new_null ();
  cursor = chain[1].data;
  X509 *intermediate = d2i_X509 (NULL, &cursor, chain[1].length);
  sk_X509_push (intermediates, intermediate);
  X509_STORE *store = X509_STORE_new ();
  X509_STORE_add_cert (store, client->root);
  X509_STORE_CTX *ctx = X509_STORE_CTX_new ();
  g_assert_cmpint (X509_STORE_CTX_init (ctx, store, cert, intermediates), ==, 1);
  X509_VERIFY_PARAM_set1_host (X509_STORE_CTX_get0_param (ctx), "localhost", 0);
  client->verified = client->verified && X509_verify_cert (ctx) == 1;
  X509_STORE_CTX_free (ctx);
  X509_STORE_free (store);
  sk_X509_pop_free (intermediates, X509_free);
  X509_free (cert);
  return client->verified;
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
exchange (ServerIdentity *server_certificate, const char *profile, const char *server_profile,
          guint key_length, guint salt_length, gboolean trust, gboolean install_verifier,
          gboolean identity, guint8 *previous)
{
  struct sockaddr_in client_address, server_address;
  Client client = { .root = server_certificate->root, .trust = trust };
  client.socket = udp_socket (&client_address);
  int server_socket = udp_socket (&server_address);
  g_assert_cmpint (
      connect (client.socket, (struct sockaddr *)&server_address, sizeof (server_address)), ==, 0);
  g_assert_cmpint (
      connect (server_socket, (struct sockaddr *)&client_address, sizeof (client_address)), ==, 0);
  X509 *server_cert = server_certificate->certificate;
  unsigned digest_length;
  g_assert_cmpint (X509_digest (server_cert, EVP_sha256 (), client.expected_digest, &digest_length),
                   ==, 1);
  SSL_CTX *context = SSL_CTX_new (DTLS_server_method ());
  g_assert_cmpint (SSL_CTX_set_min_proto_version (context, DTLS1_2_VERSION), ==, 1);
  g_assert_cmpint (SSL_CTX_use_certificate (context, server_cert), ==, 1);
  g_assert_cmpint (SSL_CTX_use_PrivateKey (context, server_certificate->key), ==, 1);
  g_assert_cmpint (SSL_CTX_set_tlsext_use_srtp (context, server_profile), ==, 0);
  g_assert_cmpint (SSL_CTX_add1_chain_cert (context, server_certificate->intermediate), ==, 1);
  g_assert_cmpint (X509_STORE_add_cert (SSL_CTX_get_cert_store (context), server_certificate->root),
                   ==, 1);
  SSL_CTX_set_verify (context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
  SSL *server = SSL_new (context);
  BIO *bio = BIO_new_dgram (server_socket, BIO_NOCLOSE);
  BIO_ctrl (bio, BIO_CTRL_DGRAM_SET_CONNECTED, 0, &client_address);
  SSL_set_bio (server, bio, bio);
  SSL_set_accept_state (server);
  SSL_set_mtu (server, 1200);
  GstRuntimeDtls *connection = NULL;
  BIO *private_identity = BIO_new (BIO_s_mem ());
  g_assert_cmpint (PEM_write_bio_X509 (private_identity, server_certificate->certificate), ==, 1);
  g_assert_cmpint (PEM_write_bio_X509 (private_identity, server_certificate->intermediate), ==, 1);
  g_assert_cmpint (PEM_write_bio_PrivateKey (private_identity, server_certificate->key, NULL, NULL,
                                             0, NULL, NULL),
                   ==, 1);
  BUF_MEM *memory;
  BIO_get_mem_ptr (private_identity, &memory);
  gchar *pem = g_strndup (memory->data, memory->length);
  int created = gst_runtime_dtls_create (
      identity ? pem : NULL, profile, install_verifier ? verify_peer : NULL, &client, &connection);
  OPENSSL_cleanse (pem, strlen (pem));
  g_free (pem);
  OPENSSL_cleanse (memory->data, memory->length);
  BIO_free (private_identity);
  if (!install_verifier)
    {
      g_assert_cmpint (created, <, 0);
      g_assert_null (connection);
      SSL_free (server);
      SSL_CTX_free (context);
      close (client.socket);
      close (server_socket);
      return;
    }
  g_assert_cmpint (created, ==, 0);
  g_assert_cmpint (gst_runtime_dtls_start (connection, 1), ==, 0);
  GstRuntimeDtlsKeys keys = { 0 };
  g_assert_cmpint (gst_runtime_dtls_take_keys (connection, &keys), ==, 0);
  gboolean server_done = FALSE, failed = FALSE;
  gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  while (g_get_monotonic_time () < deadline && !(client.enc_length && server_done))
    {
      guint8 output[65535];
      gsize output_length = sizeof (output);
      int sent;
      while ((sent = gst_runtime_dtls_output (connection, output, &output_length)) == 1)
        {
          g_assert_cmpint (send (client.socket, output, output_length, 0), ==, output_length);
          client.count++;
          output_length = sizeof (output);
        }
      if (sent < 0)
        {
          failed = TRUE;
          break;
        }
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
          if (gst_runtime_dtls_input (connection, packet, size) < 0)
            {
              failed = TRUE;
              break;
            }
        }
      int ready = client.enc_length ? 0 : gst_runtime_dtls_take_keys (connection, &keys);
      if (ready < 0)
        {
          failed = TRUE;
          break;
        }
      if (ready == 1)
        {
          g_assert_true (client.verified);
          client.enc_length = client.dec_length = keys.length;
          memcpy (client.encoder, keys.send, keys.length);
          memcpy (client.decoder, keys.receive, keys.length);
        }

      if (failed)
        break;
      struct pollfd fds[2] = { { client.socket, POLLIN, 0 }, { server_socket, POLLIN, 0 } };
      poll (fds, 2, 2);
    }
  gboolean success = trust && install_verifier && identity && !strcmp (profile, server_profile);
  if (success)
    {
      g_assert_false (failed);
      g_assert_true (server_done);
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
      g_assert_cmpint (gst_runtime_dtls_start (connection, 1), <, 0);
      g_assert_cmpint (gst_runtime_dtls_take_keys (connection, &keys), <, 0);
    }
  else
    {
      g_assert_cmpuint (client.enc_length, ==, 0);
      g_assert_cmpuint (client.dec_length, ==, 0);
    }
  guint sent = client.count;
  gint64 before = g_get_monotonic_time ();
  gst_runtime_dtls_free (connection);
  g_assert_cmpint (g_get_monotonic_time () - before, <, 100000);
  g_assert_cmpuint (client.count, ==, sent);
  OPENSSL_cleanse (&keys, sizeof (keys));
  SSL_free (server);
  SSL_CTX_free (context);
  close (client.socket);
  close (server_socket);
  OPENSSL_cleanse (&client, sizeof (client));
}

static X509 *
certificate (EVP_PKEY *key, X509 *issuer, EVP_PKEY *issuer_key, long serial, gboolean ca)
{
  X509 *cert = X509_new ();
  g_assert_nonnull (key);
  g_assert_nonnull (cert);
  X509_set_version (cert, 2);
  ASN1_INTEGER_set (X509_get_serialNumber (cert), serial);
  X509_gmtime_adj (X509_getm_notBefore (cert), -60);
  X509_gmtime_adj (X509_getm_notAfter (cert), 3600);
  X509_set_pubkey (cert, key);
  X509_NAME *name = X509_get_subject_name (cert);
  gchar *cn = ca ? g_strdup_printf ("test authority %ld", serial) : g_strdup ("localhost");
  X509_NAME_add_entry_by_txt (name, "CN", MBSTRING_ASC, (const unsigned char *)cn, -1, -1, 0);
  g_free (cn);
  X509_set_issuer_name (cert, issuer ? X509_get_subject_name (issuer) : name);
  X509_EXTENSION *ext = X509V3_EXT_conf_nid (NULL, NULL, NID_basic_constraints,
                                             ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
  X509_add_ext (cert, ext, -1);
  X509_EXTENSION_free (ext);
  ext = X509V3_EXT_conf_nid (NULL, NULL, NID_key_usage,
                             ca ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature");
  X509_add_ext (cert, ext, -1);
  X509_EXTENSION_free (ext);
  if (!ca)
    {
      ext = X509V3_EXT_conf_nid (NULL, NULL, NID_subject_alt_name, "DNS:localhost");
      X509_add_ext (cert, ext, -1);
      X509_EXTENSION_free (ext);
    }
  g_assert_cmpint (X509_sign (cert, issuer_key, EVP_sha256 ()), >, 0);
  return cert;
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  EVP_PKEY *root_key = EVP_PKEY_Q_keygen (NULL, NULL, "EC", "prime256v1");
  EVP_PKEY *intermediate_key = EVP_PKEY_Q_keygen (NULL, NULL, "EC", "prime256v1");
  X509 *root = certificate (root_key, NULL, root_key, 1, TRUE);
  X509 *intermediate = certificate (intermediate_key, root, root_key, 2, TRUE);
  EVP_PKEY *leaf_key = EVP_PKEY_Q_keygen (NULL, NULL, "EC", "prime256v1");
  ServerIdentity identity
      = { .key = leaf_key,
          .certificate = certificate (leaf_key, intermediate, intermediate_key, 3, FALSE),
          .intermediate = intermediate,
          .root = root };
  ServerIdentity *server = &identity;
  const char *profiles[]
      = { "SRTP_AES128_CM_SHA1_80", "SRTP_AEAD_AES_128_GCM", "SRTP_AEAD_AES_256_GCM" };
  const guint key_lengths[] = { 16, 16, 32 }, salt_lengths[] = { 14, 12, 12 };
  for (guint i = 0; i < 3; i++)
    {
      guint8 previous[45] = { 0 };
      for (guint repeat = 0; repeat < 2; repeat++)
        exchange (server, profiles[i], profiles[i], key_lengths[i], salt_lengths[i], TRUE, TRUE,
                  TRUE, previous);
      exchange (server, profiles[i], profiles[i], key_lengths[i], salt_lengths[i], FALSE, TRUE,
                TRUE, previous);
      exchange (server, profiles[i], profiles[i], key_lengths[i], salt_lengths[i], TRUE, FALSE,
                TRUE, previous);
      exchange (server, profiles[i], profiles[(i + 1) % 3], key_lengths[i], salt_lengths[i], TRUE,
                TRUE, TRUE, previous);
      exchange (server, profiles[i], profiles[i], key_lengths[i], salt_lengths[i], TRUE, TRUE,
                FALSE, previous);
      OPENSSL_cleanse (previous, sizeof (previous));
      g_print ("PASS opaque DTLS %s: independent exporter, verified mTLS chain, missing client "
               "identity rejection, fresh keys, rejected identity, missing "
               "verifier, profile mismatch, one-shot keys, joined stop\n",
               profiles[i]);
    }
  X509_free (server->certificate);
  EVP_PKEY_free (server->key);
  X509_free (root);
  X509_free (intermediate);
  EVP_PKEY_free (root_key);
  EVP_PKEY_free (intermediate_key);
  return 0;
}
