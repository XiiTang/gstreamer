#ifndef GST_RUNTIME_NOTIFY_H
#define GST_RUNTIME_NOTIFY_H
#include <gst/gst.h>
G_BEGIN_DECLS
/* Owned at constructor entry, even on failure. Called from native/caller
 * threads after publishing readiness. Must not block or re-enter the owner.
 * destroy runs once after all callbacks have returned, before native free. */
typedef struct {
  void (*notify) (gpointer data);
  gpointer data;
  GDestroyNotify destroy;
} GstRuntimeNotify;
static inline void gst_runtime_notify (GstRuntimeNotify *wake)
{ if (wake->notify) wake->notify (wake->data); }
static inline void gst_runtime_notify_clear (GstRuntimeNotify *wake)
{
  if (wake->destroy) wake->destroy (wake->data);
  *wake = (GstRuntimeNotify) {0};
}
G_END_DECLS
#endif
