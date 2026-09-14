/* GStreamer
 * Copyright (C) <2005,2006,2007> Wim Taymans <wim@fluendo.com>
 *               <2007> Peter Kjellerstedt  <pkj at axis com>
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
/*
 * Unless otherwise indicated, Source Code is licensed under MIT license.
 * See further explanation attached in License Statement (distributed in the file
 * LICENSE).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * SECTION:gstrtsptransport
 * @title: GstRTSPRange
 * @short_description: dealing with RTSP transports
 *
 * Provides helper functions to deal with RTSP transport strings.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>

#include "gstrtsptransport.h"
#include <gio/gio.h>
#include "gstrtsp-enumtypes.h"

#define MAX_MANAGERS	2

typedef enum
{
  RTSP_TRANSPORT_DELIVERY = 1 << 0,     /* multicast | unicast */
  RTSP_TRANSPORT_DESTINATION = 1 << 1,
  RTSP_TRANSPORT_SOURCE = 1 << 2,
  RTSP_TRANSPORT_INTERLEAVED = 1 << 3,
  RTSP_TRANSPORT_APPEND = 1 << 4,
  RTSP_TRANSPORT_TTL = 1 << 5,
  RTSP_TRANSPORT_LAYERS = 1 << 6,
  RTSP_TRANSPORT_PORT = 1 << 7,
  RTSP_TRANSPORT_CLIENT_PORT = 1 << 8,
  RTSP_TRANSPORT_SERVER_PORT = 1 << 9,
  RTSP_TRANSPORT_SSRC = 1 << 10,
  RTSP_TRANSPORT_MODE = 1 << 11,
  RTSP_TRANSPORT_DEST_ADDR = 1 << 12,
  RTSP_TRANSPORT_SRC_ADDR = 1 << 13,
  RTSP_TRANSPORT_RTCP_MUX = 1 << 14,
} RTSPTransportParameter;

typedef struct
{
  const gchar *name;
  const GstRTSPTransMode mode;
  const GstRTSPProfile profile;
  const GstRTSPLowerTrans ltrans;
  const gchar *media_type;
  const gchar *manager[MAX_MANAGERS];
} GstRTSPTransMap;

static const GstRTSPTransMap transports[] = {
  {"rtp", GST_RTSP_TRANS_RTP, GST_RTSP_PROFILE_AVP,
        GST_RTSP_LOWER_TRANS_UDP_MCAST, "application/x-rtp",
      {"rtpbin", "rtpdec"}},
  {"srtp", GST_RTSP_TRANS_RTP, GST_RTSP_PROFILE_SAVP,
        GST_RTSP_LOWER_TRANS_UDP_MCAST, "application/x-srtp",
      {"rtpbin", "rtpdec"}},
  {"rtpf", GST_RTSP_TRANS_RTP, GST_RTSP_PROFILE_AVPF,
        GST_RTSP_LOWER_TRANS_UDP_MCAST, "application/x-rtp",
      {"rtpbin", "rtpdec"}},
  {"srtpf", GST_RTSP_TRANS_RTP, GST_RTSP_PROFILE_SAVPF,
        GST_RTSP_LOWER_TRANS_UDP_MCAST, "application/x-srtp",
      {"rtpbin", "rtpdec"}},
  /* FIXME: these two are no longer supported (the rdtmanager element and
   * related code has been removed from the realmedia plugin), but let's keep
   * this around for now so it can still be added back via plugins if needed.
   * FIXME 2.0: remove */
  {"x-real-rdt", GST_RTSP_TRANS_RDT, GST_RTSP_PROFILE_AVP,
        GST_RTSP_LOWER_TRANS_UNKNOWN, "application/x-rdt",
      {"rdtmanager", NULL}},
  {"x-pn-tng", GST_RTSP_TRANS_RDT, GST_RTSP_PROFILE_AVP,
        GST_RTSP_LOWER_TRANS_UNKNOWN, "application/x-rdt",
      {"rdtmanager", NULL}},
  {NULL, GST_RTSP_TRANS_UNKNOWN, GST_RTSP_PROFILE_UNKNOWN,
      GST_RTSP_LOWER_TRANS_UNKNOWN, NULL, {NULL, NULL}}
};

typedef struct
{
  const gchar *name;
  const GstRTSPProfile profile;
} RTSPProfileMap;

static const RTSPProfileMap profiles[] = {
  {"avp", GST_RTSP_PROFILE_AVP},
  {"savp", GST_RTSP_PROFILE_SAVP},
  {"avpf", GST_RTSP_PROFILE_AVPF},
  {"savpf", GST_RTSP_PROFILE_SAVPF},
  {NULL, GST_RTSP_PROFILE_UNKNOWN}
};

typedef struct
{
  const gchar *name;
  const GstRTSPLowerTrans ltrans;
} RTSPLTransMap;

static const RTSPLTransMap ltrans[] = {
  {"udp", GST_RTSP_LOWER_TRANS_UDP},
  {"mcast", GST_RTSP_LOWER_TRANS_UDP_MCAST},
  {"tcp", GST_RTSP_LOWER_TRANS_TCP},
  {NULL, GST_RTSP_LOWER_TRANS_UNKNOWN}
};

#define RTSP_TRANSPORT_PARAMETER_IS_UNIQUE(param) \
G_STMT_START {                                    \
  if ((transport_params & (param)) != 0)          \
    goto invalid_transport;                       \
  transport_params |= (param);                    \
} G_STMT_END

/**
 * gst_rtsp_transport_new:
 * @transport: (out) (transfer full): location to hold the new #GstRTSPTransport
 *
 * Allocate a new initialized #GstRTSPTransport. Use gst_rtsp_transport_free()
 * after usage.
 *
 * Returns: a #GstRTSPResult.
 */
GstRTSPResult
gst_rtsp_transport_new (GstRTSPTransport ** transport)
{
  GstRTSPTransport *trans;

  g_return_val_if_fail (transport != NULL, GST_RTSP_EINVAL);

  trans = g_new0 (GstRTSPTransport, 1);

  *transport = trans;

  return gst_rtsp_transport_init (trans);
}

/**
 * gst_rtsp_transport_init:
 * @transport: (out caller-allocates): a #GstRTSPTransport
 *
 * Initialize @transport so that it can be used.
 *
 * Returns: #GST_RTSP_OK.
 */
GstRTSPResult
gst_rtsp_transport_init (GstRTSPTransport * transport)
{
  g_return_val_if_fail (transport != NULL, GST_RTSP_EINVAL);

  g_free (transport->destination);
  g_free (transport->source);
  for (guint i = 0; i < 2; i++) {
    g_free (transport->dest_addr[i].host);
    g_free (transport->src_addr[i].host);
  }
  g_clear_pointer (&transport->ssrcs, g_array_unref);

  memset (transport, 0, sizeof (GstRTSPTransport));

  transport->trans = GST_RTSP_TRANS_RTP;
  transport->profile = GST_RTSP_PROFILE_AVP;
  transport->lower_transport = GST_RTSP_LOWER_TRANS_UDP_MCAST;
  transport->mode_play = TRUE;
  transport->mode_record = FALSE;
  transport->interleaved.min = -1;
  transport->interleaved.max = -1;
  transport->port.min = -1;
  transport->port.max = -1;
  transport->client_port.min = -1;
  transport->client_port.max = -1;
  transport->server_port.min = -1;
  transport->server_port.max = -1;

  return GST_RTSP_OK;
}

#ifndef GST_REMOVE_DEPRECATED
/**
 * gst_rtsp_transport_get_mime:
 * @trans: a #GstRTSPTransMode
 * @mime: (out) (transfer none): location to hold the result
 *
 * Get the mime type of the transport mode @trans. This mime type is typically
 * used to generate #GstCaps events.
 *
 * Deprecated: This functions only deals with the GstRTSPTransMode and only
 *    returns the mime type for #GST_RTSP_PROFILE_AVP. Use
 *    gst_rtsp_transport_get_media_type() instead.
 *
 * Returns: #GST_RTSP_OK.
 */
GstRTSPResult
gst_rtsp_transport_get_mime (GstRTSPTransMode trans, const gchar ** mime)
{
  gint i;

  g_return_val_if_fail (mime != NULL, GST_RTSP_EINVAL);

  for (i = 0; transports[i].name; i++)
    if (transports[i].mode == trans
        && transports[i].profile == GST_RTSP_PROFILE_AVP)
      break;
  *mime = transports[i].media_type;

  return GST_RTSP_OK;
}
#endif

/**
 * gst_rtsp_transport_get_media_type:
 * @transport: a #GstRTSPTransport
 * @media_type: (out) (transfer none): media type of @transport
 *
 * Get the media type of @transport. This media type is typically
 * used to generate #GstCaps events.
 *
 * Since: 1.4
 *
 * Returns: #GST_RTSP_OK.
 */
GstRTSPResult
gst_rtsp_transport_get_media_type (GstRTSPTransport * transport,
    const gchar ** media_type)
{
  gint i;

  g_return_val_if_fail (transport != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (media_type != NULL, GST_RTSP_EINVAL);

  for (i = 0; transports[i].name; i++)
    if (transports[i].mode == transport->trans
        && transports[i].profile == transport->profile)
      break;
  *media_type = transports[i].media_type;

  return GST_RTSP_OK;
}

static GstRTSPLowerTrans
get_default_lower_trans (GstRTSPTransport * transport)
{
  gint i;

  for (i = 0; transports[i].name; i++)
    if (transports[i].mode == transport->trans
        && transports[i].profile == transport->profile)
      break;

  return transports[i].ltrans;
}

/**
 * gst_rtsp_transport_get_manager:
 * @trans: a #GstRTSPTransMode
 * @manager: (out) (nullable) (transfer none): location to hold the result
 * @option: option index.
 *
 * Get the #GstElement that can handle the buffers transported over @trans.
 *
 * It is possible that there are several managers available, use @option to
 * selected one.
 *
 * @manager will contain an element name or %NULL when no manager is
 * needed/available for @trans.
 *
 * Returns: #GST_RTSP_OK.
 */
GstRTSPResult
gst_rtsp_transport_get_manager (GstRTSPTransMode trans, const gchar ** manager,
    guint option)
{
  gint i;

  g_return_val_if_fail (manager != NULL, GST_RTSP_EINVAL);

  for (i = 0; transports[i].name; i++)
    if (transports[i].mode == trans)
      break;

  if (option < MAX_MANAGERS)
    *manager = transports[i].manager[option];
  else
    *manager = NULL;

  return GST_RTSP_OK;
}

static void
parse_mode (GstRTSPTransport * transport, const gchar * str)
{
  transport->mode_play = FALSE;
  transport->mode_record = FALSE;
  gchar *value = g_strdup (str);
  gsize length = strlen (value);
  if (length >= 2 && value[0] == '"' && value[length - 1] == '"') {
    value[length - 1] = 0;
    memmove (value, value + 1, length - 1);
  }
  gchar **modes = g_strsplit (value, ",", 0);
  for (guint i = 0; modes[i]; i++) {
    g_strstrip (modes[i]);
    if (!strcmp (modes[i], "play")) transport->mode_play = TRUE;
    else if (!strcmp (modes[i], "record")) transport->mode_record = TRUE;
    else { transport->mode_play = transport->mode_record = FALSE; break; }
  }
  g_strfreev (modes);
  g_free (value);
}

static gboolean
check_range (const gchar * str, gchar ** tmp, gint * range)
{
  glong range_val;

  range_val = strtol (str, tmp, 10);
  if (range_val >= G_MININT && range_val <= G_MAXINT) {
    *range = range_val;
    return TRUE;
  } else {
    return FALSE;
  }
}

static gboolean
parse_range (const gchar * str, GstRTSPRange * range)
{
  const gchar *minus;
  gchar *tmp;

  /* even though strtol() allows white space, plus and minus in front of
   * the number, we do not allow it
   */
  if (g_ascii_isspace (*str) || *str == '+' || *str == '-')
    goto invalid_range;

  minus = strstr (str, "-");
  if (minus) {
    if (g_ascii_isspace (minus[1]) || minus[1] == '+' || minus[1] == '-')
      goto invalid_range;

    if (!check_range (str, &tmp, &range->min) || str == tmp || tmp != minus)
      goto invalid_range;

    if (!check_range (minus + 1, &tmp, &range->max) || tmp == minus + 1 || *tmp)
      goto invalid_range;
  } else {
    if (!check_range (str, &tmp, &range->min) || str == tmp ||
        *tmp)
      goto invalid_range;

    range->max = -1;
  }

  return TRUE;

invalid_range:
  {
    range->min = -1;
    range->max = -1;
    return FALSE;
  }
}

static gchar *
range_as_text (const GstRTSPRange * range)
{
  if (range->min < 0)
    return NULL;
  else if (range->max < 0)
    return g_strdup_printf ("%d", range->min);
  else
    return g_strdup_printf ("%d-%d", range->min, range->max);
}

static const gchar *
rtsp_transport_mode_as_text (const GstRTSPTransport * transport)
{
  gint i;

  for (i = 0; transports[i].name; i++)
    if (transports[i].mode == transport->trans)
      return transports[i].name;

  return NULL;
}

static const gchar *
rtsp_transport_profile_as_text (const GstRTSPTransport * transport)
{
  gint i;

  for (i = 0; profiles[i].name; i++)
    if (profiles[i].profile == transport->profile)
      return profiles[i].name;

  return NULL;
}

static const gchar *
rtsp_transport_ltrans_as_text (const GstRTSPTransport * transport)
{
  gint i;

  /* need to special case GST_RTSP_LOWER_TRANS_UDP_MCAST */
  if (transport->lower_transport == GST_RTSP_LOWER_TRANS_UDP_MCAST)
    return "udp";

  for (i = 0; ltrans[i].name; i++)
    if (ltrans[i].ltrans == transport->lower_transport)
      return ltrans[i].name;

  return NULL;
}

#define IS_VALID_PORT_RANGE(range) \
    (range.min >= 0 && range.min < 65536 && range.max < 65536 && (range.max == -1 || range.max >= range.min))

#define IS_VALID_INTERLEAVE_RANGE(range) \
    (range.min >= 0 && range.min < 256 && range.max < 256 && (range.max == -1 || range.max >= range.min))

/* RFC 7826 Appendix C.1.2 permits one RTP and an optional RTCP tuple.
 * Parsing never resolves a hostname or opens a socket. */
static gboolean
parse_addresses (const gchar *text, GstRTSPTransportAddress addresses[2], guint *count)
{
  const gchar *cursor = text;
  while (*cursor) {
    if (*count == 2 || *cursor++ != '"') return FALSE;
    const gchar *end = strchr (cursor, '"');
    if (!end) return FALSE;
    gchar *tuple = g_strndup (cursor, end - cursor);
    gchar *port, *host = tuple;
    if (*host == '[') {
      gchar *close = strchr (host, ']');
      if (!close || close[1] != ':') { g_free (tuple); return FALSE; }
      *close = 0; host++;
      GInetAddress *ip = g_inet_address_new_from_string (host);
      if (!ip || g_inet_address_get_family (ip) != G_SOCKET_FAMILY_IPV6) {
        g_clear_object (&ip); g_free (tuple); return FALSE;
      }
      g_object_unref (ip); port = close + 2;
    } else {
      port = strchr (host, ':');
      if (!port) { g_free (tuple); return FALSE; }
      *port++ = 0;
      for (gchar *c = host; *c; c++)
        if (!g_ascii_isalnum (*c) && *c != '-' && *c != '.' && *c != '_') {
          g_free (tuple); return FALSE;
        }
    }
    guint64 number;
    if (!g_ascii_string_to_unsigned (port, 10, 0, 65535, &number, NULL)) {
      g_free (tuple); return FALSE;
    }
    addresses[*count].host = g_strdup (host);
    addresses[(*count)++].port = number;
    g_free (tuple);
    cursor = end + 1;
    if (*cursor && *cursor++ != '/') return FALSE;
    if (!*cursor && end[1] == '/') return FALSE;
  }
  return *count != 0;
}

/**
 * gst_rtsp_transport_parse:
 * @str: a transport string
 * @transport: (out caller-allocates): a #GstRTSPTransport
 *
 * Parse the RTSP transport string @str into @transport.
 *
 * Returns: a #GstRTSPResult.
 */
GstRTSPResult
gst_rtsp_transport_parse (const gchar * str, GstRTSPTransport * transport)
{
  gchar **split, *down, **transp = NULL;
  guint transport_params = 0;
  gint i, count;

  g_return_val_if_fail (transport != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (str != NULL, GST_RTSP_EINVAL);

  gst_rtsp_transport_init (transport);

  /* case insensitive */
  down = g_ascii_strdown (str, -1);

  split = g_strsplit (down, ";", 0);
  for (guint part = 0; split[part]; part++) g_strstrip (split[part]);
  g_free (down);

  /* First field contains the transport/profile/lower_transport */
  if (split[0] == NULL)
    goto invalid_transport;

  transp = g_strsplit (split[0], "/", 0);

  if (transp[0] == NULL || transp[1] == NULL)
    goto invalid_transport;

  for (i = 0; transports[i].name; i++)
    if (strcmp (transp[0], transports[i].name) == 0)
      break;
  transport->trans = transports[i].mode;

  if (transport->trans != GST_RTSP_TRANS_RDT) {
    for (i = 0; profiles[i].name; i++)
      if (strcmp (transp[1], profiles[i].name) == 0)
        break;
    transport->profile = profiles[i].profile;
    count = 2;
  } else {
    /* RDT has transport/lower_transport */
    transport->profile = GST_RTSP_PROFILE_AVP;
    count = 1;
  }

  if (transp[count] != NULL) {
    if (transp[count + 1] != NULL) goto invalid_transport;
    for (i = 0; ltrans[i].name; i++)
      if (strcmp (transp[count], ltrans[i].name) == 0)
        break;
    transport->lower_transport = ltrans[i].ltrans;
  } else {
    /* specifying the lower transport is optional */
    transport->lower_transport = get_default_lower_trans (transport);
  }

  g_strfreev (transp);
  transp = NULL;

  if (transport->trans == GST_RTSP_TRANS_UNKNOWN ||
      transport->profile == GST_RTSP_PROFILE_UNKNOWN ||
      transport->lower_transport == GST_RTSP_LOWER_TRANS_UNKNOWN)
    goto unsupported_transport;

  i = 1;
  while (split[i]) {
    if (strcmp (split[i], "multicast") == 0) {
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_DELIVERY);
      if (transport->lower_transport == GST_RTSP_LOWER_TRANS_TCP)
        goto invalid_transport;
      transport->lower_transport = GST_RTSP_LOWER_TRANS_UDP_MCAST;
    } else if (strcmp (split[i], "unicast") == 0) {
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_DELIVERY);
      if (transport->lower_transport == GST_RTSP_LOWER_TRANS_UDP_MCAST)
        transport->lower_transport = GST_RTSP_LOWER_TRANS_UDP;
    } else if (g_str_has_prefix (split[i], "destination=")) {
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_DESTINATION);
      transport->destination = g_strdup (split[i] + 12);
    } else if (g_str_has_prefix (split[i], "source=")) {
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_SOURCE);
      transport->source = g_strdup (split[i] + 7);
    } else if (g_str_has_prefix (split[i], "layers=")) {
      guint64 layers;
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_LAYERS);
      if (!g_ascii_string_to_unsigned (split[i] + 7, 10, 0, G_MAXUINT, &layers,
              NULL))
        goto invalid_transport;
      transport->layers = layers;
    } else if (g_str_has_prefix (split[i], "mode=")) {
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_MODE);
      parse_mode (transport, split[i] + 5);
      if (!transport->mode_play && !transport->mode_record)
        goto invalid_transport;
    } else if (strcmp (split[i], "append") == 0) {
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_APPEND);
      transport->append = TRUE;
    } else if (g_str_has_prefix (split[i], "interleaved=")) {
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_INTERLEAVED);
      if (!parse_range (split[i] + 12, &transport->interleaved) ||
          !IS_VALID_INTERLEAVE_RANGE (transport->interleaved))
        goto invalid_transport;
    } else if (g_str_has_prefix (split[i], "ttl=")) {
      guint64 ttl;
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_TTL);
      if (!g_ascii_string_to_unsigned (split[i] + 4, 10, 0, 255, &ttl, NULL))
        goto invalid_transport;
      transport->ttl = ttl;
    } else if (g_str_has_prefix (split[i], "port=")) {
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_PORT);
      if (!parse_range (split[i] + 5, &transport->port) ||
          !IS_VALID_PORT_RANGE (transport->port))
        goto invalid_transport;
    } else if (g_str_has_prefix (split[i], "client_port=")) {
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_CLIENT_PORT);
      if (!parse_range (split[i] + 12, &transport->client_port) ||
          !IS_VALID_PORT_RANGE (transport->client_port))
        goto invalid_transport;
    } else if (g_str_has_prefix (split[i], "server_port=")) {
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_SERVER_PORT);
      if (!parse_range (split[i] + 12, &transport->server_port) ||
          !IS_VALID_PORT_RANGE (transport->server_port))
        goto invalid_transport;
    } else if (g_str_has_prefix (split[i], "dest_addr=")) {
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_DEST_ADDR);
      if (!parse_addresses (split[i] + 10, transport->dest_addr, &transport->dest_addr_count))
        goto invalid_transport;
    } else if (g_str_has_prefix (split[i], "src_addr=")) {
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_SRC_ADDR);
      if (!parse_addresses (split[i] + 9, transport->src_addr, &transport->src_addr_count))
        goto invalid_transport;
    } else if (!strcmp (split[i], "rtcp-mux")) {
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_RTCP_MUX);
      transport->rtcp_mux = TRUE;
    } else if (g_str_has_prefix (split[i], "ssrc=")) {
      guint64 ssrc;
      RTSP_TRANSPORT_PARAMETER_IS_UNIQUE (RTSP_TRANSPORT_SSRC);
      gchar **values = g_strsplit (split[i] + 5, "/", 0);
      transport->ssrcs = g_array_new (FALSE, FALSE, sizeof (guint32));
      for (guint v = 0; values[v]; v++) {
        if (!g_ascii_string_to_unsigned (values[v], 16, 0, G_MAXUINT, &ssrc, NULL)) {
          g_strfreev (values);
          goto invalid_transport;
        }
        if (strlen (values[v]) != 8) transport->runtime_ssrc_bad_width = TRUE;
        guint32 value = ssrc;
        g_array_append_val (transport->ssrcs, value);
      }
      g_strfreev (values);
      transport->ssrc = g_array_index (transport->ssrcs, guint32, 0);
    } else {
      transport->runtime_unknown_parameter = TRUE;

    }
    i++;
  }
  g_strfreev (split);
  transport->runtime_parameters = transport_params;

  return GST_RTSP_OK;

unsupported_transport:
  {
    g_strfreev (split);
    return GST_RTSP_ERROR;
  }
invalid_transport:
  {
    g_strfreev (transp);
    g_strfreev (split);
    return GST_RTSP_EINVAL;
  }
}

GstRTSPResult
gst_rtsp_transport_parse_version (const gchar *text, GstRTSPVersion version,
    GstRTSPTransport *transport)
{
  GstRTSPResult result = gst_rtsp_transport_parse (text, transport);
  if (result != GST_RTSP_OK) return result;
  if (transport->runtime_unknown_parameter || transport->trans != GST_RTSP_TRANS_RTP)
    return GST_RTSP_ENOTIMPL;
  if (version == GST_RTSP_VERSION_1_0) {
    if (transport->dest_addr_count || transport->src_addr_count || transport->rtcp_mux ||
        (transport->ssrcs && transport->ssrcs->len != 1)) return GST_RTSP_EPARSE;
  } else if (version == GST_RTSP_VERSION_2_0) {
    if (transport->runtime_ssrc_bad_width ||
        !(transport->runtime_parameters & RTSP_TRANSPORT_DELIVERY) ||
        transport->destination || transport->source || transport->append || transport->mode_record ||
        transport->client_port.min != -1 || transport->server_port.min != -1 || transport->port.min != -1)
      return GST_RTSP_EPARSE;
    if (transport->interleaved.min != -1 && (transport->dest_addr_count || transport->src_addr_count))
      return GST_RTSP_EPARSE;
  } else return GST_RTSP_EINVAL;
  if (transport->interleaved.min != -1 && transport->lower_transport != GST_RTSP_LOWER_TRANS_TCP)
    return GST_RTSP_EPARSE;
  return GST_RTSP_OK;
}

/**
 * gst_rtsp_transport_as_text:
 * @transport: a #GstRTSPTransport
 *
 * Convert @transport into a string that can be used to signal the transport in
 * an RTSP SETUP response.
 *
 * Returns: (transfer full) (nullable): a string describing the RTSP transport
 * or %NULL when the transport is invalid.
 */
gchar *
gst_rtsp_transport_as_text (GstRTSPTransport * transport)
{
  GPtrArray *strs;
  gchar *res;
  const gchar *tmp;

  g_return_val_if_fail (transport != NULL, NULL);

  strs = g_ptr_array_new ();

  /* add the transport specifier */
  if ((tmp = rtsp_transport_mode_as_text (transport)) == NULL)
    goto invalid_transport;
  g_ptr_array_add (strs, g_ascii_strup (tmp, -1));

  g_ptr_array_add (strs, g_strdup ("/"));

  if ((tmp = rtsp_transport_profile_as_text (transport)) == NULL)
    goto invalid_transport;
  g_ptr_array_add (strs, g_ascii_strup (tmp, -1));

  if (transport->trans != GST_RTSP_TRANS_RTP ||
      (transport->profile != GST_RTSP_PROFILE_AVP &&
          transport->profile != GST_RTSP_PROFILE_SAVP &&
          transport->profile != GST_RTSP_PROFILE_AVPF &&
          transport->profile != GST_RTSP_PROFILE_SAVPF) ||
      transport->lower_transport == GST_RTSP_LOWER_TRANS_TCP) {
    g_ptr_array_add (strs, g_strdup ("/"));

    if ((tmp = rtsp_transport_ltrans_as_text (transport)) == NULL)
      goto invalid_transport;

    g_ptr_array_add (strs, g_ascii_strup (tmp, -1));
  }

  /*
   * the order of the following parameters is the same as the one specified in
   * RFC 2326 to please some weird RTSP clients that require it
   */

  /* add the unicast/multicast parameter */
  if (transport->lower_transport == GST_RTSP_LOWER_TRANS_UDP_MCAST)
    g_ptr_array_add (strs, g_strdup (";multicast"));
  else
    g_ptr_array_add (strs, g_strdup (";unicast"));

  /* add the destination parameter */
  if (transport->destination != NULL) {
    g_ptr_array_add (strs, g_strdup (";destination="));
    g_ptr_array_add (strs, g_strdup (transport->destination));
  }

  /* add the source parameter */
  if (transport->source != NULL) {
    g_ptr_array_add (strs, g_strdup (";source="));
    g_ptr_array_add (strs, g_strdup (transport->source));
  }

  for (guint side = 0; side < 2; side++) {
    guint count = side ? transport->src_addr_count : transport->dest_addr_count;
    GstRTSPTransportAddress *addresses = side ? transport->src_addr : transport->dest_addr;
    if (count > 2) goto invalid_transport;
    for (guint i = 0; i < count; i++) {
      if (!addresses[i].host) goto invalid_transport;
      g_ptr_array_add (strs, g_strdup (i ? "/" : side ? ";src_addr=" : ";dest_addr="));
      g_ptr_array_add (strs, strchr (addresses[i].host, ':')
          ? g_strdup_printf ("\"[%s]:%u\"", addresses[i].host, addresses[i].port)
          : g_strdup_printf ("\"%s:%u\"", addresses[i].host, addresses[i].port));
    }
  }
  if (transport->rtcp_mux) g_ptr_array_add (strs, g_strdup (";RTCP-mux"));

  /* add the interleaved parameter */
  if (transport->lower_transport == GST_RTSP_LOWER_TRANS_TCP &&
      transport->interleaved.min >= 0) {
    if (transport->interleaved.min < 256 && transport->interleaved.max < 256) {
      g_ptr_array_add (strs, g_strdup (";interleaved="));
      g_ptr_array_add (strs, range_as_text (&transport->interleaved));
    } else
      goto invalid_transport;
  }

  /* add the append parameter */
  if (transport->mode_record && transport->append)
    g_ptr_array_add (strs, g_strdup (";append"));

  /* add the ttl parameter */
  if (transport->lower_transport == GST_RTSP_LOWER_TRANS_UDP_MCAST &&
      transport->ttl != 0) {
    if (transport->ttl < 256) {
      g_ptr_array_add (strs, g_strdup (";ttl="));
      g_ptr_array_add (strs, g_strdup_printf ("%u", transport->ttl));
    } else
      goto invalid_transport;
  }

  /* add the layers parameter */
  if (transport->layers != 0) {
    g_ptr_array_add (strs, g_strdup (";layers="));
    g_ptr_array_add (strs, g_strdup_printf ("%u", transport->layers));
  }

  /* add the port parameter */
  if (transport->lower_transport != GST_RTSP_LOWER_TRANS_TCP) {
    if (transport->trans == GST_RTSP_TRANS_RTP && transport->port.min >= 0) {
      if (transport->port.min < 65536 && transport->port.max < 65536) {
        g_ptr_array_add (strs, g_strdup (";port="));
        g_ptr_array_add (strs, range_as_text (&transport->port));
      } else
        goto invalid_transport;
    }

    /* add the client_port parameter */
    if (transport->trans == GST_RTSP_TRANS_RTP
        && transport->client_port.min >= 0) {
      if (transport->client_port.min < 65536
          && transport->client_port.max < 65536) {
        g_ptr_array_add (strs, g_strdup (";client_port="));
        if (transport->client_port.max > 0)
          g_ptr_array_add (strs, range_as_text (&transport->client_port));
        else
          g_ptr_array_add (strs, g_strdup_printf ("%d",
                  transport->client_port.min));
      } else
        goto invalid_transport;
    }

    /* add the server_port parameter */
    if (transport->trans == GST_RTSP_TRANS_RTP
        && transport->server_port.min >= 0) {
      if (transport->server_port.min < 65536
          && transport->server_port.max < 65536) {
        g_ptr_array_add (strs, g_strdup (";server_port="));
        if (transport->server_port.max > 0)
          g_ptr_array_add (strs, range_as_text (&transport->server_port));
        else
          g_ptr_array_add (strs, g_strdup_printf ("%d",
                  transport->server_port.min));
      } else
        goto invalid_transport;
    }
  }

  /* add the ssrc parameter */
  if (transport->ssrcs && transport->ssrcs->len) {
    for (guint i = 0; i < transport->ssrcs->len; i++) {
      g_ptr_array_add (strs, g_strdup (i ? "/" : ";ssrc="));
      g_ptr_array_add (strs, g_strdup_printf ("%08X", g_array_index (transport->ssrcs, guint32, i)));
    }
  } else if (transport->lower_transport != GST_RTSP_LOWER_TRANS_UDP_MCAST && transport->ssrc != 0) {
    g_ptr_array_add (strs, g_strdup (";ssrc="));
    g_ptr_array_add (strs, g_strdup_printf ("%08X", transport->ssrc));
  }

  /* add the mode parameter */
  if (transport->mode_play && transport->mode_record)
    g_ptr_array_add (strs, g_strdup (";mode=\"PLAY,RECORD\""));
  else if (transport->mode_record)
    g_ptr_array_add (strs, g_strdup (";mode=\"RECORD\""));
  else if (transport->mode_play)
    g_ptr_array_add (strs, g_strdup (";mode=\"PLAY\""));

  /* add a terminating NULL */
  g_ptr_array_add (strs, NULL);

  res = g_strjoinv (NULL, (gchar **) strs->pdata);
  g_strfreev ((gchar **) g_ptr_array_free (strs, FALSE));

  return res;

invalid_transport:
  {
    g_ptr_array_add (strs, NULL);
    g_strfreev ((gchar **) g_ptr_array_free (strs, FALSE));
    return NULL;
  }
}

/**
 * gst_rtsp_transport_free:
 * @transport: a #GstRTSPTransport
 *
 * Free the memory used by @transport.
 *
 * Returns: #GST_RTSP_OK.
 */
GstRTSPResult
gst_rtsp_transport_free (GstRTSPTransport * transport)
{
  g_return_val_if_fail (transport != NULL, GST_RTSP_EINVAL);

  gst_rtsp_transport_init (transport);
  g_free (transport);

  return GST_RTSP_OK;
}
