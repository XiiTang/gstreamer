/* Regression for delayed RTP extensions surviving dropped H264/H265 NALs. */
#include <gst/gst.h>
#include <gst/rtp/rtp.h>
#include <string.h>

typedef struct { GstRTPHeaderExtension parent; guint reads, stale; } TestExtension;
typedef struct { GstRTPHeaderExtensionClass parent; } TestExtensionClass;
G_DEFINE_TYPE (TestExtension, test_extension, GST_TYPE_RTP_HEADER_EXTENSION);
static GstRTPHeaderExtensionFlags flags (GstRTPHeaderExtension *ext) {
  return GST_RTP_HEADER_EXTENSION_ONE_BYTE;
}
static gsize max_size (GstRTPHeaderExtension *ext, const GstBuffer *buffer) { return 1; }
static gboolean read_extension (GstRTPHeaderExtension *ext,
    GstRTPHeaderExtensionFlags flags, const guint8 *data, gsize size, GstBuffer *buffer) {
  TestExtension *test = (TestExtension *)ext;
  g_assert_cmpuint (size, ==, 1);
  test->reads++;
  if (data[0] != 0x7f) test->stale++;
  return TRUE;
}
static void test_extension_class_init (TestExtensionClass *klass) {
  GstRTPHeaderExtensionClass *ext = GST_RTP_HEADER_EXTENSION_CLASS (klass);
  ext->get_supported_flags = flags;
  ext->get_max_size = max_size;
  ext->read = read_extension;
  gst_rtp_header_extension_class_set_uri (ext, "urn:boundless:test:drop");
  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Test extension", GST_RTP_HDREXT_ELEMENT_CLASS, "Dropped extension observer", "Boundless");
}
static void test_extension_init (TestExtension *self) {}
static guint outputs;
static GstFlowReturn output (GstPad *pad, GstObject *parent, GstBuffer *buffer) {
  outputs++;
  gst_buffer_unref (buffer);
  return GST_FLOW_OK;
}
static gboolean query (GstPad *pad, GstObject *parent, GstQuery *query) {
  if (GST_QUERY_TYPE (query) == GST_QUERY_CAPS) {
    GstCaps *caps = gst_caps_from_string (gst_pad_get_element_private (pad));
    gst_query_set_caps_result (query, caps);
    gst_caps_unref (caps);
    return TRUE;
  }
  return gst_pad_query_default (pad, parent, query);
}
static void push (GstPad *sink, const guint8 *payload, gsize size,
    guint16 sequence, guint8 extension) {
  GstBuffer *buffer = gst_rtp_buffer_new_allocate (size, 0, 0);
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  g_assert_true (gst_rtp_buffer_map (buffer, GST_MAP_WRITE, &rtp));
  gst_rtp_buffer_set_payload_type (&rtp, 96);
  gst_rtp_buffer_set_ssrc (&rtp, 123);
  gst_rtp_buffer_set_seq (&rtp, sequence);
  gst_rtp_buffer_set_timestamp (&rtp, sequence * 9000);
  gst_rtp_buffer_set_marker (&rtp, TRUE);
  memcpy (gst_rtp_buffer_get_payload (&rtp), payload, size);
  g_assert_true (gst_rtp_buffer_add_extension_onebyte_header (&rtp, 1, &extension, 1));
  gst_rtp_buffer_unmap (&rtp);
  GST_BUFFER_PTS (buffer) = sequence * 100 * GST_MSECOND;
  g_assert_cmpint (gst_pad_chain (sink, buffer), ==, GST_FLOW_OK);
}
static void run (gboolean h265) {
  outputs = 0;
  GstElement *depay = gst_element_factory_make (h265 ? "rtph265depay" : "rtph264depay", NULL);
  g_assert_nonnull (depay);
  GstPad *input = gst_element_get_static_pad (depay, "sink");
  GstPad *source = gst_element_get_static_pad (depay, "src");
  GstPad *sink = gst_pad_new ("observer", GST_PAD_SINK);
  gst_pad_set_element_private (sink, (gpointer)(h265
      ? "video/x-h265,stream-format=byte-stream,alignment=nal"
      : "video/x-h264,stream-format=byte-stream,alignment=nal"));
  gst_pad_set_chain_function (sink, output);
  gst_pad_set_query_function (sink, query);
  g_assert_true (gst_pad_set_active (sink, TRUE));
  g_assert_cmpint (gst_pad_link (source, sink), ==, GST_PAD_LINK_OK);
  TestExtension *ext = g_object_new (test_extension_get_type (), NULL);
  gst_object_ref_sink (ext);
  gst_rtp_header_extension_set_id (GST_RTP_HEADER_EXTENSION (ext), 1);
  g_signal_emit_by_name (depay, "add-extension", ext);
  g_assert_cmpint (gst_element_set_state (depay, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_true (gst_pad_send_event (input, gst_event_new_stream_start ("drop-regression")));
  GstCaps *caps = gst_caps_from_string (h265
      ? "application/x-rtp,media=video,clock-rate=90000,encoding-name=H265,payload=96"
      : "application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96,packetization-mode=1");
  g_assert_true (gst_pad_send_event (input, gst_event_new_caps (caps)));
  gst_caps_unref (caps);
  GstSegment segment;
  gst_segment_init (&segment, GST_FORMAT_TIME);
  g_assert_true (gst_pad_send_event (input, gst_event_new_segment (&segment)));
  /* A FU start followed by a short non-FU NAL delays the latter header while
   * finishing the previous FU. The completed FU is emitted, while the empty aggregation is dropped. */
  const guint8 start264[] = {28, 0x85};
  const guint8 start265[] = {98, 1, 0x93};
  const guint8 short264[] = {24};
  const guint8 short265[] = {96, 1};
  for (guint i = 0; i < 1024; i++) {
    push (input, h265 ? start265 : start264, h265 ? sizeof(start265) : sizeof(start264), 2*i, 1);
    push (input, h265 ? short265 : short264, h265 ? sizeof(short265) : sizeof(short264), 2*i+1, 2);
  }
  g_assert_cmpuint (outputs, ==, 1024);
  g_assert_cmpuint (ext->reads, ==, 1024);
  ext->reads = ext->stale = outputs = 0;
  const guint8 good264[] = {0x65, 0x88, 0x84};
  const guint8 good265[] = {0x26, 1, 0x88};
  push (input, h265 ? good265 : good264, 3, 2048, 0x7f);
  g_assert_cmpuint (outputs, ==, 1);
  g_assert_cmpuint (ext->reads, ==, 1);
  g_assert_cmpuint (ext->stale, ==, 0);
  ext->reads = ext->stale = 0;
  for (guint i = 0; i < 1024; i++)
    push (input, h265 ? short265 : short264, h265 ? sizeof(short265) : sizeof(short264), 2049+i, 2);
  g_assert_cmpuint (outputs, ==, 1);
  push (input, h265 ? good265 : good264, 3, 3073, 0x7f);
  g_assert_cmpuint (outputs, ==, 2);
  g_assert_cmpuint (ext->reads, ==, 1);
  g_assert_cmpuint (ext->stale, ==, 0);
  gst_element_set_state (depay, GST_STATE_NULL);
  gst_pad_unlink (source, sink);
  gst_pad_set_active (sink, FALSE);
  gst_object_unref (input);
  gst_object_unref (source);
  gst_object_unref (sink);
  gst_object_unref (depay);
  gst_object_unref (ext);
  g_print ("PASS H%u: 2048 empty aggregation drops, clean recovery, no stale extensions\n", h265 ? 265 : 264);
}
int main (int argc, char **argv) {
  gst_init (&argc, &argv);
  run (FALSE);
  run (TRUE);
  return 0;
}
