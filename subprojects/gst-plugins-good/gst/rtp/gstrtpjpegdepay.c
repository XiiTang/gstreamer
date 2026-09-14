/* GStreamer
 * Copyright (C) <2008> Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/rtp/gstrtpbuffer.h>
#include <gst/video/video.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gstrtpelements.h"
#include "gstrtpjpegdepay.h"
#include "gstrtputils.h"

GST_DEBUG_CATEGORY_STATIC (rtpjpegdepay_debug);
#define GST_CAT_DEFAULT (rtpjpegdepay_debug)

static GstStaticPadTemplate gst_rtp_jpeg_depay_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("image/jpeg")
    );

static GstStaticPadTemplate gst_rtp_jpeg_depay_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp, "
        "media = (string) \"video\", "
        "clock-rate = (int) 90000, " "encoding-name = (string) \"JPEG\"; "
        /* optional SDP attributes */
        /*
         * "a-framerate = (string) 0.00, "
         * "x-framerate = (string) 0.00, "
         * "x-dimensions = (string) \"1234,1234\", "
         */
        "application/x-rtp, "
        "media = (string) \"video\", "
        "payload = (int) " GST_RTP_PAYLOAD_JPEG_STRING ", "
        "clock-rate = (int) 90000"
        /* optional SDP attributes */
        /*
         * "a-framerate = (string) 0.00, "
         * "x-framerate = (string) 0.00, "
         * "x-dimensions = (string) \"1234,1234\""
         */
    )
    );

#define gst_rtp_jpeg_depay_parent_class parent_class
G_DEFINE_TYPE (GstRtpJPEGDepay, gst_rtp_jpeg_depay,
    GST_TYPE_RTP_BASE_DEPAYLOAD);
GST_ELEMENT_REGISTER_DEFINE_WITH_CODE (rtpjpegdepay, "rtpjpegdepay",
    GST_RANK_SECONDARY, GST_TYPE_RTP_JPEG_DEPAY, rtp_element_init (plugin));

static void gst_rtp_jpeg_depay_finalize (GObject * object);

static GstStateChangeReturn gst_rtp_jpeg_depay_change_state (GstElement *
    element, GstStateChange transition);

static gboolean gst_rtp_jpeg_depay_setcaps (GstRTPBaseDepayload * depayload,
    GstCaps * caps);
static GstBuffer *gst_rtp_jpeg_depay_process (GstRTPBaseDepayload * depayload,
    GstRTPBuffer * rtp);

static void
gst_rtp_jpeg_depay_class_init (GstRtpJPEGDepayClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstRTPBaseDepayloadClass *gstrtpbasedepayload_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstrtpbasedepayload_class = (GstRTPBaseDepayloadClass *) klass;

  gobject_class->finalize = gst_rtp_jpeg_depay_finalize;

  gst_element_class_add_static_pad_template (gstelement_class,
      &gst_rtp_jpeg_depay_src_template);
  gst_element_class_add_static_pad_template (gstelement_class,
      &gst_rtp_jpeg_depay_sink_template);

  gst_element_class_set_static_metadata (gstelement_class,
      "RTP JPEG depayloader", "Codec/Depayloader/Network/RTP",
      "Extracts JPEG video from RTP packets (RFC 2435)",
      "Wim Taymans <wim.taymans@gmail.com>");

  gstelement_class->change_state = gst_rtp_jpeg_depay_change_state;

  gstrtpbasedepayload_class->set_caps = gst_rtp_jpeg_depay_setcaps;
  gstrtpbasedepayload_class->process_rtp_packet = gst_rtp_jpeg_depay_process;

  GST_DEBUG_CATEGORY_INIT (rtpjpegdepay_debug, "rtpjpegdepay", 0,
      "JPEG Video RTP Depayloader");
}

static void
gst_rtp_jpeg_depay_init (GstRtpJPEGDepay * rtpjpegdepay)
{
  gst_rtp_base_depayload_set_aggregate_hdrext_enabled (GST_RTP_BASE_DEPAYLOAD
      (rtpjpegdepay), TRUE);

  rtpjpegdepay->adapter = gst_adapter_new ();
}

static void
gst_rtp_jpeg_depay_reset (GstRtpJPEGDepay * depay)
{
  gint i;

  depay->width = 0;
  depay->height = 0;
  depay->media_width = 0;
  depay->media_height = 0;
  depay->frate_num = 0;
  depay->frate_denom = 1;
  depay->discont = TRUE;

  for (i = 0; i < 255; i++) {
    g_free (depay->qtables[i]);
    depay->qtables[i] = NULL;
  }

  gst_adapter_clear (depay->adapter);
}

static void
gst_rtp_jpeg_depay_finalize (GObject * object)
{
  GstRtpJPEGDepay *rtpjpegdepay;

  rtpjpegdepay = GST_RTP_JPEG_DEPAY (object);

  gst_rtp_jpeg_depay_reset (rtpjpegdepay);

  g_object_unref (rtpjpegdepay->adapter);
  rtpjpegdepay->adapter = NULL;

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static const int zigzag[] = {
  0, 1, 8, 16, 9, 2, 3, 10,
  17, 24, 32, 25, 18, 11, 4, 5,
  12, 19, 26, 33, 40, 48, 41, 34,
  27, 20, 13, 6, 7, 14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36,
  29, 22, 15, 23, 30, 37, 44, 51,
  58, 59, 52, 45, 38, 31, 39, 46,
  53, 60, 61, 54, 47, 55, 62, 63
};

/*
 * Table K.1 from JPEG spec.
 */
static const int jpeg_luma_quantizer[64] = {
  16, 11, 10, 16, 24, 40, 51, 61,
  12, 12, 14, 19, 26, 58, 60, 55,
  14, 13, 16, 24, 40, 57, 69, 56,
  14, 17, 22, 29, 51, 87, 80, 62,
  18, 22, 37, 56, 68, 109, 103, 77,
  24, 35, 55, 64, 81, 104, 113, 92,
  49, 64, 78, 87, 103, 121, 120, 101,
  72, 92, 95, 98, 112, 100, 103, 99
};

/*
 * Table K.2 from JPEG spec.
 */
static const int jpeg_chroma_quantizer[64] = {
  17, 18, 24, 47, 99, 99, 99, 99,
  18, 21, 26, 66, 99, 99, 99, 99,
  24, 26, 56, 99, 99, 99, 99, 99,
  47, 66, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99
};

/* Call MakeTables with the Q factor and a guint8[128] return array
 */
static void
MakeTables (GstRtpJPEGDepay * rtpjpegdepay, gint Q, guint8 qtable[128])
{
  gint i;
  guint factor;

  factor = CLAMP (Q, 1, 99);

  if (Q < 50)
    Q = 5000 / factor;
  else
    Q = 200 - factor * 2;

  for (i = 0; i < 64; i++) {
    gint lq = (jpeg_luma_quantizer[zigzag[i]] * Q + 50) / 100;
    gint cq = (jpeg_chroma_quantizer[zigzag[i]] * Q + 50) / 100;

    /* Limit the quantizers to 1 <= q <= 255 */
    qtable[i] = CLAMP (lq, 1, 255);
    qtable[i + 64] = CLAMP (cq, 1, 255);
  }
}

#include "gstrtpjpegdefaults.h"

static guint8 *
MakeQuantHeader (guint8 * p, guint8 * qt, gint size, gint tableNo)
{
  *p++ = 0xff;
  *p++ = 0xdb;                  /* DQT */
  *p++ = 0;                     /* length msb */
  *p++ = size + 3;              /* length lsb */
  *p++ = tableNo;
  memcpy (p, qt, size);

  return (p + size);
}

static guint8 *
MakeHuffmanHeader (guint8 * p, const guint8 * codelens, int ncodes,
    const guint8 * symbols, int nsymbols, int tableNo, int tableClass)
{
  *p++ = 0xff;
  *p++ = 0xc4;                  /* DHT */
  *p++ = 0;                     /* length msb */
  *p++ = 3 + ncodes + nsymbols; /* length lsb */
  *p++ = (tableClass << 4) | tableNo;
  memcpy (p, codelens, ncodes);
  p += ncodes;
  memcpy (p, symbols, nsymbols);
  p += nsymbols;

  return (p);
}

static guint8 *
MakeDRIHeader (guint8 * p, guint16 dri)
{
  *p++ = 0xff;
  *p++ = 0xdd;                  /* DRI */
  *p++ = 0x0;                   /* length msb */
  *p++ = 4;                     /* length lsb */
  *p++ = dri >> 8;              /* dri msb */
  *p++ = dri & 0xff;            /* dri lsb */

  return (p);
}

/*
 *  Arguments:
 *    type, width, height: as supplied in RTP/JPEG header
 *    qt: quantization tables as either derived from
 *        the Q field using MakeTables() or as specified
 *        in section 4.2.
 *    dri: restart interval in MCUs, or 0 if no restarts.
 *
 *    p: pointer to return area
 *
 *  Return value:
 *    The length of the generated headers.
 *
 *    Generate a frame and scan headers that can be prepended to the
 *    RTP/JPEG data payload to produce a JPEG compressed image in
 *    interchange format (except for possible trailing garbage and
 *    absence of an EOI marker to terminate the scan).
 */
static guint
MakeHeaders (guint8 * p, int type, int width, int height, guint8 * qt,
    guint precision, guint16 dri)
{
  guint8 *start = p;
  gint size;

  *p++ = 0xff;
  *p++ = 0xd8;                  /* SOI */

  size = ((precision & 1) ? 128 : 64);
  p = MakeQuantHeader (p, qt, size, 0);
  qt += size;

  size = ((precision & 2) ? 128 : 64);
  p = MakeQuantHeader (p, qt, size, 1);
  qt += size;

  if (dri != 0)
    p = MakeDRIHeader (p, dri);

  *p++ = 0xff;
  *p++ = 0xc0;                  /* SOF */
  *p++ = 0;                     /* length msb */
  *p++ = 17;                    /* length lsb */
  *p++ = 8;                     /* 8-bit precision */
  *p++ = height >> 8;           /* height msb */
  *p++ = height;                /* height lsb */
  *p++ = width >> 8;            /* width msb */
  *p++ = width;                 /* width lsb */
  *p++ = 3;                     /* number of components */
  *p++ = 0;                     /* comp 0 */
  if ((type & 0x3f) == 0)
    *p++ = 0x21;                /* hsamp = 2, vsamp = 1 */
  else
    *p++ = 0x22;                /* hsamp = 2, vsamp = 2 */
  *p++ = 0;                     /* quant table 0 */
  *p++ = 1;                     /* comp 1 */
  *p++ = 0x11;                  /* hsamp = 1, vsamp = 1 */
  *p++ = 1;                     /* quant table 1 */
  *p++ = 2;                     /* comp 2 */
  *p++ = 0x11;                  /* hsamp = 1, vsamp = 1 */
  *p++ = 1;                     /* quant table 1 */

  p = MakeHuffmanHeader (p, lum_dc_codelens,
      sizeof (lum_dc_codelens), lum_dc_symbols, sizeof (lum_dc_symbols), 0, 0);
  p = MakeHuffmanHeader (p, lum_ac_codelens,
      sizeof (lum_ac_codelens), lum_ac_symbols, sizeof (lum_ac_symbols), 0, 1);
  p = MakeHuffmanHeader (p, chm_dc_codelens,
      sizeof (chm_dc_codelens), chm_dc_symbols, sizeof (chm_dc_symbols), 1, 0);
  p = MakeHuffmanHeader (p, chm_ac_codelens,
      sizeof (chm_ac_codelens), chm_ac_symbols, sizeof (chm_ac_symbols), 1, 1);

  *p++ = 0xff;
  *p++ = 0xda;                  /* SOS */
  *p++ = 0;                     /* length msb */
  *p++ = 12;                    /* length lsb */
  *p++ = 3;                     /* 3 components */
  *p++ = 0;                     /* comp 0 */
  *p++ = 0;                     /* huffman table 0 */
  *p++ = 1;                     /* comp 1 */
  *p++ = 0x11;                  /* huffman table 1 */
  *p++ = 2;                     /* comp 2 */
  *p++ = 0x11;                  /* huffman table 1 */
  *p++ = 0;                     /* first DCT coeff */
  *p++ = 63;                    /* last DCT coeff */
  *p++ = 0;                     /* successive approx. */

  return (p - start);
};

static gboolean
gst_rtp_jpeg_depay_setcaps (GstRTPBaseDepayload * depayload, GstCaps * caps)
{
  GstRtpJPEGDepay *rtpjpegdepay;
  GstStructure *structure;
  gint clock_rate;
  const gchar *media_attr;

  rtpjpegdepay = GST_RTP_JPEG_DEPAY (depayload);

  structure = gst_caps_get_structure (caps, 0);
  GST_DEBUG_OBJECT (rtpjpegdepay, "Caps set: %" GST_PTR_FORMAT, caps);

  if (!gst_structure_get_int (structure, "clock-rate", &clock_rate))
    clock_rate = 90000;
  depayload->clock_rate = clock_rate;

  /* reset defaults */
  rtpjpegdepay->width = 0;
  rtpjpegdepay->height = 0;
  rtpjpegdepay->media_width = 0;
  rtpjpegdepay->media_height = 0;
  rtpjpegdepay->frate_num = 0;
  rtpjpegdepay->frate_denom = 1;

  /* check for optional SDP attributes */
  if ((media_attr = gst_structure_get_string (structure, "x-dimensions"))) {
    gint w, h;

    if (sscanf (media_attr, "%d,%d", &w, &h) == 2) {
      rtpjpegdepay->media_width = w;
      rtpjpegdepay->media_height = h;
    }
  }

  /* try to get a framerate */
  media_attr = gst_structure_get_string (structure, "a-framerate");
  if (!media_attr)
    media_attr = gst_structure_get_string (structure, "x-framerate");

  if (media_attr) {
    GValue src = { 0 };
    GValue dest = { 0 };
    gchar *s;

    /* canonicalise floating point string so we can handle framerate strings
     * in the form "24.930" or "24,930" irrespective of the current locale */
    s = g_strdup (media_attr);
    g_strdelimit (s, ",", '.');

    /* convert the float to a fraction */
    g_value_init (&src, G_TYPE_DOUBLE);
    g_value_set_double (&src, g_ascii_strtod (s, NULL));
    g_value_init (&dest, GST_TYPE_FRACTION);
    g_value_transform (&src, &dest);

    rtpjpegdepay->frate_num = gst_value_get_fraction_numerator (&dest);
    rtpjpegdepay->frate_denom = gst_value_get_fraction_denominator (&dest);

    g_free (s);
  }

  return TRUE;
}

static GstBuffer *
gst_rtp_jpeg_depay_process (GstRTPBaseDepayload * depayload, GstRTPBuffer * rtp)
{
  GstRtpJPEGDepay *rtpjpegdepay;
  GstBuffer *outbuf;
  gint payload_len, header_len;
  guint8 *payload;
  guint frag_offset;
  gint Q;
  guint type, width, height;
  guint16 dri, precision, length;
  guint8 *qtable = NULL;
  gsize qtable_len = 0;

  rtpjpegdepay = GST_RTP_JPEG_DEPAY (depayload);

  if (GST_BUFFER_IS_DISCONT (rtp->buffer)) {
    GST_DEBUG_OBJECT (depayload, "DISCONT, reset adapter");
    gst_adapter_clear (rtpjpegdepay->adapter);
    rtpjpegdepay->discont = TRUE;
  }

  /* Bound the native incomplete access unit, including NAL/JPEG header growth.
   * An outer appsrc limit cannot bound fragments retained by the depayloader. */
  if (gst_adapter_available (rtpjpegdepay->adapter)
      + 2 * (gsize) gst_rtp_buffer_get_payload_len (rtp) + 1024 > 16 * 1024 * 1024) {
    GST_ELEMENT_ERROR (rtpjpegdepay, RESOURCE, NO_SPACE_LEFT,
        ("RTP access unit exceeds native materialization capacity"), (NULL));
    gst_adapter_clear (rtpjpegdepay->adapter);
    return NULL;
  }

  payload_len = gst_rtp_buffer_get_payload_len (rtp);

  if (payload_len < 8)
    goto empty_packet;

  payload = gst_rtp_buffer_get_payload (rtp);
  header_len = 0;

  /*  0                   1                   2                   3
   *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   * | Type-specific |              Fragment Offset                  |
   * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   * |      Type     |       Q       |     Width     |     Height    |
   * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   */
  frag_offset = (payload[1] << 16) | (payload[2] << 8) | payload[3];
  type = payload[4];
  Q = payload[5];
  width = payload[6] * 8;
  height = payload[7] * 8;

  /* saw a packet with fragment offset > 0 and we don't already have data queued
   * up (most importantly, we don't have a header for this data) -- drop it
   * XXX: maybe we can check if the jpeg is progressive and salvage the data?
   * XXX: not implemented yet because jpegenc can't create progressive jpegs */
  if (frag_offset > 0 && gst_adapter_available (rtpjpegdepay->adapter) == 0)
    goto no_header_packet;

  /* allow frame dimensions > 2040, passed in SDP session or media attributes
   * from gstrtspsrc.c (gst_rtspsrc_sdp_attributes_to_caps), or in caps */
  if (!width)
    width = rtpjpegdepay->media_width;

  if (!height)
    height = rtpjpegdepay->media_height;

  if (width == 0 || height == 0)
    goto invalid_dimension;

  GST_DEBUG_OBJECT (rtpjpegdepay, "frag %u, type %u, Q %d, width %u, height %u",
      frag_offset, type, Q, width, height);

  header_len += 8;
  payload += 8;
  payload_len -= 8;

  dri = 0;
  if (type > 63) {
    if (payload_len < 4)
      goto empty_packet;

    /*  0                   1                   2                   3
     *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * |       Restart Interval        |F|L|       Restart Count       |
     * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     */
    dri = (payload[0] << 8) | payload[1];

    GST_DEBUG_OBJECT (rtpjpegdepay, "DRI %" G_GUINT16_FORMAT, dri);

    payload += 4;
    header_len += 4;
    payload_len -= 4;
  }

  if (Q >= 128 && frag_offset == 0) {
    if (payload_len < 4)
      goto empty_packet;

    /*  0                   1                   2                   3
     *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * |      MBZ      |   Precision   |             Length            |
     * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * |                    Quantization Table Data                    |
     * |                              ...                              |
     * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     */
    precision = payload[1];
    length = (payload[2] << 8) | payload[3];

    GST_DEBUG_OBJECT (rtpjpegdepay, "precision %04x, length %" G_GUINT16_FORMAT,
        precision, length);

    if (Q == 255 && length == 0)
      goto empty_packet;

    payload += 4;
    header_len += 4;
    payload_len -= 4;

    if (length > payload_len)
      goto empty_packet;

    if (length > 0) {
      qtable = payload;
      qtable_len = length;
    } else {
      qtable = rtpjpegdepay->qtables[Q];
      qtable_len = 128;
    }

    payload += length;
    header_len += length;
    payload_len -= length;
  } else {
    length = 0;
    qtable = NULL;
    precision = 0;
  }

  if (frag_offset == 0) {
    GstMapInfo map;
    guint size;

    if (rtpjpegdepay->width != width || rtpjpegdepay->height != height) {
      GstCaps *outcaps;

      outcaps =
          gst_caps_new_simple ("image/jpeg", "parsed", G_TYPE_BOOLEAN, TRUE,
          "framerate", GST_TYPE_FRACTION, rtpjpegdepay->frate_num,
          rtpjpegdepay->frate_denom, "width", G_TYPE_INT, width,
          "height", G_TYPE_INT, height, NULL);
      gst_pad_set_caps (depayload->srcpad, outcaps);
      gst_caps_unref (outcaps);

      rtpjpegdepay->width = width;
      rtpjpegdepay->height = height;
    }

    GST_LOG_OBJECT (rtpjpegdepay, "first packet, length %" G_GUINT16_FORMAT,
        length);

    /* first packet */
    if (length == 0) {
      if (Q < 128) {
        /* no quant table, see if we have one cached */
        qtable = rtpjpegdepay->qtables[Q];
        qtable_len = 128;
        if (!qtable) {
          GST_DEBUG_OBJECT (rtpjpegdepay, "making Q %d table", Q);
          /* make and cache the table */
          qtable = g_new (guint8, 128);
          MakeTables (rtpjpegdepay, Q, qtable);
          rtpjpegdepay->qtables[Q] = qtable;
        } else {
          GST_DEBUG_OBJECT (rtpjpegdepay, "using cached table for Q %d", Q);
        }
        /* all 8 bit quantizers */
        precision = 0;
      } else {
        if (!qtable)
          goto no_qtable;
      }
    }

    /* I think we can get here with a NULL qtable, so make sure we don't
       go dereferencing it in MakeHeaders if we do */
    if (!qtable)
      goto no_qtable;

    if (((precision & 1) ? 128 : 64) + ((precision & 2) ? 128 : 64) !=
        qtable_len)
      goto wrong_qtable;

    /* max header length, should be big enough */
    outbuf = gst_buffer_new_and_alloc (1000);
    gst_buffer_map (outbuf, &map, GST_MAP_WRITE);
    size = MakeHeaders (map.data, type, width, height, qtable, precision, dri);
    gst_buffer_unmap (outbuf, &map);
    gst_buffer_resize (outbuf, 0, size);

    GST_DEBUG_OBJECT (rtpjpegdepay, "pushing %u bytes of header", size);

    gst_adapter_push (rtpjpegdepay->adapter, outbuf);
  }

  /* take JPEG data, push in the adapter */
  GST_DEBUG_OBJECT (rtpjpegdepay, "pushing data at offset %d", header_len);
  outbuf = gst_rtp_buffer_get_payload_subbuffer (rtp, header_len, -1);
  gst_adapter_push (rtpjpegdepay->adapter, outbuf);
  outbuf = NULL;

  if (gst_rtp_buffer_get_marker (rtp)) {
    guint avail;
    guint8 end[2];
    GstMapInfo map;

    /* last buffer take all data out of the adapter */
    avail = gst_adapter_available (rtpjpegdepay->adapter);
    GST_DEBUG_OBJECT (rtpjpegdepay, "marker set, last buffer");

    if (avail < 2)
      goto invalid_packet;

    /* take the last bytes of the jpeg data to see if there is an EOI
     * marker */
    gst_adapter_copy (rtpjpegdepay->adapter, end, avail - 2, 2);

    if (GST_READ_UINT16_BE (end) != 0xffd9) {
      GST_DEBUG_OBJECT (rtpjpegdepay, "no EOI marker, adding one");

      /* no EOI marker, add one */
      outbuf = gst_buffer_new_and_alloc (2);
      gst_buffer_map (outbuf, &map, GST_MAP_WRITE);
      map.data[0] = 0xff;
      map.data[1] = 0xd9;
      gst_buffer_unmap (outbuf, &map);

      gst_adapter_push (rtpjpegdepay->adapter, outbuf);
      avail += 2;
    }
    outbuf = gst_adapter_take_buffer (rtpjpegdepay->adapter, avail);

    if (rtpjpegdepay->discont) {
      GST_BUFFER_FLAG_SET (outbuf, GST_BUFFER_FLAG_DISCONT);
      rtpjpegdepay->discont = FALSE;
    }

    gst_rtp_drop_non_video_meta (rtpjpegdepay, outbuf);

    GST_DEBUG_OBJECT (rtpjpegdepay, "returning %u bytes", avail);
  }

  return outbuf;

  /* ERRORS */
empty_packet:
  {
    GST_ELEMENT_WARNING (rtpjpegdepay, STREAM, DECODE,
        ("Empty Payload."), (NULL));
    gst_rtp_base_depayload_dropped (depayload);
    return NULL;
  }
invalid_dimension:
  {
    GST_ELEMENT_WARNING (rtpjpegdepay, STREAM, FORMAT,
        ("Invalid Dimension %dx%d.", width, height), (NULL));
    gst_rtp_base_depayload_dropped (depayload);
    return NULL;
  }
no_qtable:
  {
    GST_WARNING_OBJECT (rtpjpegdepay, "no qtable");
    gst_rtp_base_depayload_dropped (depayload);
    return NULL;
  }
wrong_qtable:
  {
    GST_WARNING_OBJECT (rtpjpegdepay, "wrong qtable for precision");
    gst_rtp_base_depayload_dropped (depayload);
    return NULL;
  }
invalid_packet:
  {
    GST_WARNING_OBJECT (rtpjpegdepay, "invalid packet");
    gst_adapter_flush (rtpjpegdepay->adapter,
        gst_adapter_available (rtpjpegdepay->adapter));
    gst_rtp_base_depayload_flush (depayload, TRUE);
    return NULL;
  }
no_header_packet:
  {
    GST_WARNING_OBJECT (rtpjpegdepay,
        "discarding data packets received when we have no header");
    gst_rtp_base_depayload_dropped (depayload);
    return NULL;
  }
}


static GstStateChangeReturn
gst_rtp_jpeg_depay_change_state (GstElement * element,
    GstStateChange transition)
{
  GstRtpJPEGDepay *rtpjpegdepay;
  GstStateChangeReturn ret;

  rtpjpegdepay = GST_RTP_JPEG_DEPAY (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_rtp_jpeg_depay_reset (rtpjpegdepay);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    default:
      break;
  }
  return ret;
}
