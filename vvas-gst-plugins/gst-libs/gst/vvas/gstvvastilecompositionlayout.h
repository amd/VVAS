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

#ifndef __GST_VVAS_TILE_COMPOSITION_LAYOUT_H__
#define __GST_VVAS_TILE_COMPOSITION_LAYOUT_H__

#include <gst/gst.h>
#include <gst/video/video.h>

G_BEGIN_DECLS
#define GST_VVAS_TILE_COMPOSITION_TILE_COUNT 4
#define GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR                                 \
  (gst_vvas_tile_composition_layout_error_quark ())
typedef struct _GstVvasTileCompositionLayout GstVvasTileCompositionLayout;

#define GST_TYPE_VVAS_TILE_COMPOSITION_LAYOUT                                  \
  (gst_vvas_tile_composition_layout_get_type ())

typedef enum
{
  GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_ARGUMENT,
  GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_UNSUPPORTED_FORMAT,
  GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_GEOMETRY,
  GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_STRIDE,
  GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_OUT_OF_BOUNDS,
  GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_OVERFLOW
} GstVvasTileCompositionLayoutError;

typedef struct _GstVvasTileCompositionTile
{
  guint tile_id;
  guint row;
  guint column;
  guint x;
  guint y;
  guint width;
  guint height;
  gsize plane_offset[GST_VIDEO_MAX_PLANES];
} GstVvasTileCompositionTile;

GST_EXPORT
GQuark gst_vvas_tile_composition_layout_error_quark (void);

GST_EXPORT
GType gst_vvas_tile_composition_layout_get_type (void) G_GNUC_CONST;

/**
 * gst_vvas_tile_composition_layout_new:
 * @master_info: complete master geometry, including actual stride and size
 * @layout_version: immutable version assigned by the owning coordinator
 * @error: (nullable): return location for a validation error
 *
 * Creates an immutable 2x2 packed RGB/BGR tile composition layout.
 * Programmer-error precondition failures return %NULL without setting @error.
 *
 * Returns: (transfer full) (nullable): a new layout, or %NULL on failure.
 */
GST_EXPORT
GstVvasTileCompositionLayout *gst_vvas_tile_composition_layout_new (
    const GstVideoInfo *master_info, guint64 layout_version, GError **error);

/**
 * gst_vvas_tile_composition_layout_ref:
 * @layout: a tile composition layout
 *
 * Returns: (transfer full): @layout with its reference count increased.
 */
GST_EXPORT
GstVvasTileCompositionLayout *gst_vvas_tile_composition_layout_ref (
    GstVvasTileCompositionLayout *layout);

GST_EXPORT
void gst_vvas_tile_composition_layout_unref (
    GstVvasTileCompositionLayout *layout);

/**
 * gst_vvas_tile_composition_layout_get_master_info:
 * @layout: a tile composition layout
 *
 * Returns: (transfer none): immutable master video information owned by
 * @layout.
 */
GST_EXPORT
const GstVideoInfo *
gst_vvas_tile_composition_layout_get_master_info (
    const GstVvasTileCompositionLayout *layout);

GST_EXPORT
gsize gst_vvas_tile_composition_layout_get_allocation_size (
    const GstVvasTileCompositionLayout *layout);

GST_EXPORT
guint64 gst_vvas_tile_composition_layout_get_version (
    const GstVvasTileCompositionLayout *layout);

/**
 * gst_vvas_tile_composition_layout_get_tile:
 * @layout: a tile composition layout
 * @tile_id: tile index in the range 0..3
 *
 * Returns: (transfer none) (nullable): immutable tile-slot geometry.
 */
GST_EXPORT
const GstVvasTileCompositionTile *
gst_vvas_tile_composition_layout_get_tile (
    const GstVvasTileCompositionLayout *layout, guint tile_id);

/**
 * gst_vvas_tile_composition_layout_prepare_tile_info:
 * @layout: a tile composition layout
 * @tile_id: tile index in the range 0..3
 * @input_info: tile-sized producer caps information
 * @tile_info: (out): prepared tile view
 * @error: (nullable): return location for a validation error
 *
 * Validates a producer against its tile slot. @tile_info retains the producer
 * width and active height, but uses the master's stride, an absolute tile
 * offset and the full master allocation size. Programmer-error precondition
 * failures return %FALSE without setting @error.
 *
 * Returns: %TRUE when @tile_info was prepared.
 */
GST_EXPORT
gboolean gst_vvas_tile_composition_layout_prepare_tile_info (
    const GstVvasTileCompositionLayout *layout,
    guint tile_id,
    const GstVideoInfo *input_info,
    GstVideoInfo *tile_info,
    GError **error);

G_END_DECLS
#endif /* __GST_VVAS_TILE_COMPOSITION_LAYOUT_H__ */
