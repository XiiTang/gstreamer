/* Exercise native loss detection, NACK/RTX repair and explicit PLI/FIR. */
#include "gstruntimertpsession.h"
#include <gst/rtp/gstrtcpbuffer.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <string.h>

static guint
rtcp_kinds (const guint8 *bytes, gsize length)
{
  GstBuffer *buffer = gst_buffer_new_memdup (bytes, length);
  GstRTCPBuffer rtcp = GST_RTCP_BUFFER_INIT;
  GstRTCPPacket packet;
  guint kinds = 0;
  g_assert_true (gst_rtcp_buffer_map (buffer, GST_MAP_READ, &rtcp));
  if (gst_rtcp_buffer_get_first_packet (&rtcp, &packet))
    do
      {
        GstRTCPType type = gst_rtcp_packet_get_type (&packet);
        if (type == GST_RTCP_TYPE_RTPFB
            && gst_rtcp_packet_fb_get_type (&packet) == GST_RTCP_RTPFB_TYPE_NACK)
          kinds |= 1;
        if (type == GST_RTCP_TYPE_PSFB
            && gst_rtcp_packet_fb_get_type (&packet) == GST_RTCP_PSFB_TYPE_PLI)
          kinds |= 2;
        if (type == GST_RTCP_TYPE_PSFB
            && gst_rtcp_packet_fb_get_type (&packet) == GST_RTCP_PSFB_TYPE_FIR)
          kinds |= 4;
      }
    while (gst_rtcp_packet_move_to_next (&packet));
  gst_rtcp_buffer_unmap (&rtcp);
  gst_buffer_unref (buffer);
  return kinds;
}
static gpointer
feedback_pressure (gpointer owner)
{
  const guint8 nack[]
      = { 128, 201, 0, 1, 0, 0, 0, 8, 129, 205, 0, 3, 0, 0, 0, 8, 0, 0, 0, 7, 0, 2, 0, 0 };
  while (gst_runtime_rtp_session_write (owner, 2, nack, sizeof (nack)) == 0)
    {
    }
  return NULL;
}
int
main (int argc, char **argv)
{
  extern void gst_runtime_initialize (void);
  gst_runtime_initialize ();
  GstRuntimeRtxSettings at = { 97, 70, 8, 80, 32, 1000 }, bt = { 97, 80, 7, 70, 32, 1000 };
  GstRuntimeRtpSettings ac = { .ssrc = 7,
                               .payload_type = 96,
                               .clock_rate = 90000,
                               .probation = 0,
                               .rtcp_min_interval = 10 * GST_MSECOND,
                               .reports = TRUE,
                               .feedback = 7,
                               .rtx = &at,
                               .reorder = TRUE,
                               .latency_ms = 200,
                               .bandwidth_bps = 128000 };
  GstRuntimeRtpSettings bc = ac;
  bc.ssrc = 8;
  bc.rtx = &bt;
  GstRuntimeRtpSession *a = gst_runtime_rtp_session_new (&ac),
                       *b = gst_runtime_rtp_session_new (&bc);
  g_assert_nonnull (a);
  g_assert_nonnull (b);
  for (guint seq = 1; seq <= 3; seq++)
    {
      guint8 packet[]
          = { 128, 96, 0, seq, 0, 0, (seq * 3000) >> 8, (seq * 3000) & 255, 0, 0, 0, 7, seq };
      g_assert_cmpint (gst_runtime_rtp_session_write (a, 0, packet, sizeof (packet)), ==, 0);
    }
  guint original = 0, repaired = 0, delivered = 0, feedback = 0;
  gboolean pli = FALSE, fir = FALSE;
  gint64 deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  while (g_get_monotonic_time () < deadline && !(delivered == 3 && feedback == 7))
    {
      guint8 bytes[65536];
      gsize n = 0;
      while (gst_runtime_rtp_session_read (a, 0, bytes, sizeof (bytes), &n, 0) == 0)
        {
          GstBuffer *buffer = gst_buffer_new_memdup (bytes, n);
          GstRTPBuffer packet = GST_RTP_BUFFER_INIT;
          g_assert_true (gst_rtp_buffer_map (buffer, GST_MAP_READ, &packet));
          guint pt = gst_rtp_buffer_get_payload_type (&packet),
                seq = gst_rtp_buffer_get_seq (&packet);
          gboolean drop = pt == 96 && seq == 2;
          if (pt == 96)
            original++;
          else
            {
              g_assert_cmpuint (pt, ==, 97);
              g_assert_cmpuint (gst_rtp_buffer_get_ssrc (&packet), ==, 70);
              guint8 *payload = gst_rtp_buffer_get_payload (&packet);
              g_assert_cmpuint (gst_rtp_buffer_get_payload_len (&packet), ==, 3);
              g_assert_cmpuint (payload[0] * 256 + payload[1], ==, 2);
              g_assert_cmpuint (payload[2], ==, 2);
              repaired++;
            }
          gst_rtp_buffer_unmap (&packet);
          gst_buffer_unref (buffer);
          if (!drop)
            g_assert_cmpint (gst_runtime_rtp_session_write (b, 1, bytes, n), ==, 0);
        }
      while (gst_runtime_rtp_session_read (b, 2, bytes, sizeof (bytes), &n, 0) == 0)
        {
          feedback |= rtcp_kinds (bytes, n);
          g_assert_cmpint (gst_runtime_rtp_session_write (a, 2, bytes, n), ==, 0);
        }
      while (gst_runtime_rtp_session_read (a, 2, bytes, sizeof (bytes), &n, 0) == 0)
        g_assert_cmpint (gst_runtime_rtp_session_write (b, 2, bytes, n), ==, 0);
      while (gst_runtime_rtp_session_read (b, 1, bytes, sizeof (bytes), &n, 0) == 0)
        {
          delivered++;
          g_assert_cmpuint (delivered, <=, 3);
          g_assert_cmpuint (n, ==, 13);
          g_assert_cmpuint (bytes[3], ==, delivered);
          g_assert_cmpuint (bytes[11], ==, 7);
          g_assert_cmpuint (bytes[12], ==, delivered);
        }
      if (!pli && repaired)
        {
          g_assert_cmpint (gst_runtime_rtp_session_feedback (b, 2, 7, 0, 0), ==, 1);
          pli = TRUE;
        }
      if (!fir && (feedback & 2))
        {
          g_assert_cmpint (gst_runtime_rtp_session_feedback (b, 4, 7, 0, 0), ==, 1);
          fir = TRUE;
        }
      g_usleep (1000);
    }
  g_assert_cmpuint (original, ==, 3);
  g_assert_cmpuint (repaired, >=, 1);
  g_assert_cmpuint (delivered, ==, 3);
  g_assert_cmpuint (feedback, ==, 7);
  gchar *stats = gst_runtime_rtp_session_stats (a);
  g_assert_nonnull (strstr (stats, "rtx-sent"));
  gst_runtime_rtp_session_stats_free (stats);
  GThread *pressure = g_thread_new ("feedback-pressure", feedback_pressure, a);
  g_usleep (50000);
  gint64 stopped = g_get_monotonic_time ();
  gst_runtime_rtp_session_stop (a);
  g_thread_join (pressure);
  g_assert_cmpint (g_get_monotonic_time () - stopped, <, G_USEC_PER_SEC);
  gst_runtime_rtp_session_free (a);
  gst_runtime_rtp_session_free (b);
  ac.rtx = NULL;
  ac.feedback = 0;
  a = gst_runtime_rtp_session_new (&ac);
  g_assert_nonnull (a);
  for (guint kind = 1; kind <= 4; kind <<= 1)
    g_assert_cmpint (gst_runtime_rtp_session_feedback (a, kind, 7, 0, 0), <, 0);
  gst_runtime_rtp_session_free (a);
  ac.rtx = &at;
  g_assert_null (gst_runtime_rtp_session_new (&ac));
  g_print ("PASS declared native NACK/RTX loss repair, exact RTX mapping and payload, ordered "
           "recovery, explicit PLI/FIR and undeclared rejection\n");
}
