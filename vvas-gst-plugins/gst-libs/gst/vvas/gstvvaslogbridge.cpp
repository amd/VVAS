/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "gstvvaslogbridge.h"

#include <gst/gst.h>
#include <vvas_core/vvas_log.h>
#ifdef HAVE_VART_X_LOG_BRIDGE
#include <vart/vart_logger.hpp>
#endif

#include <stdlib.h>
#include <string.h>

typedef enum
{
  SOURCE_VVAS_CORE = 0,
  SOURCE_VART_X = 1
} BridgeSource;

typedef struct
{
  BridgeSource source;
  const char *basename;
} CategoryCacheKey;

GST_DEBUG_CATEGORY_STATIC (vvas_core_fallback);
#ifdef HAVE_VART_X_LOG_BRIDGE
GST_DEBUG_CATEGORY_STATIC (vart_x_fallback);
#endif

static GMutex g_registry_mutex;
static GHashTable *g_category_cache = NULL;
static GPrivate g_tls_category;

static const GstDebugLevel level_map[] = {
  GST_LEVEL_NONE,
  GST_LEVEL_ERROR,
  GST_LEVEL_WARNING,
  GST_LEVEL_FIXME,
  GST_LEVEL_INFO,
  GST_LEVEL_DEBUG,
};

static guint
category_cache_hash (gconstpointer key)
{
  const CategoryCacheKey *k = (const CategoryCacheKey *) key;

  return g_str_hash (k->basename) ^
      ((guint) k->source * 2654435761u);
}

static gboolean
category_cache_equal (gconstpointer a, gconstpointer b)
{
  const CategoryCacheKey *ka = (const CategoryCacheKey *) a;
  const CategoryCacheKey *kb = (const CategoryCacheKey *) b;

  return ka->source == kb->source &&
      g_str_equal (ka->basename, kb->basename);
}

static void
category_cache_key_free (gpointer key)
{
  CategoryCacheKey *k = (CategoryCacheKey *) key;

  g_free ((gpointer) k->basename);
  g_free (k);
}

static GstDebugLevel
vvas_level_to_gst (int level)
{
  if (level < 0 || level >= (int) G_N_ELEMENTS (level_map))
    level = 1;
  return level_map[level];
}

static GstDebugCategory *
category_for_basename (BridgeSource source, const char *basename)
{
  CategoryCacheKey stack_key;
  GstDebugCategory *cat;

  if (!basename || basename[0] == '\0') {
    if (source == SOURCE_VVAS_CORE)
      return vvas_core_fallback;
#ifdef HAVE_VART_X_LOG_BRIDGE
    return vart_x_fallback;
#else
    return vvas_core_fallback;
#endif
  }

  stack_key.source = source;
  stack_key.basename = basename;

  g_mutex_lock (&g_registry_mutex);
  cat = (GstDebugCategory *) g_hash_table_lookup (g_category_cache, &stack_key);
  if (!cat) {
    char *cat_name = g_strdup_printf ("%s_%s",
        source == SOURCE_VVAS_CORE ? "vvas_core" : "vart_x", basename);
    char *desc = g_strdup_printf ("Forwarded log messages from %s", cat_name);

    cat = NULL;
    GST_DEBUG_CATEGORY_INIT (cat, cat_name, 0, desc);
    g_free (cat_name);
    g_free (desc);

    {
      CategoryCacheKey *heap_key = g_new (CategoryCacheKey, 1);

      heap_key->source = source;
      heap_key->basename = g_strdup (basename);
      g_hash_table_insert (g_category_cache, heap_key, cat);
    }
  }
  g_mutex_unlock (&g_registry_mutex);

  return cat;
}

typedef struct
{
  GstObject **stack;
  guint depth;
  guint capacity;
} ObjectStack;

static void
object_stack_free (gpointer data)
{
  ObjectStack *s = (ObjectStack *) data;

  if (!s)
    return;
  g_free (s->stack);
  g_free (s);
}

static GPrivate g_object_stack_key = G_PRIVATE_INIT (object_stack_free);

static ObjectStack *
get_or_create_stack (void)
{
  ObjectStack *s = (ObjectStack *) g_private_get (&g_object_stack_key);

  if (!s) {
    s = g_new0 (ObjectStack, 1);
    s->capacity = 8;
    s->stack = g_new0 (GstObject *, s->capacity);
    g_private_set (&g_object_stack_key, s);
  }
  return s;
}

void
gst_vvas_log_bridge_push_object (GstObject *obj)
{
  ObjectStack *s = get_or_create_stack ();

  if (s->depth == s->capacity) {
    s->capacity *= 2;
    s->stack = g_renew (GstObject *, s->stack, s->capacity);
  }
  s->stack[s->depth++] = obj;
}

void
gst_vvas_log_bridge_pop_object (void)
{
  ObjectStack *s = (ObjectStack *) g_private_get (&g_object_stack_key);

  if (s && s->depth > 0)
    s->depth--;
}

void
gst_vvas_log_bridge_attach_thread (GstObject *obj)
{
  ObjectStack *s = get_or_create_stack ();

  s->depth = 0;
  if (obj)
    s->stack[s->depth++] = obj;
}

static GstObject *
top_object (void)
{
  ObjectStack *s = (ObjectStack *) g_private_get (&g_object_stack_key);

  if (!s || s->depth == 0)
    return NULL;
  return s->stack[s->depth - 1];
}

static bool
bridge_accept (BridgeSource source, int level, const char *basename,
    int32_t instance_id)
{
  GstDebugCategory *cat;
  GstDebugLevel gst_lvl;

  (void) instance_id;

  cat = category_for_basename (source, basename);
  g_private_set (&g_tls_category, cat);

  if (level == 0)
    return TRUE;

  gst_lvl = vvas_level_to_gst (level);
  return gst_debug_category_get_threshold (cat) >= gst_lvl;
}

static void
bridge_deliver (BridgeSource source, int level, const char *basename,
    int32_t instance_id, const char *file, const char *func, uint32_t line,
    const char *message)
{
  GstDebugCategory *cat =
      (GstDebugCategory *) g_private_get (&g_tls_category);
  GstDebugLevel gst_lvl = vvas_level_to_gst (level);
  GstObject *obj = top_object ();

  if (!cat)
    cat = category_for_basename (source, basename);

  if (basename && instance_id >= 0) {
    gst_debug_log (cat, gst_lvl, file, func, line, (GObject *) obj,
        "[%s_%d] %s", basename, instance_id, message);
  } else if (basename) {
    gst_debug_log (cat, gst_lvl, file, func, line, (GObject *) obj,
        "[%s] %s", basename, message);
  } else {
    gst_debug_log (cat, gst_lvl, file, func, line, (GObject *) obj,
        "[(none)] %s", message);
  }
}

static bool
vvas_bridge_accept (VvasLogLevel level, const char *basename,
    int32_t instance_id, void *user_data)
{
  (void) user_data;
  return bridge_accept (SOURCE_VVAS_CORE, (int) level, basename, instance_id);
}

static void
vvas_bridge_deliver (VvasLogLevel level, const char *basename,
    int32_t instance_id, const char *file, const char *func, uint32_t line,
    const char *message, void *user_data)
{
  (void) user_data;
  bridge_deliver (SOURCE_VVAS_CORE, (int) level, basename, instance_id, file,
      func, line, message);
}

#ifdef HAVE_VART_X_LOG_BRIDGE
static bool
vart_bridge_accept (vart::LogLevel level, const char *basename,
    int32_t instance_id, void *user_data)
{
  (void) user_data;
  return bridge_accept (SOURCE_VART_X, static_cast < int >(level), basename,
      instance_id);
}

static void
vart_bridge_deliver (vart::LogLevel level, const char *basename,
    int32_t instance_id, const char *file, const char *func, uint32_t line,
    const char *message, void *user_data)
{
  (void) user_data;
  bridge_deliver (SOURCE_VART_X, static_cast < int >(level), basename,
      instance_id, file, func, line, message);
}
#endif

void
gst_vvas_log_bridge_install (void)
{
  static gsize installed = 0;

  if (!g_once_init_enter (&installed))
    return;

  GST_DEBUG_CATEGORY_INIT (vvas_core_fallback, "vvas_core", 0,
      "Forwarded log messages from vvas-core (no module)");
#ifdef HAVE_VART_X_LOG_BRIDGE
  GST_DEBUG_CATEGORY_INIT (vart_x_fallback, "vart_x", 0,
      "Forwarded log messages from vart-x (no module)");
#endif

  g_mutex_init (&g_registry_mutex);
  g_category_cache = g_hash_table_new_full (category_cache_hash,
      category_cache_equal, category_cache_key_free, NULL);

  /*
   * Promote-if-unset: when env filters are unset, set DEBUG so the library
   * admits messages and GST_DEBUG category thresholds control output. See
   * docs/logging-bridge.md.
   */
  if (!g_getenv ("VVAS_CORE_DEBUG"))
    g_setenv ("VVAS_CORE_DEBUG", "5", FALSE);
  static VvasLogBackend vvas_backend = {
    vvas_bridge_accept,
    vvas_bridge_deliver,
    NULL,
  };
  vvas_log_set_backend (&vvas_backend);

#ifdef HAVE_VART_X_LOG_BRIDGE
  static vart::LogBackend vart_backend = {
    vart_bridge_accept,
    vart_bridge_deliver,
    NULL,
  };
  vart::Logger::get_instance ().set_backend (&vart_backend);
#endif

  g_once_init_leave (&installed, 1);
}
