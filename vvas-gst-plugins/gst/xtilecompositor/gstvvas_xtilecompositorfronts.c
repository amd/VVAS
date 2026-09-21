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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstvvas_xtilecompositorfronts.h"

static gboolean
identity_equal (const GstVvasTileCompositionIdentity * first,
    const GstVvasTileCompositionIdentity * second)
{
  return first->slot_id == second->slot_id &&
      first->epoch == second->epoch &&
      first->layout_version == second->layout_version;
}

GstVvasXTileCompositorFrontResult
gst_vvas_xtilecompositor_align_fronts (GstVvasXTileCompositorPeekFrontFunc
    peek_front, GstVvasXTileCompositorPopFrontFunc pop_front,
    GstVvasXTileCompositorAbortFrontFunc abort_front,
    GstVvasXTileCompositorMismatchFunc report_mismatch, gpointer user_data,
    GstVvasTileCompositionIdentity
    identities[GST_VVAS_TILE_COMPOSITION_TILE_COUNT])
{
  g_return_val_if_fail (peek_front != NULL,
      GST_VVAS_XTILECOMPOSITOR_FRONTS_ERROR);
  g_return_val_if_fail (pop_front != NULL,
      GST_VVAS_XTILECOMPOSITOR_FRONTS_ERROR);
  g_return_val_if_fail (abort_front != NULL,
      GST_VVAS_XTILECOMPOSITOR_FRONTS_ERROR);
  g_return_val_if_fail (identities != NULL,
      GST_VVAS_XTILECOMPOSITOR_FRONTS_ERROR);

  while (TRUE) {
    guint64 oldest_epoch = G_MAXUINT64;
    gboolean aligned = TRUE;
    gboolean popped = FALSE;
    guint tile;

    for (tile = 0; tile < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile++) {
      if (!peek_front (user_data, tile, &identities[tile]))
        return GST_VVAS_XTILECOMPOSITOR_FRONTS_NEED_DATA;
      oldest_epoch = MIN (oldest_epoch, identities[tile].epoch);
      if (tile && !identity_equal (&identities[0], &identities[tile]))
        aligned = FALSE;
    }
    if (aligned)
      return GST_VVAS_XTILECOMPOSITOR_FRONTS_ALIGNED;
    if (report_mismatch)
      report_mismatch (user_data, identities);

    for (tile = 0; tile < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile++) {
      gboolean duplicate = FALSE;
      GstVvasTileCompositionResult abort_result;
      guint previous;

      if (identities[tile].epoch != oldest_epoch)
        continue;
      for (previous = 0; previous < tile; previous++) {
        if (identities[previous].epoch == oldest_epoch &&
            identity_equal (&identities[previous], &identities[tile])) {
          duplicate = TRUE;
          break;
        }
      }
      if (duplicate)
        continue;
      abort_result = abort_front (user_data, &identities[tile]);
      if (abort_result != GST_VVAS_TILE_COMPOSITION_RESULT_OK &&
          abort_result != GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE &&
          abort_result != GST_VVAS_TILE_COMPOSITION_RESULT_STALE)
        return GST_VVAS_XTILECOMPOSITOR_FRONTS_ERROR;
    }

    for (tile = 0; tile < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile++) {
      if (identities[tile].epoch != oldest_epoch)
        continue;
      if (!pop_front (user_data, tile))
        return GST_VVAS_XTILECOMPOSITOR_FRONTS_ERROR;
      popped = TRUE;
    }
    if (!popped)
      return GST_VVAS_XTILECOMPOSITOR_FRONTS_ERROR;
  }
}
