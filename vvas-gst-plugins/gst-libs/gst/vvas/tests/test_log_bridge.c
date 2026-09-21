/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include <gst/gst.h>
#include <gst/vvas/gstvvaslogbridge.h>
#include <vvas_core/vvas_log.h>

#include <stdio.h>
#include <string.h>

typedef struct
{
  GMutex lock;
  GString *captured;
  guint count;
} CaptureCtx;

static CaptureCtx g_cap;

static void
capture_log_cb (GstDebugCategory *category, GstDebugLevel level,
    const gchar *file, const gchar *function, gint line,
    GObject *object, GstDebugMessage *message, gpointer user_data)
{
  (void) category;
  (void) level;
  (void) file;
  (void) function;
  (void) line;
  (void) user_data;
  g_mutex_lock (&g_cap.lock);
  g_string_append_printf (g_cap.captured,
      "[%s|%d|obj=%p] %s\n",
      gst_debug_category_get_name (category), (int) level,
      (void *) object, gst_debug_message_get (message));
  g_cap.count++;
  g_mutex_unlock (&g_cap.lock);
}

static void
reset_capture (void)
{
  g_mutex_lock (&g_cap.lock);
  g_string_truncate (g_cap.captured, 0);
  g_cap.count = 0;
  g_mutex_unlock (&g_cap.lock);
}

static gboolean
captured_contains (const char *needle)
{
  gboolean ret;
  g_mutex_lock (&g_cap.lock);
  ret = (strstr (g_cap.captured->str, needle) != NULL);
  g_mutex_unlock (&g_cap.lock);
  return ret;
}

static guint
captured_count (void)
{
  guint c;
  g_mutex_lock (&g_cap.lock);
  c = g_cap.count;
  g_mutex_unlock (&g_cap.lock);
  return c;
}

#define ASSERT(cond, msg) do { \
  if (!(cond)) { \
    fprintf (stderr, "FAIL %s:%d %s\n", __func__, __LINE__, msg); \
    return 1; \
  } \
} while (0)

static int
test_install_and_deliver (void)
{
  char *modname = NULL;
  VvasLogger *logger =
      vvas_logger_register (CORE_POST_PROCESS, VVAS_LOG_LEVEL_DEBUG, &modname);
  ASSERT (logger, "register failed");

  reset_capture ();
  VVAS_LOG_INFO_OBJ (logger, "bridge hello");
  ASSERT (captured_contains ("bridge hello"), "message body missing");
  ASSERT (captured_contains ("postprocess"), "module name missing");

  vvas_logger_deregister (logger);
  return 0;
}

static int
test_object_context_appears (void)
{
  GstObject *fake = (GstObject *) gst_bin_new ("test_bin_ctx");
  char needle[64];
  char *modname = NULL;
  VvasLogger *logger;

  reset_capture ();
  logger =
      vvas_logger_register (CORE_POST_PROCESS, VVAS_LOG_LEVEL_DEBUG, &modname);
  ASSERT (logger, "register failed");

  gst_vvas_log_bridge_push_object (fake);
  VVAS_LOG_INFO_OBJ (logger, "ctx hello");
  gst_vvas_log_bridge_pop_object ();

  g_snprintf (needle, sizeof (needle), "obj=%p", (void *) fake);
  ASSERT (captured_contains (needle),
      "GstObject pointer not present in captured output");
  ASSERT (captured_contains ("ctx hello"), "message body missing");
  ASSERT (captured_contains ("postprocess"), "module name missing");

  vvas_logger_deregister (logger);
  gst_object_unref (fake);
  return 0;
}

static int
test_per_module_category_filter (void)
{
  char *m1 = NULL, *m2 = NULL;
  VvasLogger *l_pp;
  VvasLogger *l_ov;

  gst_debug_set_threshold_for_name ("vvas_core_postprocess", GST_LEVEL_INFO);
  gst_debug_set_threshold_for_name ("vvas_core_overlay", GST_LEVEL_NONE);

  l_pp = vvas_logger_register (CORE_POST_PROCESS, VVAS_LOG_LEVEL_DEBUG, &m1);
  l_ov = vvas_logger_register (CORE_OVERLAY, VVAS_LOG_LEVEL_DEBUG, &m2);

  reset_capture ();
  VVAS_LOG_INFO_OBJ (l_pp, "from postprocess");
  VVAS_LOG_INFO_OBJ (l_ov, "from overlay");

  ASSERT (captured_contains ("from postprocess"),
      "postprocess line filtered unexpectedly");
  ASSERT (!captured_contains ("from overlay"),
      "overlay line was not filtered out");

  gst_debug_set_threshold_for_name ("vvas_core_overlay", GST_LEVEL_INFO);

  vvas_logger_deregister (l_pp);
  vvas_logger_deregister (l_ov);
  return 0;
}

static int
test_instance_identity_display (void)
{
  char *m1 = NULL, *m2 = NULL;
  VvasLogger *a;
  VvasLogger *b;

  gst_debug_set_threshold_for_name ("vvas_core_imageprocess", GST_LEVEL_INFO);

  a = vvas_logger_register (CORE_IMAGE_PROCESS, VVAS_LOG_LEVEL_DEBUG, &m1);
  b = vvas_logger_register (CORE_IMAGE_PROCESS, VVAS_LOG_LEVEL_DEBUG, &m2);
  ASSERT (a && b, "register failed");

  reset_capture ();
  VVAS_LOG_INFO_OBJ (a, "alpha");
  VVAS_LOG_INFO_OBJ (b, "beta");

  ASSERT (captured_contains ("vvas_core_imageprocess"),
      "imageprocess category did not appear");
  ASSERT (captured_contains ("alpha") && captured_contains ("beta"),
      "instance messages missing");
  ASSERT (captured_contains ("[imageprocess_0]"),
      "first instance display tag missing");
  ASSERT (captured_contains ("[imageprocess_1]"),
      "second instance display tag missing");

  vvas_logger_deregister (a);
  vvas_logger_deregister (b);
  return 0;
}

static int
test_accept_drops_before_deliver (void)
{
  char *modname = NULL;
  VvasLogger *logger;
  guint after_debug;
  guint after_error;

  gst_debug_set_threshold_for_name ("vvas_core_postprocess", GST_LEVEL_ERROR);

  logger =
      vvas_logger_register (CORE_POST_PROCESS, VVAS_LOG_LEVEL_DEBUG, &modname);
  ASSERT (logger, "register failed");

  reset_capture ();
  VVAS_LOG_DEBUG_OBJ (logger, "debug dropped");
  after_debug = captured_count ();
  VVAS_LOG_ERROR_OBJ (logger, "error kept");
  after_error = captured_count ();

  ASSERT (after_debug == 0, "DEBUG reached deliver despite ERROR threshold");
  ASSERT (after_error == 1, "ERROR not delivered");
  ASSERT (captured_contains ("error kept"), "ERROR body missing");
  ASSERT (!captured_contains ("debug dropped"), "DEBUG body leaked");

  gst_debug_set_threshold_for_name ("vvas_core_postprocess", GST_LEVEL_DEBUG);

  vvas_logger_deregister (logger);
  return 0;
}

static int
test_none_level_preserved (void)
{
  char *modname = NULL;
  VvasLogger *logger =
      vvas_logger_register (CORE_POST_PROCESS, VVAS_LOG_LEVEL_DEBUG, &modname);

  ASSERT (logger, "register failed");

  gst_debug_set_threshold_for_name ("vvas_core_postprocess", GST_LEVEL_ERROR);

  reset_capture ();
  vvas_logger_log_obj (VVAS_LOG_LEVEL_NONE, logger, __FILE__, __func__,
      __LINE__, "profiler line");

  ASSERT (captured_count () == 1, "NONE level message not delivered");
  ASSERT (captured_contains ("profiler line"), "message body missing");
  ASSERT (captured_contains ("postprocess|0|"),
      "NONE not mapped to GST_LEVEL_NONE");
  ASSERT (!captured_contains ("postprocess|1|"),
      "NONE incorrectly mapped to GST_LEVEL_ERROR");

  gst_debug_set_threshold_for_name ("vvas_core_postprocess", GST_LEVEL_DEBUG);

  vvas_logger_deregister (logger);
  return 0;
}

int
main (int argc, char **argv)
{
  int rc = 0;

  gst_init (&argc, &argv);
  g_mutex_init (&g_cap.lock);
  g_cap.captured = g_string_sized_new (4096);

  gst_debug_set_colored (FALSE);
  gst_debug_set_default_threshold (GST_LEVEL_NONE);
  gst_debug_set_threshold_for_name ("vvas_core_*", GST_LEVEL_DEBUG);

  gst_vvas_log_bridge_install ();

  gst_debug_add_log_function (capture_log_cb, NULL, NULL);

  rc |= test_install_and_deliver ();
  rc |= test_object_context_appears ();
  rc |= test_per_module_category_filter ();
  rc |= test_instance_identity_display ();
  rc |= test_accept_drops_before_deliver ();
  rc |= test_none_level_preserved ();

  if (rc == 0)
    printf ("PASS: test_log_bridge\n");
  else
    printf ("FAIL: test_log_bridge rc=%d\n", rc);
  g_string_free (g_cap.captured, TRUE);
  return rc;
}
