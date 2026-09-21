/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

/**
 * SECTION: gstvvaslogbridge
 * @title: VVAS / VART logging bridge
 *
 * Bridges vvas-core and vart-x logging into GStreamer's GST_DEBUG via the
 * Phase 1 VvasLogBackend / LogBackend APIs (accept + deliver hooks).
 */

#ifndef GST_VVAS_LOG_BRIDGE_H
#define GST_VVAS_LOG_BRIDGE_H

#include <gst/gst.h>

G_BEGIN_DECLS

void gst_vvas_log_bridge_install (void);
void gst_vvas_log_bridge_push_object (GstObject * obj);
void gst_vvas_log_bridge_pop_object (void);
void gst_vvas_log_bridge_attach_thread (GstObject * obj);

static inline void
gst_vvas_log_scope_cleanup_ (int *p)
{
  (void) p;
  gst_vvas_log_bridge_pop_object ();
}

#define GST_VVAS_LOG_SCOPE(obj)                                              \
  __attribute__ ((cleanup (gst_vvas_log_scope_cleanup_)))                    \
  int G_PASTE (_vlog_scope_, __LINE__) =                                     \
      (gst_vvas_log_bridge_push_object (GST_OBJECT (obj)), 0)

G_END_DECLS

#endif /* GST_VVAS_LOG_BRIDGE_H */
