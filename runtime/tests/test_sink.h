#ifndef RUNTIME_TEST_SINK_H
#define RUNTIME_TEST_SINK_H
#include "../gstruntimertpsession.h"
typedef struct { GMutex lock; guint count; gchar *first_domain; gint first_code; } TestSink;
static void test_sink_offer (gpointer data, const gchar *domain, gint code,
                             const gchar *message, gsize length)
{
  TestSink *sink = data;
  g_mutex_lock (&sink->lock);
  if (!sink->count) { sink->first_domain = g_strdup (domain); sink->first_code = code; }
  sink->count++;
  g_mutex_unlock (&sink->lock);
}
static GstRuntimeDiagnosticSink test_sink (TestSink *sink)
{ return (GstRuntimeDiagnosticSink) { test_sink_offer, sink }; }
static void test_sink_assert_none (TestSink *sink)
{ g_assert_cmpuint (sink->count, ==, 0); }
#endif
