#include "gstruntimesdp.h"
#include <gio/gio.h>
#include <gst/rtp/gstrtppayloads.h>
#include <gst/sdp/gstsdpmessage.h>
#include <string.h>

typedef struct
{
  int scope, type;
  const char *value;
} Field;
struct _GstRuntimeSdp
{
  GstSDPMessage *message;
  gchar **lines;
  GArray *fields;
  GPtrArray *formats;
};
static gboolean
decimal (const char *v, guint64 maximum)
{
  if (!v || !*v)
    return FALSE;
  guint64 n = 0;
  for (const guchar *p = (const guchar *)v; *p; p++)
    {
      if (!g_ascii_isdigit (*p) || n > (maximum - (*p - '0')) / 10)
        return FALSE;
      n = n * 10 + (*p - '0');
    }
  return n <= maximum;
}
static gboolean
digits (const char *v)
{
  if (!v || !*v)
    return FALSE;
  for (const guchar *p = (const guchar *)v; *p; p++)
    if (!g_ascii_isdigit (*p))
      return FALSE;
  return TRUE;
}
static gboolean
token (const char *v)
{
  if (!v || !*v)
    return FALSE;
  for (const guchar *p = (const guchar *)v; *p; p++)
    if (*p <= 32 || *p >= 127 || strchr ("()<>@,;:\\[]\"", *p))
      return FALSE;
  return TRUE;
}
static gboolean
typed_time (const char *v, gboolean sign)
{
  if (sign && *v == '-')
    v++;
  gsize n = strlen (v);
  if (n && strchr ("dhms", v[n - 1]))
    n--;
  if (!n)
    return FALSE;
  for (gsize i = 0; i < n; i++)
    if (!g_ascii_isdigit (v[i]))
      return FALSE;
  return n == strlen (v) || n + 1 == strlen (v);
}
static gboolean
validate_value (char type, const char *value)
{
  if (!*value)
    return FALSE;
  if (type == 'v')
    return !strcmp (value, "0");
  if (strchr ("siuep", type))
    return TRUE;
  if (type == 'a' || type == 'k' || type == 'b')
    {
      const char *colon = strchr (value, ':');
      gchar *key = colon ? g_strndup (value, colon - value) : g_strdup (value);
      gboolean ok = token (key) && strlen (key) < 8192;
      g_free (key);
      if (type == 'b')
        ok = ok && colon && decimal (colon + 1, G_MAXUINT32);
      return ok;
    }
  if (type == 'm')
    {
      guint tokens = 1;
      for (const char *p = value; *p; p++)
        if (*p == ' ' && ++tokens > 4096)
          return FALSE;
    }
  gchar **parts = g_strsplit (value, " ", -1);
  guint count = g_strv_length (parts);
  gboolean ok = TRUE;
  for (guint i = 0; i < count; i++)
    if (!*parts[i] || strlen (parts[i]) >= 8192)
      ok = FALSE;
  switch (type)
    {
    case 'o':
      ok = ok && count == 6 && digits (parts[1]) && digits (parts[2]) && token (parts[3])
           && token (parts[4]);
      break;
    case 'm':
      {
        ok = ok && count >= 4 && token (parts[0]) && token (parts[2]);
        if (ok)
          {
            gchar **range = g_strsplit (parts[1], "/", -1);
            guint n = g_strv_length (range);
            ok = n <= 2 && decimal (range[0], 65535)
                 && (n == 1 || (decimal (range[1], 65535) && strcmp (range[1], "0")));
            g_strfreev (range);
            gboolean seen[128] = { FALSE };
            for (guint i = 3; ok && i < count; i++)
              {
                ok = token (parts[i]);
                if (g_str_has_prefix (parts[2], "RTP/"))
                  {
                    ok = ok && decimal (parts[i], 127);
                    if (ok)
                      {
                        guint pt = atoi (parts[i]);
                        ok = !seen[pt];
                        seen[pt] = TRUE;
                      }
                  }
              }
          }
        break;
      }
    case 'c':
      {
        ok = ok && count == 3 && token (parts[0]) && token (parts[1]);
        if (ok)
          {
            gchar **address = g_strsplit (parts[2], "/", -1);
            guint n = g_strv_length (address);
            ok = *address[0] && n <= (!strcmp (parts[1], "IP4") ? 3u : 2u);
            for (guint i = 1; ok && i < n; i++)
              ok = decimal (address[i], i == 1 && !strcmp (parts[1], "IP4") ? 255 : G_MAXUINT32);
            g_strfreev (address);
          }
        break;
      }
    case 't':
      ok = ok && count == 2 && digits (parts[0]) && digits (parts[1]);
      break;
    case 'r':
      ok = ok && count >= 3;
      for (guint i = 0; ok && i < count; i++)
        ok = typed_time (parts[i], FALSE);
      break;
    case 'z':
      ok = ok && count >= 2 && count % 2 == 0;
      for (guint i = 0; ok && i < count; i++)
        ok = i % 2 ? typed_time (parts[i], TRUE) : digits (parts[i]);
      break;
    default:
      ok = FALSE;
    }
  g_strfreev (parts);
  return ok;
}
static gboolean
fields (GstRuntimeSdp *s, const guint8 *data, gsize length)
{
  /* Protect the existing parser's materialization and fixed token workspace.
   * This is an input buffer bound, not a cumulative session/media quota. */
  if (!length || length > 16 * 1024 * 1024 || memchr (data, 0, length)
      || !g_utf8_validate ((const char *)data, length, NULL))
    return FALSE;
  guint lines = 1;
  gsize line_length = 0;
  for (gsize i = 0; i < length; i++)
    {
      if (data[i] == '\n')
        {
          if (++lines > 65536)
            return FALSE;
          line_length = 0;
        }
      else if (++line_length > 65537)
        return FALSE;
    }
  gchar *text = g_strndup ((const char *)data, length);
  s->lines = g_strsplit (text, "\n", -1);
  g_free (text);
  if (g_strv_length (s->lines) > 65536)
    return FALSE;
  int scope = -1, rank = -1;
  gboolean version = FALSE, origin = FALSE, name = FALSE, time = FALSE;
  for (guint i = 0; s->lines[i]; i++)
    {
      gchar *line = s->lines[i];
      gsize n = strlen (line);
      if (n && line[n - 1] == '\r')
        line[--n] = 0;
      if (!n && !s->lines[i + 1])
        break;
      if (n < 3 || n > 65536 || line[1] != '=' || memchr (line, '\r', n))
        return FALSE;
      for (gsize j = 0; j < n; j++)
        if ((guchar)line[j] < 32 && line[j] != '\t')
          return FALSE;
      char type = line[0];
      if (type == 'm')
        {
          if (!version || !origin || !name || !time)
            return FALSE;
          scope++;
          rank = -1;
        }
      const char *order = scope < 0 ? "vosiuepcbtrzka" : "micbka";
      const char *position = strchr (order, type);
      if (!position || type == ' ')
        return FALSE;
      int next = position - order;
      if (scope < 0 && type == 'r' && (!time || rank < (strchr (order, 't') - order)))
        return FALSE;
      if (scope < 0 && type == 't' && rank == (strchr (order, 'r') - order))
        rank = next;
      if (next < rank)
        return FALSE;
      if (next == rank && !strchr (scope < 0 ? "epbtra" : "cbam", type))
        return FALSE;
      rank = next;
      if (type == 'v')
        version = TRUE;
      if (type == 'o')
        origin = TRUE;
      if (type == 's')
        name = TRUE;
      if (type == 't')
        time = TRUE;
      if (!validate_value (type, line + 2))
        return FALSE;
      Field f = { scope, type, line + 2 };
      g_array_append_val (s->fields, f);
    }
  return version && origin && name && time;
}
static void
caps_free (gpointer caps)
{
  if (caps)
    gst_caps_unref (caps);
}
int
gst_runtime_sdp_new (const guint8 *data, gsize length, GstRuntimeSdp **result)
{
  if (!result || !data)
    return -2;
  *result = NULL;
  GstRuntimeSdp *s = g_new0 (GstRuntimeSdp, 1);
  s->fields = g_array_new (FALSE, FALSE, sizeof (Field));
  s->formats = g_ptr_array_new_with_free_func (caps_free);
  if (!fields (s, data, length) || gst_sdp_message_new (&s->message) != GST_SDP_OK
      || gst_sdp_message_parse_buffer (data, length, s->message) != GST_SDP_OK)
    {
      gst_runtime_sdp_free (s);
      return -2;
    }
  for (guint i = 0; i < gst_sdp_message_medias_len (s->message); i++)
    {
      const GstSDPMedia *m = gst_sdp_message_get_media (s->message, i);
      for (guint j = 0; j < gst_sdp_media_formats_len (m); j++)
        {
          const char *format = gst_sdp_media_get_format (m, j);
          GstCaps *caps = NULL;
          if (g_str_has_prefix (gst_sdp_media_get_proto (m), "RTP/") && decimal (format, 127))
            {
              caps = gst_sdp_media_get_caps_from_media (m, atoi (format));
              if (caps)
                {
                  GstStructure *structure = gst_caps_get_structure (caps, 0);
                  if (!gst_structure_has_field (structure, "encoding-name"))
                    {
                      const GstRTPPayloadInfo *info = gst_rtp_payload_info_for_pt (atoi (format));
                      if (info && info->encoding_name)
                        gst_structure_set (structure, "encoding-name", G_TYPE_STRING,
                                           info->encoding_name, NULL);
                    }
                }
            }
          g_ptr_array_add (s->formats, caps);
        }
    }
  *result = s;
  return 0;
}
void
gst_runtime_sdp_free (GstRuntimeSdp *s)
{
  if (!s)
    return;
  if (s->message)
    gst_sdp_message_free (s->message);
  g_strfreev (s->lines);
  if (s->fields)
    g_array_unref (s->fields);
  if (s->formats)
    g_ptr_array_unref (s->formats);
  g_free (s);
}
guint
gst_runtime_sdp_media_count (GstRuntimeSdp *s)
{
  return s ? gst_sdp_message_medias_len (s->message) : 0;
}
int
gst_runtime_sdp_media (GstRuntimeSdp *s, guint index, GstRuntimeSdpMedia *v)
{
  if (!s || !v || index >= gst_runtime_sdp_media_count (s))
    return -2;
  const GstSDPMedia *m = gst_sdp_message_get_media (s->message, index);
  *v = (GstRuntimeSdpMedia){ gst_sdp_media_get_media (m), gst_sdp_media_get_proto (m),
                             gst_sdp_media_get_port (m), gst_sdp_media_get_num_ports (m),
                             gst_sdp_media_formats_len (m) };
  return 0;
}
int
gst_runtime_sdp_field (GstRuntimeSdp *s, guint index, GstRuntimeSdpField *v)
{
  if (!s || !v || index >= s->fields->len)
    return -2;
  Field *f = &g_array_index (s->fields, Field, index);
  *v = (GstRuntimeSdpField){ f->scope, f->type, f->value };
  return 0;
}
static GstCaps *
format_caps (GstRuntimeSdp *s, guint media, guint format)
{
  if (!s || media >= gst_runtime_sdp_media_count (s))
    return NULL;
  const GstSDPMedia *m = gst_sdp_message_get_media (s->message, media);
  if (format >= gst_sdp_media_formats_len (m))
    return NULL;
  guint offset = format;
  for (guint i = 0; i < media; i++)
    offset += gst_sdp_media_formats_len (gst_sdp_message_get_media (s->message, i));
  return g_ptr_array_index (s->formats, offset);
}
int
gst_runtime_sdp_format (GstRuntimeSdp *s, guint media, guint format, const char **name)
{
  if (!s || !name || media >= gst_runtime_sdp_media_count (s))
    return -2;
  const GstSDPMedia *m = gst_sdp_message_get_media (s->message, media);
  if (format >= gst_sdp_media_formats_len (m))
    return -2;
  *name = gst_sdp_media_get_format (m, format);
  return 0;
}
int
gst_runtime_sdp_parameter (GstRuntimeSdp *s, guint media, guint format, guint index,
                           GstRuntimeSdpParameter *view)
{
  if (!view)
    return -2;
  GstCaps *caps = format_caps (s, media, format);
  if (!caps)
    return 1;
  const GstStructure *structure = gst_caps_get_structure (caps, 0);
  if (index >= (guint)gst_structure_n_fields (structure))
    return 1;
  const char *name = gst_structure_nth_field_name (structure, index);
  const GValue *value = gst_structure_get_value (structure, name);
  *view = (GstRuntimeSdpParameter){ .name = name };
  if (G_VALUE_HOLDS_STRING (value))
    {
      view->type = 1;
      view->text = g_value_get_string (value);
    }
  else if (G_VALUE_HOLDS_INT (value))
    {
      view->type = 2;
      view->number = g_value_get_int (value);
    }
  else if (G_VALUE_HOLDS_UINT (value))
    {
      view->type = 2;
      view->number = g_value_get_uint (value);
    }
  else if (G_VALUE_HOLDS_BOOLEAN (value))
    {
      view->type = 3;
      view->number = g_value_get_boolean (value);
    }
  else
    return -2;
  return 0;
}
static gboolean
control_uri (const char *text)
{
  GUri *uri = g_uri_parse (text, G_URI_FLAGS_ENCODED, NULL);
  if (!uri)
    return FALSE;
  const char *scheme = g_uri_get_scheme (uri), *host = g_uri_get_host (uri);
  gboolean valid
      = scheme && (!g_ascii_strcasecmp (scheme, "rtsp") || !g_ascii_strcasecmp (scheme, "rtsps"))
        && host && *host && !g_uri_get_userinfo (uri) && !g_uri_get_fragment (uri);
  g_uri_unref (uri);
  return valid;
}
int
gst_runtime_sdp_control (GstRuntimeSdp *s, int media, const char *base, char **result)
{
  if (!s || !base || !result || media < -1
      || (media >= 0 && (guint)media >= gst_runtime_sdp_media_count (s)) || !control_uri (base))
    return -2;
  *result = NULL;
  const char *control = NULL;
  for (guint i = 0; i < s->fields->len; i++)
    {
      Field *f = &g_array_index (s->fields, Field, i);
      if (f->scope == media && f->type == 'a' && g_str_has_prefix (f->value, "control:"))
        {
          if (control)
            return -2;
          control = f->value + strlen ("control:");
        }
    }
  if (!control)
    return 0;
  if (!strcmp (control, "*"))
    *result = g_strdup (base);
  else
    *result = g_uri_resolve_relative (base, control, G_URI_FLAGS_ENCODED, NULL);
  if (!*result || !control_uri (*result))
    {
      g_clear_pointer (result, g_free);
      return -2;
    }
  return 0;
}
void
gst_runtime_sdp_text_free (char *text)
{
  g_free (text);
}
