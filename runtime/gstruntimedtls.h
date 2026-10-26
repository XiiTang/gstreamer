#ifndef GST_RUNTIME_DTLS_H
#define GST_RUNTIME_DTLS_H
#include "../subprojects/gst-plugins-bad/ext/dtls/gstdtlsconnection.h"
#include <gst/gst.h>
G_BEGIN_DECLS
typedef struct GstRuntimeDtls GstRuntimeDtls;
typedef struct
{
  guint32 profile;
  gsize length;
  guint8 send[44], receive[44];
} GstRuntimeDtlsKeys;
/* pem is an optional private identity: leaf, chain and matching private key.
 * No shared agent cache, sockets, system trust, or implicit profile fallback. */
GST_API int gst_runtime_dtls_create (const gchar *pem, const gchar *profiles,
                                     GstDtlsVerifyChain verify, gpointer data,
                                     GstRuntimeDtls **out);
GST_API int gst_runtime_dtls_start (GstRuntimeDtls *, int client);
GST_API int gst_runtime_dtls_input (GstRuntimeDtls *, guint8 *, gsize);
/* 1 connected, 0 handshaking, negative failure/peer close. */
GST_API int gst_runtime_dtls_status (GstRuntimeDtls *);
/* 1 datagram, 0 empty; length is capacity on entry. Never removes on short buffer. */
GST_API int gst_runtime_dtls_output (GstRuntimeDtls *, guint8 *, gsize *length);
/* Take once, only after completed and verified handshake. */
GST_API int gst_runtime_dtls_take_keys (GstRuntimeDtls *, GstRuntimeDtlsKeys *);
GST_API int gst_runtime_dtls_fingerprint (GstRuntimeDtls *, guint8 output[32]);
/* Stop and join all callbacks before releasing owner and private key material. */
GST_API void gst_runtime_dtls_free (GstRuntimeDtls *);
typedef enum {
  GST_RUNTIME_DTLS_ERROR_OUTPUT_FULL = 1, /* 32 queued datagrams */
  GST_RUNTIME_DTLS_ERROR_OUTPUT_INVALID = 2, /* length 0 or > 65535 */
  GST_RUNTIME_DTLS_ERROR_KEY_INVALID = 3,
  GST_RUNTIME_DTLS_ERROR_APPLICATION_DATA = 4,
  GST_RUNTIME_DTLS_ERROR_CLOSED = 5,
  GST_RUNTIME_DTLS_ERROR_START = 6,
  GST_RUNTIME_DTLS_ERROR_PROCESS = 7
} GstRuntimeDtlsError;
GST_API GQuark gst_runtime_dtls_error_quark (void);
/* 1: first failure, domain is an immortal GLib quark string. Copies up to capacity
 * non-NUL-terminated message bytes, length is copied bytes. 0: none; -2: invalid. */
GST_API int gst_runtime_dtls_failure (GstRuntimeDtls *, const gchar **domain, gint *code,
                                     gchar *text, gsize capacity, gsize *length);
G_END_DECLS
#endif
