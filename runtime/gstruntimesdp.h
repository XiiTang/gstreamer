#ifndef GST_RUNTIME_SDP_H
#define GST_RUNTIME_SDP_H
#include <gst/gst.h>
G_BEGIN_DECLS
typedef struct _GstRuntimeSdp GstRuntimeSdp;
typedef struct
{
  const char *media, *protocol;
  guint port, ports, formats;
} GstRuntimeSdpMedia;
typedef struct
{
  int scope, type;
  const char *value;
} GstRuntimeSdpField;
GST_API int gst_runtime_sdp_new (const guint8 *data, gsize length, GstRuntimeSdp **result);
GST_API void gst_runtime_sdp_free (GstRuntimeSdp *description);
GST_API guint gst_runtime_sdp_media_count (GstRuntimeSdp *description);
GST_API int gst_runtime_sdp_media (GstRuntimeSdp *description, guint index,
                                   GstRuntimeSdpMedia *view);
GST_API int gst_runtime_sdp_field (GstRuntimeSdp *description, guint index,
                                   GstRuntimeSdpField *view);
typedef struct
{
  const char *name, *text;
  gint64 number;
  int type;
} GstRuntimeSdpParameter;
GST_API int gst_runtime_sdp_format (GstRuntimeSdp *description, guint media, guint format,
                                    const char **name);
GST_API int gst_runtime_sdp_parameter (GstRuntimeSdp *description, guint media, guint format,
                                       guint index, GstRuntimeSdpParameter *view);
GST_API int gst_runtime_sdp_control (GstRuntimeSdp *description, int media, const char *base,
                                     char **result);
GST_API void gst_runtime_sdp_text_free (char *text);
G_END_DECLS
#endif
