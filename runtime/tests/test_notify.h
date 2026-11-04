#ifndef RUNTIME_TEST_NOTIFY_H
#define RUNTIME_TEST_NOTIFY_H
#include "../gstruntimenotify.h"
static void test_noop_notify (gpointer data) {}
static GstRuntimeNotify test_notify (void)
{ return (GstRuntimeNotify) { test_noop_notify, NULL, NULL }; }
#endif
