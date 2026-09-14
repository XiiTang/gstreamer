#ifndef GST_RUNTIME_SRTP_API_H
#define GST_RUNTIME_SRTP_API_H
#include <gst/gst.h>
#include <srtp2/srtp_runtime.h>
G_BEGIN_DECLS
GST_API
int gst_runtime_srtp_create (const srtp_runtime_options *options, const guint8 *state, gsize length,
                             int restore, srtp_runtime_context **out);
GST_API
int gst_runtime_srtp_create_dtls (const srtp_runtime_options *, srtp_runtime_context **out);
GST_API
int gst_runtime_srtp_export (srtp_runtime_context *context, guint8 *output, gsize *length);
GST_API
int gst_runtime_srtp_packet (srtp_runtime_context *context, int sending, int rtcp, guint8 *packet,
                             gsize capacity, gsize *length);
GST_API
void gst_runtime_srtp_free (srtp_runtime_context *context);
G_END_DECLS
#endif
