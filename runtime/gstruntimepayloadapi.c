#include "gstruntimepayloadapi.h"

struct _GstRuntimePayloadFrame
{
  GstSample *sample;
  GstMapInfo map;
};
static void
parameter (GstCaps *caps, const gchar *name, const gchar *value)
{
  if (value && value[0])
    gst_caps_set_simple (caps, name, G_TYPE_STRING, value, NULL);
}
GstCaps *
gst_runtime_payload_caps (const GstRuntimePayloadSettings *s)
{
  if (!s || s->format < 0 || s->format > GST_RUNTIME_PAYLOAD_PCMU || !s->clock_rate
      || s->clock_rate > G_MAXINT || s->channels > 255 || s->sequence > G_MAXUINT16
      || s->codec_data_length > 65536 || (s->codec_data_length && !s->codec_data))
    return NULL;
  const gboolean video
      = s->format >= GST_RUNTIME_PAYLOAD_H264 && s->format <= GST_RUNTIME_PAYLOAD_JPEG;
  if ((video && s->clock_rate != 90000)
      || (s->format == GST_RUNTIME_PAYLOAD_OPUS
          && (s->clock_rate != 48000 || !s->channels || s->channels > 2))
      || (s->format >= GST_RUNTIME_PAYLOAD_PCMA && (s->clock_rate != 8000 || s->channels != 1))
      || (s->format == GST_RUNTIME_PAYLOAD_AAC && (!s->channels || !s->codec_data_length)))
    return NULL;
  GstCaps *caps = NULL;
  if (!s->sending || s->format == GST_RUNTIME_PAYLOAD_RAW)
    {
      const gchar *names[]
          = { "RAW", "H264", "H265", "JPEG", "OPUS", "MPEG4-GENERIC", "PCMA", "PCMU" };
      caps = gst_caps_new_simple ("application/x-rtp", "media", G_TYPE_STRING,
                                  video ? "video" : "audio", "encoding-name", G_TYPE_STRING,
                                  names[s->format], "clock-rate", G_TYPE_INT, (gint)s->clock_rate,
                                  "payload", G_TYPE_INT, (gint)s->payload_type, NULL);
      if (s->format == GST_RUNTIME_PAYLOAD_H264)
        {
          gst_caps_set_simple (caps, "packetization-mode", G_TYPE_STRING, "1", NULL);
          parameter (caps, "sprop-parameter-sets", s->h264_parameter_sets);
        }
      else if (s->format == GST_RUNTIME_PAYLOAD_H265)
        {
          parameter (caps, "sprop-vps", s->h265_vps);
          parameter (caps, "sprop-sps", s->h265_sps);
          parameter (caps, "sprop-pps", s->h265_pps);
        }
      else if (s->format == GST_RUNTIME_PAYLOAD_AAC)
        {
          gchar *hex = g_malloc (s->codec_data_length * 2 + 1);
          for (gsize i = 0; i < s->codec_data_length; i++)
            g_snprintf (hex + i * 2, 3, "%02x", s->codec_data[i]);
          gst_caps_set_simple (caps, "mode", G_TYPE_STRING, "AAC-hbr", "streamtype", G_TYPE_STRING,
                               "5", "sizelength", G_TYPE_STRING, "13", "indexlength", G_TYPE_STRING,
                               "3", "indexdeltalength", G_TYPE_STRING, "3", "config", G_TYPE_STRING,
                               hex, NULL);
          g_free (hex);
        }
      else if (s->format == GST_RUNTIME_PAYLOAD_OPUS)
        {
          /* sprop-stereo is a sender hint, not a receiver compatibility limit. */
          gst_caps_set_simple (caps, "encoding-params", G_TYPE_STRING, "2", NULL);
        }
    }
  else if (s->format == GST_RUNTIME_PAYLOAD_H264 || s->format == GST_RUNTIME_PAYLOAD_H265)
    {
      caps = gst_caps_new_simple (
          s->format == GST_RUNTIME_PAYLOAD_H264 ? "video/x-h264" : "video/x-h265", "stream-format",
          G_TYPE_STRING, "byte-stream", "alignment", G_TYPE_STRING, "au", NULL);
    }
  else if (s->format == GST_RUNTIME_PAYLOAD_JPEG)
    {
      if (!s->width || !s->height || s->width > 2040 || s->height > 2040 || s->width % 8
          || s->height % 8)
        return NULL;
      caps = gst_caps_new_simple ("image/jpeg", "width", G_TYPE_INT, (gint)s->width, "height",
                                  G_TYPE_INT, (gint)s->height, NULL);
    }
  else if (s->format == GST_RUNTIME_PAYLOAD_AAC)
    {
      GstBuffer *config = gst_buffer_new_memdup (s->codec_data, s->codec_data_length);
      caps = gst_caps_new_simple ("audio/mpeg", "mpegversion", G_TYPE_INT, 4, "stream-format",
                                  G_TYPE_STRING, "raw", "rate", G_TYPE_INT, (gint)s->clock_rate,
                                  "channels", G_TYPE_INT, (gint)s->channels, "codec_data",
                                  GST_TYPE_BUFFER, config, NULL);
      gst_buffer_unref (config);
    }
  else
    {
      const gchar *name = s->format == GST_RUNTIME_PAYLOAD_OPUS   ? "audio/x-opus"
                          : s->format == GST_RUNTIME_PAYLOAD_PCMA ? "audio/x-alaw"
                                                                  : "audio/x-mulaw";
      caps = gst_caps_new_simple (name, "rate", G_TYPE_INT, (gint)s->clock_rate, "channels",
                                  G_TYPE_INT, (gint)s->channels, NULL);
      if (s->format == GST_RUNTIME_PAYLOAD_OPUS)
        gst_caps_set_simple (caps, "channel-mapping-family", G_TYPE_INT, 0, NULL);
    }
  return caps;
}
GstRuntimePayloadFrame *
gst_runtime_payload_frame_take (GstSample *sample)
{
  if (!sample)
    return NULL;
  GstRuntimePayloadFrame *frame = g_new0 (GstRuntimePayloadFrame, 1);
  frame->sample = sample;
  if (!gst_buffer_map (gst_sample_get_buffer (sample), &frame->map, GST_MAP_READ))
    {
      gst_sample_unref (sample);
      g_free (frame);
      return NULL;
    }
  return frame;
}

void
gst_runtime_payload_frame_view (GstRuntimePayloadFrame *frame, GstRuntimePayloadFrameView *view)
{
  GstBuffer *buffer = gst_sample_get_buffer (frame->sample);
  *view = (GstRuntimePayloadFrameView){ frame->map.data, frame->map.size, GST_BUFFER_PTS (buffer),
                                        GST_BUFFER_DURATION (buffer) };
}
void
gst_runtime_payload_frame_free (GstRuntimePayloadFrame *frame)
{
  gst_buffer_unmap (gst_sample_get_buffer (frame->sample), &frame->map);
  gst_sample_unref (frame->sample);
  g_free (frame);
}
