/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __GST_VVAS_XTILECOMPOSITOR_FRONTS_H__
#define __GST_VVAS_XTILECOMPOSITOR_FRONTS_H__

#include <gst/gst.h>
#include <gst/vvas/gstvvastilecompositioncoordinator.h>

G_BEGIN_DECLS

typedef enum {
  GST_VVAS_XTILECOMPOSITOR_FRONTS_ALIGNED,
  GST_VVAS_XTILECOMPOSITOR_FRONTS_NEED_DATA,
  GST_VVAS_XTILECOMPOSITOR_FRONTS_ERROR
} GstVvasXTileCompositorFrontResult;

typedef gboolean (*GstVvasXTileCompositorPeekFrontFunc) (gpointer user_data,
    guint tile_id,
    GstVvasTileCompositionIdentity *identity);

typedef gboolean (*GstVvasXTileCompositorPopFrontFunc) (
    gpointer user_data, guint tile_id);

typedef GstVvasTileCompositionResult (*GstVvasXTileCompositorAbortFrontFunc) (
    gpointer user_data, const GstVvasTileCompositionIdentity *identity);

typedef void (*GstVvasXTileCompositorMismatchFunc) (gpointer user_data,
    const GstVvasTileCompositionIdentity
        identities[GST_VVAS_TILE_COMPOSITION_TILE_COUNT]);

/**
 * gst_vvas_xtilecompositor_align_fronts:
 * @peek_front: returns the current identity for one tile, or false if empty
 * @pop_front: removes one current tile front
 * @abort_front: invalidates the epoch before its fronts are removed
 * @report_mismatch: optional observer called before each drain iteration
 * @user_data: callback context
 * @identities: receives the aligned identities only on ALIGNED
 *
 * Drains finite, monotonically increasing per-tile FIFO fronts. Each mismatch
 * iteration aborts each unique identity at the oldest visible epoch, then pops
 * every front at that epoch. The function terminates when all fronts match, a
 * tile FIFO is empty, a callback fails, or aborting an identity is unsafe.
 *
 * Returns: ALIGNED, NEED_DATA, or ERROR.
 */
GstVvasXTileCompositorFrontResult gst_vvas_xtilecompositor_align_fronts (
    GstVvasXTileCompositorPeekFrontFunc peek_front,
    GstVvasXTileCompositorPopFrontFunc pop_front,
    GstVvasXTileCompositorAbortFrontFunc abort_front,
    GstVvasXTileCompositorMismatchFunc report_mismatch,
    gpointer user_data,
    GstVvasTileCompositionIdentity
        identities[GST_VVAS_TILE_COMPOSITION_TILE_COUNT]);

G_END_DECLS
#endif
