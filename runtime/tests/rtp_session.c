#include "gstruntimertpsession.h"
#include <gst/rtp/gstrtcpbuffer.h>
#include <gst/rtp/gstrtpdefs.h>
#include <string.h>
static const guint8 rtp[]
    = { 0xb2, 0xe0, 0xff, 0xfe, 0, 0, 0,    1,    0, 0, 0, 7, 0, 0, 0, 10, 0, 0,
        0,    11,   0xbe, 0xde, 0, 1, 0x10, 0x77, 0, 0, 1, 2, 3, 4, 0, 0,  0, 4 };
static gpointer
blocked_writer (gpointer data)
{
  GstRuntimeRtpSession *s = data;
  for (int i = 0; i < 10000; i++)
    if (gst_runtime_rtp_session_write (s, 0, rtp, sizeof (rtp)) != 0)
      return NULL;
  g_assert_not_reached ();
}
int
main (int argc, char **argv)
{
  gst_init (NULL, NULL);
  /* A request to send RTCP never silently enables an unnegotiated profile. */
  GstElement *probe = gst_element_factory_make ("rtpsession", NULL);
  GObject *engine = NULL;
  g_object_get (probe, "internal-session", &engine, NULL);
  for (int profile = GST_RTP_PROFILE_AVP; profile <= GST_RTP_PROFILE_SAVPF; profile++)
    {
      g_object_set (engine, "rtp-profile", profile, NULL);
      gboolean scheduled = FALSE;
      g_signal_emit_by_name (engine, "send-rtcp-full", GST_SECOND, &scheduled);
      int actual = 0;
      g_object_get (engine, "rtp-profile", &actual, NULL);
      g_assert_cmpint (actual, ==, profile);
    }
  g_object_unref (engine);
  gst_object_unref (probe);
  GstRuntimeRtpSettings settings = { 7, 96, 90000, 0, 10 * GST_MSECOND, TRUE, TRUE };
  settings.bandwidth_bps = 128000;
  GstRuntimeRtpSession *a = gst_runtime_rtp_session_new (&settings);
  settings.ssrc = 8;
  GstRuntimeRtpSession *b = gst_runtime_rtp_session_new (&settings);
  g_assert_nonnull (a);
  g_assert_nonnull (b);
  guint8 original[sizeof (rtp)], buffer[65536];
  gsize length;
  for (int i = 0; i < 3; i++)
    {
      memcpy (original, rtp, sizeof (rtp));
      guint16 seq = 65534 + i;
      original[2] = seq >> 8;
      original[3] = seq;
      g_assert_cmpint (gst_runtime_rtp_session_write (a, 0, original, sizeof (original)), ==, 0);
      g_assert_cmpint (
          gst_runtime_rtp_session_read (a, 0, buffer, sizeof (buffer), &length, GST_SECOND), ==, 0);
      g_assert_cmpmem (buffer, length, original, sizeof (original));
      g_assert_cmpint (gst_runtime_rtp_session_write (b, 1, buffer, length), ==, 0);
      g_assert_cmpint (
          gst_runtime_rtp_session_read (b, 1, buffer, sizeof (buffer), &length, GST_SECOND), ==, 0);
      g_assert_cmpmem (buffer, length, original, sizeof (original));
    }
  /* Reports are native protocol output; the Runtime transports them explicitly. */
  guint blocks = 0;
  gint64 report_deadline = g_get_monotonic_time () + 3000000;
  while (blocks == 0)
    {
      g_assert_cmpint (g_get_monotonic_time (), <, report_deadline);
      /* Repeated early requests must not starve an already-due regular report.
       * The periodic variant also checks the native autonomous schedule. */
      if (argc == 1 || strcmp (argv[1], "periodic"))
        gst_runtime_rtp_session_report (b, GST_SECOND);
      int result = gst_runtime_rtp_session_read (b, 2, buffer, sizeof (buffer), &length,
                                                 100 * GST_MSECOND);
      if (result == 1)
        continue; /* Native randomized RTCP scheduling need not meet each poll. */
      g_assert_cmpint (result, ==, 0);
      GstBuffer *report = gst_buffer_new_allocate (NULL, length, NULL);
      gst_buffer_fill (report, 0, buffer, length);
      g_assert_true (gst_rtcp_buffer_validate (report));
      GstRTCPBuffer mapped = GST_RTCP_BUFFER_INIT;
      GstRTCPPacket packet;
      g_assert_true (gst_rtcp_buffer_map (report, GST_MAP_READ, &mapped));
      g_assert_true (gst_rtcp_buffer_get_first_packet (&mapped, &packet));
      g_assert_cmpint (gst_rtcp_packet_get_type (&packet), ==, GST_RTCP_TYPE_RR);
      g_assert_cmpuint (gst_rtcp_packet_rr_get_ssrc (&packet), ==, 8);
      blocks = gst_rtcp_packet_get_rb_count (&packet);
      gst_rtcp_buffer_unmap (&mapped);
      gst_buffer_unref (report);
    }
  g_assert_cmpuint (blocks, ==, 1);
  g_assert_cmpint (gst_runtime_rtp_session_write (a, 2, buffer, length), ==, 0);
  gchar *stats = gst_runtime_rtp_session_stats (b);
  g_assert_nonnull (stats);
  g_assert_nonnull (strstr (stats, "source-stats"));
  g_assert_nonnull (strstr (stats, "packets-received"));
  g_free (stats);
  /* Invalid headers/PT/SSRC are rejected before asynchronous admission. */
  memcpy (original, rtp, sizeof (rtp));
  original[1] = 97;
  g_assert_cmpint (gst_runtime_rtp_session_write (a, 0, original, sizeof (original)), <, 0);
  memcpy (original, rtp, sizeof (rtp));
  original[11] = 9;
  g_assert_cmpint (gst_runtime_rtp_session_write (a, 0, original, sizeof (original)), <, 0);
  g_assert_cmpint (gst_runtime_rtp_session_write (a, 2, rtp, sizeof (rtp)), <, 0);
  gst_runtime_rtp_session_free (a);
  gst_runtime_rtp_session_free (b);
  settings.ssrc = 7;
  settings.reports = FALSE;
  a = gst_runtime_rtp_session_new (&settings);
  g_assert_nonnull (a);
  g_assert_false (gst_runtime_rtp_session_report (a, GST_SECOND));
  GThread *thread = g_thread_new ("blocked-rtp-writer", blocked_writer, a);
  g_usleep (30000);
  gint64 start = g_get_monotonic_time ();
  gst_runtime_rtp_session_stop (a);
  g_thread_join (thread);
  gst_runtime_rtp_session_free (a);
  g_assert_cmpint (g_get_monotonic_time () - start, <, 200000);
  g_print ("RTP session: raw fields/rollover, native RR/stats, declared reports, full queue stop "
           "passed\n");
  return 0;
}
