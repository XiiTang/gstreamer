#include "test_sink.h"
#include "gstruntimepayload.h"
#include "gstruntimertpsession.h"
#include "gstruntimesdp.h"
#include <gst/rtp/gstrtpbuffer.h>
#include <string.h>

static void
session_roundtrip (GstRuntimePayloadFormat format, const guint8 *data, gsize length, gboolean exact,
                   const gchar *output_path)
{
  const guint8 asc[] = { 0x12, 0x10 };
  GstRuntimePayloadSettings codec = {
    .format = format,
    .payload_type = 96,
    .ssrc = 123456,
    .sequence = 65530,
    .timestamp = 0xffff0000,
    .mtu = 256,
    .clock_rate = format == GST_RUNTIME_PAYLOAD_OPUS   ? 48000
                  : format == GST_RUNTIME_PAYLOAD_AAC  ? 44100
                  : format >= GST_RUNTIME_PAYLOAD_PCMA ? 8000
                                                       : 90000,
    .channels = format == GST_RUNTIME_PAYLOAD_AAC || format == GST_RUNTIME_PAYLOAD_OPUS ? 2 : 1,
    .width = 64,
    .height = 64,
    .codec_data = format == GST_RUNTIME_PAYLOAD_AAC ? asc : NULL,
    .codec_data_length = format == GST_RUNTIME_PAYLOAD_AAC ? 2 : 0,
  };
  GstRuntimeRtpSettings settings = {
    .ssrc = codec.ssrc,
    .payload_type = codec.payload_type,
    .clock_rate = codec.clock_rate,
    .probation = 0,
    .payload = &codec,
    .reorder = TRUE,
    .latency_ms = 10,
  };
  const char *mapping[] = { NULL,
                            "H264/90000\r\na=fmtp:96 packetization-mode=1",
                            "H265/90000",
                            "JPEG/90000",
                            "opus/48000/2",
                            "MPEG4-GENERIC/44100/2\r\na=fmtp:96 streamtype=5; mode=AAC-hbr; "
                            "config=1210; sizeLength=13; indexLength=3; indexDeltaLength=3",
                            "PCMA/8000",
                            "PCMU/8000" };
  gchar *body
      = g_strdup_printf ("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=test\r\nt=0 0\r\n"
                         "m=%s 5004 RTP/AVP 96\r\na=rtpmap:96 %s\r\na=control:track\r\n",
                         format <= GST_RUNTIME_PAYLOAD_JPEG ? "video" : "audio", mapping[format]);
  GstRuntimeSdp *description = NULL;
  g_assert_cmpint (gst_runtime_sdp_new ((guint8 *)body, strlen (body), &description), ==, 0);
  GstCaps *selection = NULL;
  g_assert_cmpint (gst_runtime_sdp_select (description, 0, "rtsp://localhost/media/",
                                           "rtsp://localhost/media/track", "AVP", "tcp", FALSE,
                                           &settings, &selection),
                   ==, 0);
  settings.negotiated_caps = selection;
  gst_runtime_sdp_free (description);
  g_free (body);
  TestSink send_facts = {0};
  GstRuntimeDiagnosticSink send_sink = test_sink (&send_facts);
  GstRuntimeRtpSession *send = gst_runtime_rtp_session_new (&settings, &send_sink);
  g_assert_nonnull (send);
  codec.ssrc = settings.ssrc = 654321;
  TestSink receive_facts = {0};
  GstRuntimeDiagnosticSink receive_sink = test_sink (&receive_facts);
  GstRuntimeRtpSession *receive = gst_runtime_rtp_session_new (&settings, &receive_sink);
  g_assert_nonnull (receive);
  gst_runtime_sdp_selection_free (selection);
  g_assert_cmpint (
      gst_runtime_rtp_session_try_write_frame (send, data, length, 0, 20 * GST_MSECOND), ==,
      GST_FLOW_OK);
  guint count = 0;
  while (TRUE)
    {
      GstRuntimePayloadFrame *frame = NULL;
      int result = gst_runtime_rtp_session_pull (send, 0, count ? 50 * GST_MSECOND : 2 * GST_SECOND,
                                                 &frame);
      if (result == 1)
        break;
      g_assert_cmpint (result, ==, GST_FLOW_OK);
      GstRuntimePayloadFrameView view;
      gst_runtime_payload_frame_view (frame, &view);
      GstBuffer *packet = gst_buffer_new_memdup (view.data, view.length);
      GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
      g_assert_true (gst_rtp_buffer_map (packet, GST_MAP_READ, &rtp));
      g_assert_cmpuint (gst_rtp_buffer_get_ssrc (&rtp), ==, 123456);
      g_assert_cmpuint (gst_rtp_buffer_get_seq (&rtp), ==, (guint16)(65530 + count));
      g_assert_cmpuint (view.length, <=, 256);
      gst_rtp_buffer_unmap (&rtp);
      gst_buffer_unref (packet);
      g_assert_cmpint (gst_runtime_rtp_session_write (receive, 1, view.data, view.length), ==, 0);
      gst_runtime_payload_frame_free (frame);
      count++;
    }
  g_assert_cmpuint (count, >, 0);
  GstRuntimePayloadFrame *frame = NULL;
  g_assert_cmpint (gst_runtime_rtp_session_pull (receive, 1, 2 * GST_SECOND, &frame), ==, 0);
  GstRuntimePayloadFrameView view;
  gst_runtime_payload_frame_view (frame, &view);
  g_assert_cmpuint (view.pts, !=, GST_CLOCK_TIME_NONE);
  if (exact)
    g_assert_cmpmem (data, length, view.data, view.length);
  if (output_path)
    {
      gchar *path = g_strconcat (output_path, ".session", NULL);
      GError *error = NULL;
      g_assert_true (g_file_set_contents (path, (gchar *)view.data, view.length, &error));
      g_assert_no_error (error);
      g_free (path);
    }
  gst_runtime_payload_frame_free (frame);
  gst_runtime_rtp_session_free (send);
  test_sink_assert_none (&send_facts);
  gst_runtime_rtp_session_free (receive);
  test_sink_assert_none (&receive_facts);
  g_print ("PASS shared RTP session payload %u: %u packets, jitter, decoded frame timestamp\n",
           format, count);
}
static void
incomplete_access_unit_bound (void)
{
  for (int format = GST_RUNTIME_PAYLOAD_H264; format <= GST_RUNTIME_PAYLOAD_H265; format++)
    {
      GstRuntimePayloadSettings codec = { .format = format,
                                          .payload_type = 96,
                                          .ssrc = 7,
                                          .mtu = 1200,
                                          .clock_rate = 90000,
                                          .channels = 1 };
      GstRuntimeRtpSettings settings
          = { .ssrc = 7, .payload_type = 96, .clock_rate = 90000, .payload = &codec };
      TestSink session_facts = {0};
      GstRuntimeDiagnosticSink session_sink = test_sink (&session_facts);
      GstRuntimeRtpSession *session = gst_runtime_rtp_session_new (&settings, &session_sink);
      g_assert_nonnull (session);
      guint8 *packet = g_malloc0 (60000);
      packet[0] = 0x80;
      packet[1] = 96;
      packet[11] = 8;
      guint count = 0;
      int result = 0;
      gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
      while (result >= 0 && g_get_monotonic_time () < deadline)
        {
          packet[2] = count >> 8;
          packet[3] = count;
          packet[12] = format == GST_RUNTIME_PAYLOAD_H264 ? 0x7c : 0x62;
          packet[13] = format == GST_RUNTIME_PAYLOAD_H264 ? (count ? 5 : 0x85) : 1;
          if (format == GST_RUNTIME_PAYLOAD_H265)
            packet[14] = count ? 1 : 0x81;
          result = gst_runtime_rtp_session_try_write (session, 1, packet, 60000);
          if (result == 0)
            count++;
          if (result < 0)
            break;
          GstRuntimePayloadFrame *frame = NULL;
          result = gst_runtime_rtp_session_pull (session, 1, 0, &frame);
          g_assert_null (frame);
          if (result == 1)
            g_usleep (100);
        }
      g_assert_cmpint (result, ==, GST_FLOW_CUSTOM_ERROR);
      g_assert_cmpstr (session_facts.first_domain, ==, "gst-resource-error-quark");
      g_assert_cmpint (session_facts.first_code, ==, GST_RESOURCE_ERROR_NO_SPACE_LEFT);
      g_assert_cmpuint (count, <, 400);
      gst_runtime_rtp_session_free (session);
  g_free (session_facts.first_domain);
      g_free (packet);
    }
  g_print ("PASS incomplete H264/H265 fragments fail at native access-unit capacity\n");
}
static void
jitter_byte_pressure_stop (void)
{
  GstRuntimeRtpSettings settings
      = { .ssrc = 7, .payload_type = 96, .clock_rate = 90000, .reorder = TRUE, .latency_ms = 1 };
  TestSink session_facts = {0};
  GstRuntimeDiagnosticSink session_sink = test_sink (&session_facts);
  GstRuntimeRtpSession *session = gst_runtime_rtp_session_new (&settings, &session_sink);
  g_assert_nonnull (session);
  guint8 *packet = g_malloc0 (16000);
  packet[0] = 0x80;
  packet[1] = 96;
  packet[11] = 8;
  guint count = 0, pressured = 0;
  gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  while (pressured < 50 && g_get_monotonic_time () < deadline)
    {
      packet[2] = count >> 8;
      packet[3] = count;
      int result = gst_runtime_rtp_session_try_write (session, 1, packet, 16000);
      g_assert_cmpint (result, >=, 0);
      if (result == 0)
        {
          count++;
          pressured = 0;
        }
      else
        {
          pressured++;
          g_usleep (1000);
        }
      g_assert_cmpuint (count, <, 1500);
    }
  g_assert_cmpuint (pressured, ==, 50);
  gint64 start = g_get_monotonic_time ();
  gst_runtime_rtp_session_free (session);
  test_sink_assert_none (&session_facts);
  g_assert_cmpint (g_get_monotonic_time () - start, <, 200000);
  g_free (packet);
  g_print ("PASS equal-timestamp jitter byte pressure and joined stop\n");
}
static void
raw_and_stop (void)
{
  GstRuntimeRtpSettings settings
      = { .ssrc = 0x05060708, .payload_type = 0, .clock_rate = 8000 };
  TestSink raw_facts = {0};
  GstRuntimeDiagnosticSink raw_sink = test_sink (&raw_facts);
  GstRuntimeRtpSession *raw = gst_runtime_rtp_session_new (&settings, &raw_sink);
  g_assert_nonnull (raw);
  const guint8 packet[] = { 0x80, 0, 0xff, 0xff, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0xff, 0, 1 };
  g_assert_cmpint (gst_runtime_rtp_session_try_write (raw, 0, packet, sizeof (packet)), ==, 0);
  GstRuntimePayloadFrame *frame = NULL;
  g_assert_cmpint (gst_runtime_rtp_session_pull (raw, 0, GST_SECOND, &frame), ==, 0);
  GstRuntimePayloadFrameView view;
  gst_runtime_payload_frame_view (frame, &view);
  g_assert_cmpmem (packet, sizeof (packet), view.data, view.length);
  gst_runtime_payload_frame_free (frame);
  const guint8 invalid[12] = { 0 };
  g_assert_cmpint (gst_runtime_rtp_session_try_write (raw, 0, invalid, sizeof (invalid)), <, 0);
  int result;
  gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  do
    {
      result = gst_runtime_rtp_session_try_write (raw, 0, packet, sizeof (packet));
      g_assert_cmpint (g_get_monotonic_time (), <, deadline);
    }
  while (result == 0);
  g_assert_cmpint (result, ==, 1);
  gst_runtime_rtp_session_stop (raw);
  g_assert_cmpint (gst_runtime_rtp_session_try_write (raw, 0, packet, sizeof (packet)), <, 0);
  GstRuntimeRtpStopOutcome outcome;
  g_assert_cmpint (gst_runtime_rtp_session_close (raw, &outcome), ==, 0);
  g_assert_cmpint (outcome.state_change, !=, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpuint (outcome.bus_errors, ==, 0);
  gst_runtime_rtp_session_free (raw);
  test_sink_assert_none (&raw_facts);
  g_print ("PASS shared RTP session raw bytes, malformed rejection, input pressure and joined stop\n");
}

static void
jpeg_rejection (const gchar *directory)
{
  gchar *path = g_build_filename (directory, "jpeg-optimized", NULL);
  gchar *data = NULL;
  gsize length;
  GError *error = NULL;
  g_assert_true (g_file_get_contents (path, &data, &length, &error));
  g_assert_no_error (error);
  GstRuntimePayloadSettings codec = { .format = GST_RUNTIME_PAYLOAD_JPEG,
                                       .payload_type = 96,
                                       .ssrc = 123,
                                       .mtu = 1200,
                                       .clock_rate = 90000,
                                       .channels = 1,
                                       .width = 64,
                                       .height = 64 };
  GstRuntimeRtpSettings settings
      = { .ssrc = 123, .payload_type = 96, .clock_rate = 90000, .payload = &codec };
  TestSink send_facts = {0};
  GstRuntimeDiagnosticSink send_sink = test_sink (&send_facts);
  GstRuntimeRtpSession *send = gst_runtime_rtp_session_new (&settings, &send_sink);
  g_assert_nonnull (send);
  g_assert_cmpint (gst_runtime_rtp_session_try_write_frame (send, (guint8 *)data, length, 0,
                                                          GST_CLOCK_TIME_NONE),
                   ==, 0);
  GstRuntimePayloadFrame *frame = NULL;
  g_assert_cmpint (gst_runtime_rtp_session_pull (send, 0, 5 * GST_SECOND, &frame), ==, GST_FLOW_CUSTOM_ERROR);
  g_assert_null (frame);
  gst_runtime_rtp_session_free (send);
  g_assert_cmpuint (send_facts.count, >=, 1);
  g_free (send_facts.first_domain);
  g_free (path);
  g_free (data);
  g_print ("PASS shared RTP session rejects optimized JPEG before emitting a corrupt packet\n");
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_assert_cmpint (argc, ==, 2);
  g_assert_null (gst_element_factory_find ("rtspsrc"));
  g_assert_null (gst_element_factory_find ("filesink"));
  guint8 audio[160];
  for (guint i = 0; i < 160; i++)
    audio[i] = (guint8)i;
  session_roundtrip (GST_RUNTIME_PAYLOAD_PCMA, audio, sizeof (audio), TRUE, NULL);
  session_roundtrip (GST_RUNTIME_PAYLOAD_PCMU, audio, sizeof (audio), TRUE, NULL);
  const guint8 opus[] = { 0xf8, 0xff, 0xfe };
  session_roundtrip (GST_RUNTIME_PAYLOAD_OPUS, opus, sizeof (opus), TRUE, NULL);
  const gchar *names[] = { "h264", "h265", "jpeg", "aac" };
  const GstRuntimePayloadFormat formats[] = { GST_RUNTIME_PAYLOAD_H264, GST_RUNTIME_PAYLOAD_H265,
                                              GST_RUNTIME_PAYLOAD_JPEG, GST_RUNTIME_PAYLOAD_AAC };
  for (guint i = 0; i < 4; i++)
    {
      gchar *input = g_build_filename (argv[1], names[i], NULL), *encoded = NULL;
      gsize length = 0;
      GError *error = NULL;
      g_assert_true (g_file_get_contents (input, &encoded, &length, &error));
      g_assert_no_error (error);
      gchar *output = g_strconcat (input, ".recovered", NULL);
      session_roundtrip (formats[i], (guint8 *)encoded, length, i == 3, output);
      g_free (encoded);
      g_free (input);
      g_free (output);
    }
  incomplete_access_unit_bound ();
  jitter_byte_pressure_stop ();
  jpeg_rejection (argv[1]);
  raw_and_stop ();
  return 0;
}
