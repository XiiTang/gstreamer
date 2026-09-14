#include "gstruntimesdp.h"
#include <string.h>
#define HEADER "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=test\r\nt=0 0\r\n"
static void
check (const char *tail, GstRuntimeRtpSettings *settings, const char *profile, const char *lower,
       gboolean record, const char *uri, gboolean success)
{
  gchar *body = g_strconcat (HEADER, tail, NULL);
  GstRuntimeSdp *s = NULL;
  g_assert_cmpint (gst_runtime_sdp_new ((guint8 *)body, strlen (body), &s), ==, 0);
  GstCaps *caps = NULL;
  int result = gst_runtime_sdp_select (s, 0, "rtsp://localhost/media/", uri, profile, lower, record,
                                       settings, &caps);
  if (success)
    {
      g_assert_cmpint (result, ==, 0);
      g_assert_nonnull (caps);
    }
  else
    {
      g_assert_cmpint (result, ==, -2);
      g_assert_null (caps);
    }
  gst_runtime_sdp_selection_free (caps);
  gst_runtime_sdp_free (s);
  g_free (body);
}
#define CHECK(t, s, p, ok) check (t, s, p, "tcp", FALSE, "rtsp://localhost/media/track", ok)
int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  GstRuntimeRtpSettings s = { .ssrc = 1, .payload_type = 0, .clock_rate = 8000 };
  const char *pcmu = "m=audio 0 RTP/AVP 0\r\na=control:track\r\n";
  CHECK (pcmu, &s, "AVP", TRUE);
  CHECK (pcmu, &s, "SAVP", FALSE);
  check (pcmu, &s, "AVP", "tcp", FALSE, "rtsp://localhost/other", FALSE);
  s.payload_type = 8;
  CHECK (pcmu, &s, "AVP", FALSE);
  s.payload_type = 0;
  s.clock_rate = 16000;
  CHECK (pcmu, &s, "AVP", FALSE);
  s.clock_rate = 8000;
  CHECK ("a=control:track\r\nm=audio 0 RTP/AVP 0\r\n", &s, "AVP", TRUE);
  CHECK ("m=audio 0 RTP/AVP 0\r\n", &s, "AVP", FALSE);
  CHECK ("m=audio 0 RTP/AVP/UDP 0\r\na=control:track\r\n", &s, "AVP", FALSE);
  check ("m=audio 0 RTP/AVP/UDP 0\r\na=control:track\r\n", &s, "AVP", "udp", FALSE,
         "rtsp://localhost/media/track", TRUE);
  CHECK ("a=sendonly\r\nm=audio 0 RTP/AVP 0\r\na=control:track\r\n", &s, "AVP", FALSE);
  CHECK ("a=sendonly\r\nm=audio 0 RTP/AVP 0\r\na=recvonly\r\na=control:track\r\n", &s, "AVP", TRUE);
  check ("m=audio 0 RTP/AVP 0\r\na=recvonly\r\na=control:track\r\n", &s, "AVP", "tcp", TRUE,
         "rtsp://localhost/media/track", FALSE);
  CHECK ("m=audio 0 RTP/AVP 0\r\na=inactive\r\na=control:track\r\n", &s, "AVP", FALSE);
  GstRuntimePayloadSettings codec
      = { .format = GST_RUNTIME_PAYLOAD_H264, .payload_type = 96, .clock_rate = 90000 };
  s.payload = &codec;
  s.payload_type = 96;
  s.clock_rate = 90000;
#define H264 "m=video 0 RTP/AVP 96\r\na=control:track\r\na=rtpmap:96 H264/90000\r\n"
  CHECK (H264 "a=fmtp:96 packetization-mode=1\r\n", &s, "AVP", TRUE);
  CHECK (H264, &s, "AVP", FALSE);
  CHECK (H264 "a=fmtp:96 packetization-mode=2\r\n", &s, "AVP", FALSE);
  codec.format = GST_RUNTIME_PAYLOAD_H265;
  CHECK (H264 "a=fmtp:96 packetization-mode=1\r\n", &s, "AVP", FALSE);
  CHECK ("m=video 0 RTP/AVP 96\r\na=control:track\r\na=rtpmap:96 H265/90000\r\na=fmtp:96 "
         "sprop-max-don-diff=1\r\n",
         &s, "AVP", FALSE);
  codec.format = GST_RUNTIME_PAYLOAD_OPUS;
  codec.channels = 1;
  codec.clock_rate = s.clock_rate = 48000;
  CHECK ("m=audio 0 RTP/AVP 96\r\na=control:track\r\na=rtpmap:96 opus/48000/2\r\na=fmtp:96 "
         "sprop-stereo=1\r\n",
         &s, "AVP", TRUE);
  CHECK ("m=audio 0 RTP/AVP 96\r\na=control:track\r\na=rtpmap:96 opus/48000/1\r\n", &s, "AVP",
         FALSE);
  const guint8 asc[] = { 0x12, 0x10 };
  codec.format = GST_RUNTIME_PAYLOAD_AAC;
  codec.channels = 2;
  codec.codec_data = asc;
  codec.codec_data_length = 2;
  codec.clock_rate = s.clock_rate = 44100;
#define AAC "m=audio 0 RTP/AVP 96\r\na=control:track\r\na=rtpmap:96 MPEG4-GENERIC/44100/2\r\n"
#define AU "streamtype=5; mode=AAC-hbr; sizeLength=13; indexLength=3; indexDeltaLength=3"
  CHECK (AAC "a=fmtp:96 " AU "; config=1210\r\n", &s, "AVP", TRUE);
  CHECK (AAC "a=fmtp:96 " AU "\r\n", &s, "AVP", FALSE);
  CHECK (AAC "a=fmtp:96 " AU "; config=1190\r\n", &s, "AVP", FALSE);
  CHECK (AAC "a=fmtp:96 streamtype=5; mode=AAC-hbr; config=1210; sizeLength=12; indexLength=3; "
             "indexDeltaLength=3\r\n",
         &s, "AVP", FALSE);
  s.payload = NULL;
  s.clock_rate = 90000;
  s.feedback = 7;
  GstRuntimeRtxSettings rtx = { .payload_type = 97 };
  s.rtx = &rtx;
#define RTX                                                                                        \
  "m=video 0 RTP/AVPF 96 97\r\na=control:track\r\na=rtpmap:96 H264/90000\r\na=rtpmap:97 "          \
  "rtx/90000\r\na=rtcp-fb:96 nack\r\na=rtcp-fb:96 nack pli\r\na=rtcp-fb:96 ccm fir\r\n"
  CHECK (RTX "a=fmtp:97 apt=96\r\n", &s, "AVPF", TRUE);
  CHECK (RTX "a=fmtp:97 apt=95\r\n", &s, "AVPF", FALSE);
  CHECK (RTX, &s, "AVPF", FALSE);
  s.rtx = NULL;
  CHECK (
      "m=video 0 RTP/AVPF 96\r\na=control:track\r\na=rtpmap:96 H264/90000\r\na=rtcp-fb:96 nack\r\n",
      &s, "AVPF", FALSE);
  g_print ("PASS native SDP selection: identity, profile, payload, clock, inherited control, "
           "transport, direction, codec modes, Opus hints, feedback and RTX mapping\n");
}
