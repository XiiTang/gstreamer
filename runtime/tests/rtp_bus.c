/* Real RTCP SDES must not accumulate bus messages or retain a closed pipeline. */
#include "../gstruntimertpsession.c"
#include "test_notify.h"
#include "test_sink.h"

typedef struct {
  GMutex lock;
  GCond changed;
  guint updates;
} Sdes;

static void
sdes_received (GstElement *rtp, guint ssrc, gpointer data)
{
  Sdes *sdes = data;
  g_assert_cmpuint (ssrc, ==, 8);
  /* rtpsession posts the ELEMENT message before emitting on-ssrc-sdes. */
  g_mutex_lock (&sdes->lock);
  sdes->updates++;
  g_cond_broadcast (&sdes->changed);
  g_mutex_unlock (&sdes->lock);
}

static void
finalized (gpointer data, GObject *object)
{
  g_atomic_int_set ((gint *)data, TRUE);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  TestSink facts = {0};
  GstRuntimeDiagnosticSink sink = test_sink (&facts);
  GstRuntimeRtpSettings settings = { .ssrc = 7, .payload_type = 96, .clock_rate = 90000 };
  GstRuntimeRtpSession *s = gst_runtime_rtp_session_new (&settings, &sink, test_notify ());
  g_assert_nonnull (s);
  Sdes sdes = {0};
  gint pipeline_finalized = FALSE;
  g_object_weak_ref (G_OBJECT (s->pipeline), finalized, &pipeline_finalized);
  g_signal_connect (s->rtp, "on-ssrc-sdes", G_CALLBACK (sdes_received), &sdes);
  gboolean retained = FALSE;
  for (guint i = 1; i <= 64; i++)
    {
      GstBuffer *buffer = gst_rtcp_buffer_new (256);
      GstRTCPBuffer rtcp = GST_RTCP_BUFFER_INIT;
      GstRTCPPacket packet;
      gchar cname[32];
      g_snprintf (cname, sizeof (cname), "peer-%u", i);
      g_assert_true (gst_rtcp_buffer_map (buffer, GST_MAP_READWRITE, &rtcp));
      g_assert_true (gst_rtcp_buffer_add_packet (&rtcp, GST_RTCP_TYPE_RR, &packet));
      gst_rtcp_packet_rr_set_ssrc (&packet, 8);
      g_assert_true (gst_rtcp_buffer_add_packet (&rtcp, GST_RTCP_TYPE_SDES, &packet));
      g_assert_true (gst_rtcp_packet_sdes_add_item (&packet, 8));
      g_assert_true (gst_rtcp_packet_sdes_add_entry (&packet, GST_RTCP_SDES_CNAME,
                                                    strlen (cname), (guint8 *)cname));
      gst_rtcp_buffer_unmap (&rtcp);
      g_assert_true (gst_rtcp_buffer_validate (buffer));
      GstMapInfo bytes;
      g_assert_true (gst_buffer_map (buffer, &bytes, GST_MAP_READ));
      g_assert_cmpint (gst_runtime_rtp_session_try_write (s, 2, bytes.data, bytes.size), ==, 0);
      gst_buffer_unmap (buffer, &bytes);
      gst_buffer_unref (buffer);
      g_mutex_lock (&sdes.lock);
      gint64 stall = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
      while (sdes.updates < i)
        g_assert_true (g_cond_wait_until (&sdes.changed, &sdes.lock, stall));
      g_assert_cmpuint (sdes.updates, ==, i);
      g_mutex_unlock (&sdes.lock);
      /* Do not pop/drain: that would hide the missing production consumer. */
      retained |= gst_bus_have_pending (s->bus);
    }
  GstRuntimeRtpStopOutcome outcome;
  g_assert_cmpint (gst_runtime_rtp_session_close (s, &outcome), ==, 0);
  g_assert_cmpint (outcome.state_change, ==, GST_STATE_CHANGE_SUCCESS);
  g_assert_cmpuint (outcome.bus_errors, ==, 0);
  retained |= gst_bus_have_pending (s->bus);
  gst_runtime_rtp_session_free (s);
  test_sink_assert_none (&facts);
  g_print ("64 real RTCP SDES updates: retained bus messages=%d, pipeline finalized=%d\n",
           retained, g_atomic_int_get (&pipeline_finalized));
  g_assert_false (retained);
  g_assert_true (g_atomic_int_get (&pipeline_finalized));
  g_cond_clear (&sdes.changed);
  g_mutex_clear (&sdes.lock);
  g_print ("PASS bounded SDES bus retention and pipeline finalization after free\n");
}
