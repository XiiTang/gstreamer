/* Exercise the real timeout callback with a failed OpenSSL timer operation.
 * Including the implementation keeps fault injection out of the native ABI. */
#include <gst/gst.h>
#include <openssl/ssl.h>

typedef struct {
  GMutex lock; GCond changed;
  gpointer watched, runtime_mutex, disposer;
  gboolean wrap_next, in_callback, release, disposer_waiting, fence_entered;
  gint seq, callback_done, fence_returned, torn_down, free_returned;
  const gchar *domain; gint code, slot;
} Probe;
static Probe probe;
static void test_mutex_lock (GMutex *mutex) {
  if (mutex == g_atomic_pointer_get (&probe.watched) &&
      g_thread_self () == g_atomic_pointer_get (&probe.disposer)) {
    g_mutex_lock (&probe.lock); probe.disposer_waiting = TRUE;
    g_cond_broadcast (&probe.changed); g_mutex_unlock (&probe.lock);
  }
  g_mutex_lock (mutex);
}
#define g_mutex_lock(m) test_mutex_lock (m)

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

static void gated_failure (GstDtlsConnection *, const GError *, gpointer);
static GstDtlsConnectionFailureCallback real_failure;
static gboolean test_set_failure_callback (GstDtlsConnection *c, GstDtlsConnectionFailureCallback f, gpointer d)
{
  if (probe.wrap_next) { probe.wrap_next = FALSE; real_failure = f;
    return gst_dtls_connection_set_failure_callback (c, gated_failure, d); }
  return gst_dtls_connection_set_failure_callback (c, f, d);
}
static void test_stop_and_join (GstDtlsConnection *c)
{
  g_mutex_lock (&probe.lock); probe.fence_entered = TRUE; g_cond_broadcast (&probe.changed); g_mutex_unlock (&probe.lock);
  gst_dtls_connection_stop_and_join (c);
  g_mutex_lock (&probe.lock); probe.fence_returned = g_atomic_int_add (&probe.seq, 1) + 1;
  g_cond_broadcast (&probe.changed); g_mutex_unlock (&probe.lock);
}
static void test_mutex_clear (GMutex *m)
{
  if (m == g_atomic_pointer_get (&probe.runtime_mutex)) {
    g_mutex_lock (&probe.lock); probe.torn_down = g_atomic_int_add (&probe.seq, 1) + 1;
    g_cond_broadcast (&probe.changed); g_mutex_unlock (&probe.lock);
  }
  g_mutex_clear (m);
}
#define gst_dtls_connection_set_failure_callback(c,f,d) test_set_failure_callback (c,f,d)
#define gst_dtls_connection_stop_and_join(c) test_stop_and_join (c)
#define g_mutex_clear(m) test_mutex_clear (m)
#include "../gstruntimedtls.c"
#undef gst_dtls_connection_set_failure_callback
#undef gst_dtls_connection_stop_and_join
#undef g_mutex_clear

static void gated_failure (GstDtlsConnection *c, const GError *e, gpointer d)
{
  g_mutex_lock (&probe.lock); probe.in_callback = TRUE; g_cond_broadcast (&probe.changed);
  while (!probe.release) g_cond_wait (&probe.changed, &probe.lock);
  g_mutex_unlock (&probe.lock);
  g_assert_true (((GstRuntimeDtls *)d)->connection == c);
  real_failure (c, e, d);
  gsize length;
  probe.slot = gst_runtime_dtls_failure (d, &probe.domain, &probe.code, NULL, 0, &length);
  g_mutex_lock (&probe.lock); probe.callback_done = g_atomic_int_add (&probe.seq, 1) + 1;
  g_cond_broadcast (&probe.changed); g_mutex_unlock (&probe.lock);
}
typedef struct { guint count; GQuark domain; gint code; } Failures;
static void record (GstDtlsConnection *c, const GError *e, gpointer data)
{
  Failures *f = data;
  if (!f->count) { f->domain = e->domain; f->code = e->code; }
  f->count++;
}
static GstDtlsConnection *fresh (void)
{
  GstDtlsCertificate *cert = g_object_new (GST_TYPE_DTLS_CERTIFICATE, NULL);
  GstDtlsAgent *agent = g_object_new (GST_TYPE_DTLS_AGENT, "certificate", cert, NULL);
  GstDtlsConnection *c = g_object_new (GST_TYPE_DTLS_CONNECTION, "agent", agent, NULL);
  g_object_unref (agent); g_object_unref (cert); return c;
}
static GError *timer_then_process (GstDtlsConnection *c)
{
  c->priv->connection_state = GST_DTLS_CONNECTION_STATE_CONNECTING;
  GstDtlsConnectionState state; g_object_get (c, "connection-state", &state, NULL);
  g_assert_cmpint (state, ==, GST_DTLS_CONNECTION_STATE_CONNECTING);
  timeout_result = -1; handle_timeout (NULL, c);
  guint8 bytes[] = {0}; gsize written; GError *error = NULL;
  g_assert_cmpint (gst_dtls_connection_process (c, bytes, sizeof (bytes), &written, &error), ==, GST_FLOW_ERROR);
  g_assert_error (error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ);
  return error;
}
static void timer_failure_is_first_over_its_consequence (void)
{
  GstDtlsConnection *c = fresh (); Failures failures = {0};
  g_assert_true (gst_dtls_connection_set_failure_callback (c, record, &failures));
  GError *error = timer_then_process (c);
  g_assert_cmpuint (failures.count, ==, 1);
  g_assert_cmpuint (failures.domain, ==, GST_RESOURCE_ERROR);
  g_assert_cmpint (failures.code, ==, GST_RESOURCE_ERROR_FAILED);
  g_clear_error (&error); gst_dtls_connection_stop_and_join (c); g_object_unref (c);
}
static void runtime_slot_keeps_the_timer_failure (void)
{
  GstRuntimeDtls *self = g_new0 (GstRuntimeDtls, 1);
  g_mutex_init (&self->mutex); self->connection = fresh ();
  g_assert_true (gst_dtls_connection_set_failure_callback (self->connection, failure_callback, self));
  GError *error = timer_then_process (self->connection);
  fail (self, error, GST_RUNTIME_DTLS_ERROR_PROCESS); g_clear_error (&error);
  const gchar *domain; gint code; gsize length;
  g_assert_cmpint (gst_runtime_dtls_failure (self, &domain, &code, NULL, 0, &length), ==, 1);
  g_assert_cmpstr (domain, ==, "gst-resource-error-quark"); g_assert_cmpint (code, ==, 1);
  gst_runtime_dtls_free (self);
}
static gboolean drop_packet (GstDtlsConnection *c, gconstpointer p, gsize n, gpointer data) { return TRUE; }
static void failure_callback_is_construction_only (void)
{
  Failures a = {0}, b = {0}; GstDtlsConnection *c1 = fresh (), *c2 = fresh (), *c3 = fresh ();
  g_assert_true (gst_dtls_connection_set_failure_callback (c1, record, &a));
  g_assert_false (gst_dtls_connection_set_failure_callback (c1, record, &b));
  g_assert_false (gst_dtls_connection_set_failure_callback (c1, NULL, &b));
  c1->priv->connection_state = GST_DTLS_CONNECTION_STATE_CONNECTING;
  timeout_result = -1; handle_timeout (NULL, c1);
  g_assert_cmpuint (a.count, ==, 1); g_assert_cmpuint (a.domain, ==, GST_RESOURCE_ERROR);
  g_assert_cmpint (a.code, ==, GST_RESOURCE_ERROR_FAILED);
  GError *error = NULL; gst_dtls_connection_set_send_callback (c2, drop_packet, NULL, NULL);
  g_assert_true (gst_dtls_connection_start (c2, TRUE, &error)); g_assert_no_error (error);
  g_assert_false (gst_dtls_connection_set_failure_callback (c2, record, &b));
  gst_dtls_connection_stop_and_join (c2);
  gst_dtls_connection_stop_and_join (c3);
  g_assert_false (gst_dtls_connection_set_failure_callback (c3, record, &b));
  g_assert_cmpuint (b.count, ==, 0);
  gst_dtls_connection_stop_and_join (c1);
  g_object_unref (c1); g_object_unref (c2); g_object_unref (c3);
}
static gboolean accept (const GstDtlsCertificateDer *chain, gsize length, gpointer data) { return TRUE; }
static gpointer dispose_runtime (gpointer self)
{
  g_atomic_pointer_set (&probe.disposer, g_thread_self ());
  gst_runtime_dtls_free (self);
  g_mutex_lock (&probe.lock); probe.free_returned = g_atomic_int_add (&probe.seq, 1) + 1;
  g_cond_broadcast (&probe.changed); g_mutex_unlock (&probe.lock); return NULL;
}
static void failure_callback_data_outlives_a_running_callback (void)
{
  /* Earlier cases used the wrappers too; only this callback is gated. */
  probe.fence_entered = FALSE; probe.fence_returned = probe.torn_down = probe.free_returned = 0;
  probe.wrap_next = TRUE; GstRuntimeDtls *self = NULL;
  g_assert_cmpint (gst_runtime_dtls_create (NULL, "SRTP_AES128_CM_SHA1_80", accept, NULL, &self), ==, 0);
  self->connection->priv->connection_state = GST_DTLS_CONNECTION_STATE_CONNECTING;
  timeout_result = -1;
  g_atomic_pointer_set (&probe.watched, &self->connection->priv->mutex);
  g_atomic_pointer_set (&probe.runtime_mutex, &self->mutex);
  g_thread_pool_push (self->connection->priv->thread_pool, GINT_TO_POINTER (0xc0ffee), NULL);
  g_mutex_lock (&probe.lock);
  while (!probe.in_callback) g_cond_wait (&probe.changed, &probe.lock);
  g_mutex_unlock (&probe.lock);
  GThread *f = g_thread_new ("free-runtime", dispose_runtime, self);
  g_mutex_lock (&probe.lock);
  while (!probe.disposer_waiting && !probe.fence_returned && !probe.torn_down && !probe.free_returned)
    g_cond_wait (&probe.changed, &probe.lock);
  g_assert_true (probe.fence_entered); g_assert_true (probe.disposer_waiting);
  g_assert_cmpint (probe.fence_returned, ==, 0); g_assert_cmpint (probe.torn_down, ==, 0); g_assert_cmpint (probe.free_returned, ==, 0);
  probe.release = TRUE; g_cond_broadcast (&probe.changed); g_mutex_unlock (&probe.lock);
  g_thread_join (f);
  g_assert_cmpint (probe.slot, ==, 1); g_assert_cmpstr (probe.domain, ==, "gst-resource-error-quark");
  g_assert_cmpint (probe.code, ==, 1);
  g_assert_cmpint (probe.callback_done, <, probe.fence_returned);
  g_assert_cmpint (probe.fence_returned, <, probe.torn_down);
  g_assert_cmpint (probe.torn_down, <, probe.free_returned);
  g_atomic_pointer_set (&probe.watched, NULL); g_atomic_pointer_set (&probe.runtime_mutex, NULL); g_atomic_pointer_set (&probe.disposer, NULL);
}

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
  timer_failure_is_first_over_its_consequence ();
  runtime_slot_keeps_the_timer_failure ();
  failure_callback_is_construction_only ();
  failure_callback_data_outlives_a_running_callback ();
  g_print ("PASS DTLS timeout failure: FAILED, one unlocked notification, stopped timer ignored\n");
  return 0;
}
