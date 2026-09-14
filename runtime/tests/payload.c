#include "gstruntimepayload.h"
#include "gstruntimertpsession.h"
#include "gstruntimesdp.h"
#include <gst/rtp/gstrtpbuffer.h>
#include <string.h>

static GstBuffer *
buffer (const guint8 *data, gsize length)
{
  GstBuffer *value = gst_buffer_new_memdup (data, length);
  GST_BUFFER_PTS (value) = 0;
  GST_BUFFER_DURATION (value) = 20 * GST_MSECOND;
  return value;
}
static void
payload_roundtrip (GstRuntimePayloadFormat format, GstCaps *caps, const guint8 *data, gsize length,
                   gboolean exact, const gchar *output_path)
{
  GError *error = NULL;
  GstRuntimePayload *send
      = gst_runtime_payload_new (format, TRUE, caps, 96, 123456, 65530, 0xffff0000, 256, &error);
  g_assert_no_error (error);
  g_assert_nonnull (send);
  g_assert_cmpint (gst_runtime_payload_push (send, buffer (data, length)), ==, GST_FLOW_OK);
  GstRuntimePayload *receive = NULL;
  guint count = 0;
  while (TRUE)
    {
      GstSample *sample
          = gst_runtime_payload_pull (send, count ? 50 * GST_MSECOND : 2 * GST_SECOND);
      if (!sample)
        break;
      if (!receive)
        {
          receive = gst_runtime_payload_new (format, FALSE, gst_sample_get_caps (sample), 96, 0, 0,
                                             0, 256, &error);
          g_assert_no_error (error);
          g_assert_nonnull (receive);
        }
      GstBuffer *packet = gst_sample_get_buffer (sample);
      GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
      g_assert_true (gst_rtp_buffer_map (packet, GST_MAP_READ, &rtp));
      g_assert_cmpuint (gst_rtp_buffer_get_ssrc (&rtp), ==, 123456);
      g_assert_cmpuint (gst_rtp_buffer_get_seq (&rtp), ==, (guint16)(65530 + count));
      g_assert_cmpuint (gst_buffer_get_size (packet), <=, 256);
      gst_rtp_buffer_unmap (&rtp);
      g_assert_cmpint (gst_runtime_payload_push (receive, gst_buffer_ref (packet)), ==,
                       GST_FLOW_OK);
      gst_sample_unref (sample);
      count++;
    }
  GstMessage *failure = gst_runtime_payload_error (send);
  if (failure)
    {
      GError *cause = NULL;
      gst_message_parse_error (failure, &cause, NULL);
      g_error ("Send failure: %s", cause->message);
    }
  g_assert_nonnull (receive);
  g_assert_cmpuint (count, >, 0);
  GstSample *decoded = gst_runtime_payload_pull (receive, 2 * GST_SECOND);
  if (!decoded)
    {
      failure = gst_runtime_payload_error (receive);
      if (failure)
        {
          GError *cause = NULL;
          gst_message_parse_error (failure, &cause, NULL);
          g_error ("Receive failure: %s", cause->message);
        }
    }
  g_assert_nonnull (decoded);
  GstMapInfo map;
  g_assert_true (gst_buffer_map (gst_sample_get_buffer (decoded), &map, GST_MAP_READ));
  if (exact)
    g_assert_cmpmem (data, length, map.data, map.size);
  if (output_path)
    g_assert_true (g_file_set_contents (output_path, (gchar *)map.data, map.size, &error));
  g_assert_no_error (error);
  gst_buffer_unmap (gst_sample_get_buffer (decoded), &map);
  gst_sample_unref (decoded);
  gst_runtime_payload_free (send);
  gst_runtime_payload_free (receive);
  g_print ("PASS payload %u: %u RTP packets, sequence wrap, bounded MTU, recovered frame\n", format,
           count);
}
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
  GstRuntimeRtpSession *send = gst_runtime_rtp_session_new (&settings);
  g_assert_nonnull (send);
  codec.ssrc = settings.ssrc = 654321;
  GstRuntimeRtpSession *receive = gst_runtime_rtp_session_new (&settings);
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
  gst_runtime_rtp_session_free (receive);
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
      GstRuntimeRtpSession *session = gst_runtime_rtp_session_new (&settings);
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
      g_assert_cmpint (result, <, 0);
      g_assert_cmpuint (count, <, 400);
      gst_runtime_rtp_session_free (session);
      g_free (packet);
    }
  g_print ("PASS incomplete H264/H265 fragments fail at native access-unit capacity\n");
}
static void
jitter_byte_pressure_stop (void)
{
  GstRuntimeRtpSettings settings
      = { .ssrc = 7, .payload_type = 96, .clock_rate = 90000, .reorder = TRUE, .latency_ms = 1 };
  GstRuntimeRtpSession *session = gst_runtime_rtp_session_new (&settings);
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
  g_assert_cmpint (g_get_monotonic_time () - start, <, 200000);
  g_free (packet);
  g_print ("PASS equal-timestamp jitter byte pressure and joined stop\n");
}
typedef struct
{
  GstRuntimePayload *payload;
  gint count;
  GstFlowReturn result;
} BlockedPush;
static gpointer
blocked_push (gpointer value)
{
  BlockedPush *push = value;
  const guint8 bytes[160] = { 0 };
  for (guint i = 0; i < 10000; i++)
    {
      push->result = gst_runtime_payload_push (push->payload, buffer (bytes, sizeof (bytes)));
      if (push->result != GST_FLOW_OK)
        break;
      g_atomic_int_inc (&push->count);
    }
  return NULL;
}
static void
raw_and_stop (void)
{
  GError *error = NULL;
  GstCaps *caps = gst_caps_new_simple ("application/x-rtp", "media", G_TYPE_STRING, "audio",
                                       "clock-rate", G_TYPE_INT, 8000, "encoding-name",
                                       G_TYPE_STRING, "PCMU", "payload", G_TYPE_INT, 0, NULL);
  GstRuntimePayload *raw
      = gst_runtime_payload_new (GST_RUNTIME_PAYLOAD_RAW, TRUE, caps, 0, 0, 0, 0, 1200, &error);
  g_assert_no_error (error);
  g_assert_nonnull (raw);
  const guint8 packet[] = { 0x80, 0, 0xff, 0xff, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0xff, 0, 1 };
  g_assert_cmpint (gst_runtime_payload_push (raw, buffer (packet, sizeof (packet))), ==,
                   GST_FLOW_OK);
  GstSample *sample = gst_runtime_payload_pull (raw, GST_SECOND);
  g_assert_nonnull (sample);
  guint8 got[sizeof (packet)];
  g_assert_cmpuint (gst_buffer_extract (gst_sample_get_buffer (sample), 0, got, sizeof (got)), ==,
                    sizeof (got));
  g_assert_cmpmem (packet, sizeof (packet), got, sizeof (got));
  gst_sample_unref (sample);
  const guint8 invalid[12] = { 0 };
  g_assert_cmpint (gst_runtime_payload_push (raw, buffer (invalid, sizeof (invalid))), ==,
                   GST_FLOW_ERROR);
  gst_runtime_payload_free (raw);
  gst_caps_unref (caps);
  caps = gst_caps_new_simple ("audio/x-mulaw", "rate", G_TYPE_INT, 8000, "channels", G_TYPE_INT, 1,
                              NULL);
  BlockedPush push = { .payload = gst_runtime_payload_new (GST_RUNTIME_PAYLOAD_PCMU, TRUE, caps, 0,
                                                           99, 0, 0, 1200, &error) };
  g_assert_no_error (error);
  gst_caps_unref (caps);
  GThread *worker = g_thread_new ("bounded-payload", blocked_push, &push);
  gint64 deadline = g_get_monotonic_time () + G_USEC_PER_SEC;
  while (g_atomic_int_get (&push.count) < 8 && g_get_monotonic_time () < deadline)
    g_usleep (1000);
  g_usleep (20000);
  g_assert_cmpint (g_atomic_int_get (&push.count), <, 10000);
  gint64 start = g_get_monotonic_time ();
  gst_runtime_payload_stop (push.payload);
  g_thread_join (worker);
  g_assert_cmpint (g_get_monotonic_time () - start, <, 200000);
  g_assert_cmpint (push.result, ==, GST_FLOW_FLUSHING);
  gst_runtime_payload_free (push.payload);
  g_print (
      "PASS raw RTP exact bytes, malformed RTP rejection, bounded backpressure and joined stop\n");
}

static void
jpeg_rejection (const gchar *directory)
{
  gchar *path = g_build_filename (directory, "jpeg-optimized", NULL);
  gchar *data = NULL;
  gsize length;
  GError *error = NULL;
  g_assert_true (g_file_get_contents (path, &data, &length, &error));
  GstCaps *caps
      = gst_caps_new_simple ("image/jpeg", "width", G_TYPE_INT, 64, "height", G_TYPE_INT, 64, NULL);
  GstRuntimePayload *send
      = gst_runtime_payload_new (GST_RUNTIME_PAYLOAD_JPEG, TRUE, caps, 96, 123, 0, 0, 1200, &error);
  g_assert_no_error (error);
  g_assert_nonnull (send);
  g_assert_cmpint (gst_runtime_payload_push (send, buffer ((guint8 *)data, length)), ==,
                   GST_FLOW_OK);
  g_assert_null (gst_runtime_payload_pull (send, GST_SECOND));
  GstMessage *failure = gst_runtime_payload_error (send);
  g_assert_nonnull (failure);
  gst_message_parse_error (failure, &error, NULL);
  g_assert_nonnull (strstr (error->message, "Huffman"));
  g_clear_error (&error);
  gst_message_unref (failure);
  gst_runtime_payload_free (send);
  gst_caps_unref (caps);
  g_free (path);
  g_free (data);
  g_print ("PASS optimized JPEG rejected before emitting a corrupt RTP frame\n");
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
  GstCaps *caps = gst_caps_new_simple ("audio/x-alaw", "rate", G_TYPE_INT, 8000, "channels",
                                       G_TYPE_INT, 1, NULL);
  payload_roundtrip (GST_RUNTIME_PAYLOAD_PCMA, caps, audio, sizeof (audio), TRUE, NULL);
  session_roundtrip (GST_RUNTIME_PAYLOAD_PCMA, audio, sizeof (audio), TRUE, NULL);
  gst_caps_unref (caps);
  caps = gst_caps_new_simple ("audio/x-mulaw", "rate", G_TYPE_INT, 8000, "channels", G_TYPE_INT, 1,
                              NULL);
  payload_roundtrip (GST_RUNTIME_PAYLOAD_PCMU, caps, audio, sizeof (audio), TRUE, NULL);
  gst_caps_unref (caps);
  session_roundtrip (GST_RUNTIME_PAYLOAD_PCMU, audio, sizeof (audio), TRUE, NULL);
  const guint8 opus[] = { 0xf8, 0xff, 0xfe };
  caps = gst_caps_new_simple ("audio/x-opus", "rate", G_TYPE_INT, 48000, "channels", G_TYPE_INT, 2,
                              "channel-mapping-family", G_TYPE_INT, 0, NULL);
  payload_roundtrip (GST_RUNTIME_PAYLOAD_OPUS, caps, opus, sizeof (opus), TRUE, NULL);
  gst_caps_unref (caps);
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
      if (i < 2)
        caps = gst_caps_new_simple (i == 0 ? "video/x-h264" : "video/x-h265", "stream-format",
                                    G_TYPE_STRING, "byte-stream", "alignment", G_TYPE_STRING, "au",
                                    NULL);
      else if (i == 2)
        caps = gst_caps_new_simple ("image/jpeg", "width", G_TYPE_INT, 64, "height", G_TYPE_INT, 64,
                                    NULL);
      else
        {
          const guint8 asc[] = { 0x12, 0x10 };
          GstBuffer *configuration = gst_buffer_new_memdup (asc, 2);
          caps = gst_caps_new_simple ("audio/mpeg", "mpegversion", G_TYPE_INT, 4, "stream-format",
                                      G_TYPE_STRING, "raw", "rate", G_TYPE_INT, 44100, "channels",
                                      G_TYPE_INT, 2, "codec_data", GST_TYPE_BUFFER, configuration,
                                      NULL);
          gst_buffer_unref (configuration);
        }
      gchar *output = g_strconcat (input, ".recovered", NULL);
      payload_roundtrip (formats[i], caps, (guint8 *)encoded, length, i == 3, output);
      session_roundtrip (formats[i], (guint8 *)encoded, length, i == 3, output);
      gst_caps_unref (caps);
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
