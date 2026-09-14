#include "gstruntimedtls.h"
#include "../subprojects/gst-plugins-bad/ext/dtls/gstdtlsagent.h"
#include "../subprojects/gst-plugins-bad/ext/dtls/gstdtlscertificate.h"
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <string.h>

struct GstRuntimeDtls
{
  GstDtlsConnection *connection;
  GstDtlsAgent *agent;
  GstDtlsCertificate *certificate;
  GMutex mutex;
  GQueue output;
  GstRuntimeDtlsKeys keys;
  gboolean encoder, decoder, taken, started, failed;
};

static gboolean
send_packet (GstDtlsConnection *connection, gconstpointer data, gsize length, gpointer value)
{
  GstRuntimeDtls *self = value;
  g_mutex_lock (&self->mutex);
  gboolean accepted = !self->failed && length > 0 && length <= 65535 && self->output.length < 32;
  if (accepted)
    g_queue_push_tail (&self->output, g_bytes_new (data, length));
  else
    self->failed = TRUE;
  g_mutex_unlock (&self->mutex);
  return accepted;
}

static void
save_key (GstRuntimeDtls *self, gboolean sending, gconstpointer key, guint length, guint cipher,
          guint auth)
{
  guint32 profile = 0;
  if (cipher == GST_DTLS_SRTP_CIPHER_AES_128_ICM && auth == GST_DTLS_SRTP_AUTH_HMAC_SHA1_80
      && length == 30)
    profile = 1;
  if (cipher == GST_DTLS_SRTP_CIPHER_AES_128_GCM && auth == GST_DTLS_SRTP_AUTH_NULL && length == 28)
    profile = 2;
  if (cipher == GST_DTLS_SRTP_CIPHER_AES_256_GCM && auth == GST_DTLS_SRTP_AUTH_NULL && length == 44)
    profile = 3;
  g_mutex_lock (&self->mutex);
  if (!profile || (sending ? self->encoder : self->decoder) || self->taken
      || (self->keys.profile && self->keys.profile != profile))
    self->failed = TRUE;
  else
    {
      self->keys.profile = profile;
      self->keys.length = length;
      memcpy (sending ? self->keys.send : self->keys.receive, key, length);
      if (sending)
        self->encoder = TRUE;
      else
        self->decoder = TRUE;
    }
  g_mutex_unlock (&self->mutex);
}
static void
encoder_key (GstDtlsConnection *connection, gconstpointer key, guint length, guint cipher,
             guint auth, gpointer data)
{
  save_key (data, TRUE, key, length, cipher, auth);
}
static void
decoder_key (GstDtlsConnection *connection, gconstpointer key, guint length, guint cipher,
             guint auth, gpointer data)
{
  save_key (data, FALSE, key, length, cipher, auth);
}

int
gst_runtime_dtls_create (const gchar *pem, const gchar *profiles, GstDtlsVerifyChain verify,
                         gpointer data, GstRuntimeDtls **out)
{
  if (!out || !profiles || !verify)
    return -2;
  *out = NULL;
  /* Validate the supplied identity before constructors that use GLib assertions. */
  if (pem)
    {
      gsize length = strlen (pem);
      if (!length || length > 1024 * 1024)
        return -2;
      BIO *bio = BIO_new_mem_buf (pem, length);
      X509 *cert = bio ? PEM_read_bio_X509 (bio, NULL, NULL, NULL) : NULL;
      BIO_free (bio);
      bio = BIO_new_mem_buf (pem, length);
      EVP_PKEY *key = bio ? PEM_read_bio_PrivateKey (bio, NULL, NULL, "") : NULL;
      gboolean valid = cert && key && X509_check_private_key (cert, key) == 1;
      EVP_PKEY_free (key);
      X509_free (cert);
      BIO_free (bio);
      if (!valid)
        return -2;
    }
  GstRuntimeDtls *self = g_new0 (GstRuntimeDtls, 1);
  g_mutex_init (&self->mutex);
  g_queue_init (&self->output);
  /* Avoid a transient GValue-owned, non-cleansed copy of the private PEM. */
  GValue identity = G_VALUE_INIT;
  const gchar *property = "pem";
  g_value_init (&identity, G_TYPE_STRING);
  g_value_set_static_string (&identity, pem);
  self->certificate = GST_DTLS_CERTIFICATE (
      g_object_new_with_properties (GST_TYPE_DTLS_CERTIFICATE, 1, &property, &identity));
  g_value_unset (&identity);
  self->agent = g_object_new (GST_TYPE_DTLS_AGENT, "certificate", self->certificate, NULL);
  SSL_CTX *ctx = _gst_dtls_agent_peek_context (self->agent);
  if (!ctx || !SSL_CTX_set_min_proto_version (ctx, DTLS1_2_VERSION)
      || !SSL_CTX_check_private_key (ctx))
    goto invalid;
  SSL_CTX_set_verify_depth (ctx, 15);
  /* Install the entire explicitly supplied chain on this unique agent only. */
  if (pem)
    {
      BIO *bio = BIO_new_mem_buf (pem, -1);
      X509 *cert = PEM_read_bio_X509 (bio, NULL, NULL, NULL);
      X509_free (cert);
      guint count = 1;
      while ((cert = PEM_read_bio_X509 (bio, NULL, NULL, NULL)))
        {
          gboolean valid = ++count <= 16 && SSL_CTX_add1_chain_cert (ctx, cert);
          X509_free (cert);
          if (!valid)
            {
              BIO_free (bio);
              goto invalid;
            }
        }
      BIO_free (bio);
      ERR_clear_error ();
    }
  self->connection = g_object_new (GST_TYPE_DTLS_CONNECTION, "agent", self->agent, NULL);
  if (!gst_dtls_connection_set_srtp_profiles (self->connection, profiles)
      || !gst_dtls_connection_set_chain_verifier (self->connection, verify, data))
    goto invalid;
  g_signal_connect (self->connection, "on-encoder-key", G_CALLBACK (encoder_key), self);
  g_signal_connect (self->connection, "on-decoder-key", G_CALLBACK (decoder_key), self);
  gst_dtls_connection_set_send_callback (self->connection, send_packet, self, NULL);
  *out = self;
  return 0;
invalid:
  gst_runtime_dtls_free (self);
  return -2;
}

int
gst_runtime_dtls_start (GstRuntimeDtls *self, int client)
{
  if (!self || self->started || (client != 0 && client != 1))
    return -2;
  self->started = TRUE;
  GError *error = NULL;
  gboolean success = gst_dtls_connection_start (self->connection, client, &error);
  g_clear_error (&error);
  if (!success)
    return -1001;
  gst_dtls_connection_check_timeout (self->connection);
  return 0;
}
int
gst_runtime_dtls_status (GstRuntimeDtls *self)
{
  if (!self)
    return -2;
  g_mutex_lock (&self->mutex);
  gboolean failed = self->failed;
  g_mutex_unlock (&self->mutex);
  if (failed)
    return -1001;
  GstDtlsConnectionState state;
  g_object_get (self->connection, "connection-state", &state, NULL);
  if (state == GST_DTLS_CONNECTION_STATE_CONNECTED)
    return 1;
  if (state == GST_DTLS_CONNECTION_STATE_FAILED)
    return -1001;
  if (state == GST_DTLS_CONNECTION_STATE_CLOSED)
    return -11;
  return 0;
}
int
gst_runtime_dtls_input (GstRuntimeDtls *self, guint8 *bytes, gsize length)
{
  if (!self || !self->started || !bytes || !length || length > 65535)
    return -2;
  if (gst_runtime_dtls_status (self) < 0)
    return -1001;
  GError *error = NULL;
  gsize written = 0;
  GstFlowReturn result
      = gst_dtls_connection_process (self->connection, bytes, length, &written, &error);
  g_clear_error (&error);
  /* This owner admits DTLS-SRTP only, never DTLS application data. */
  if (result < 0 || written)
    {
      g_mutex_lock (&self->mutex);
      self->failed = TRUE;
      g_mutex_unlock (&self->mutex);
      return result == GST_FLOW_EOS ? -11 : -1001;
    }
  gst_dtls_connection_check_timeout (self->connection);
  return gst_runtime_dtls_status (self) < 0 ? -1001 : 0;
}
int
gst_runtime_dtls_output (GstRuntimeDtls *self, guint8 *bytes, gsize *length)
{
  if (!self || !bytes || !length)
    return -2;
  g_mutex_lock (&self->mutex);
  GBytes *packet = g_queue_peek_head (&self->output);
  int result = 0;
  if (self->failed)
    result = -1001;
  else if (packet)
    {
      gsize size;
      const guint8 *data = g_bytes_get_data (packet, &size);
      if (*length < size)
        result = -2;
      else
        {
          memcpy (bytes, data, size);
          *length = size;
          g_bytes_unref (g_queue_pop_head (&self->output));
          result = 1;
        }
    }
  g_mutex_unlock (&self->mutex);
  return result;
}
int
gst_runtime_dtls_take_keys (GstRuntimeDtls *self, GstRuntimeDtlsKeys *keys)
{
  if (!self || !keys)
    return -2;
  int status = gst_runtime_dtls_status (self);
  if (status != 1)
    return status;
  g_mutex_lock (&self->mutex);
  int result = -2;
  if (!self->failed && self->encoder && self->decoder && !self->taken)
    {
      *keys = self->keys;
      OPENSSL_cleanse (&self->keys, sizeof (self->keys));
      self->taken = TRUE;
      result = 1;
    }
  g_mutex_unlock (&self->mutex);
  return result;
}
int
gst_runtime_dtls_fingerprint (GstRuntimeDtls *self, guint8 output[32])
{
  if (!self || !output)
    return -2;
  unsigned length = 0;
  X509 *cert = _gst_dtls_certificate_get_internal_certificate (self->certificate);
  return X509_digest (cert, EVP_sha256 (), output, &length) && length == 32 ? 0 : -2;
}
void
gst_runtime_dtls_free (GstRuntimeDtls *self)
{
  if (!self)
    return;
  if (self->connection)
    {
      gst_dtls_connection_stop_and_join (self->connection);
      g_object_unref (self->connection);
    }
  g_clear_object (&self->agent);
  g_clear_object (&self->certificate);
  g_queue_clear_full (&self->output, (GDestroyNotify)g_bytes_unref);
  g_mutex_clear (&self->mutex);
  OPENSSL_cleanse (self, sizeof (*self));
  g_free (self);
}
