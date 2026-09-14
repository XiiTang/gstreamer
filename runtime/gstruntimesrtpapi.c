#include "gstruntimesrtpapi.h"
/* SRTP initialization is process-global; native GStreamer SRTP elements use
 * the same library. Never shut it down while other native owners can exist. */
extern int gst_srtp_initialize_library (void);
int
gst_runtime_srtp_create (const srtp_runtime_options *options, const guint8 *state, gsize length,
                         int restore, srtp_runtime_context **out)
{
  int status = gst_srtp_initialize_library ();
  if (status)
    {
      if (out)
        *out = NULL;
      return status;
    }
  return restore ? srtp_runtime_restore (options, state, length, out)
                 : srtp_runtime_create (options, out);
}
int
gst_runtime_srtp_export (srtp_runtime_context *context, guint8 *output, gsize *length)
{
  return srtp_runtime_export (context, output, length);
}
int
gst_runtime_srtp_packet (srtp_runtime_context *context, int sending, int rtcp, guint8 *packet, gsize capacity,
                         gsize *length)
{
  return srtp_runtime_packet (context, sending, rtcp, packet, capacity, length);
}
void
gst_runtime_srtp_free (srtp_runtime_context *context)
{
  srtp_runtime_free (context);
}
