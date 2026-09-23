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

#include "gstvvastilecompositionlayout.h"

#include <stdarg.h>

GST_DEBUG_CATEGORY_STATIC (gst_vvas_tile_composition_layout_debug);
#define GST_CAT_DEFAULT gst_vvas_tile_composition_layout_debug

struct _GstVvasTileCompositionLayout
{
  GstMiniObject parent;
  GstVideoInfo master_info;
  gsize allocation_size;
  guint64 layout_version;
  GstVvasTileCompositionTile tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT];
};

GST_DEFINE_MINI_OBJECT_TYPE (GstVvasTileCompositionLayout,
    gst_vvas_tile_composition_layout);

static void
gst_vvas_tile_composition_layout_ensure_debug_category (void)
{
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized)) {
    GST_DEBUG_CATEGORY_INIT (gst_vvas_tile_composition_layout_debug,
        "vvas_tile_composition", 0,
        "VVAS tile composition coordinator and layout");
    g_once_init_leave (&initialized, 1);
  }
}

static gboolean
gst_vvas_tile_composition_layout_set_error (GError ** error,
    GstVvasTileCompositionLayoutError code, const gchar * format, ...)
G_GNUC_PRINTF (3, 4);

     static gboolean
         gst_vvas_tile_composition_layout_set_error (GError ** error,
    GstVvasTileCompositionLayoutError code, const gchar * format, ...)
{
  va_list args;
  gchar *message;

  if (!error)
    return FALSE;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);

  g_set_error_literal (error, GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR, code,
      message);
  g_free (message);
  return FALSE;
}

static gboolean
gst_vvas_tile_composition_layout_checked_mul (gsize left, gsize right,
    gsize * result)
{
  if (left && right > G_MAXSIZE / left)
    return FALSE;

  *result = left * right;
  return TRUE;
}

static gboolean
gst_vvas_tile_composition_layout_checked_add (gsize left, gsize right,
    gsize * result)
{
  if (left > G_MAXSIZE - right)
    return FALSE;

  *result = left + right;
  return TRUE;
}

static gboolean
gst_vvas_tile_composition_layout_active_span (guint height, gsize stride,
    gsize row_bytes, gsize * active_span)
{
  gsize preceding_rows;

  if (!height)
    return FALSE;

  if (!gst_vvas_tile_composition_layout_checked_mul (height - 1, stride,
          &preceding_rows))
    return FALSE;

  return gst_vvas_tile_composition_layout_checked_add (preceding_rows,
      row_bytes, active_span);
}

static void
gst_vvas_tile_composition_layout_free (GstVvasTileCompositionLayout * layout)
{
  g_slice_free (GstVvasTileCompositionLayout, layout);
}

GQuark
gst_vvas_tile_composition_layout_error_quark (void)
{
  return g_quark_from_static_string ("gst-vvas-tile-composition-layout-error");
}

GstVvasTileCompositionLayout *
gst_vvas_tile_composition_layout_new (const GstVideoInfo * master_info,
    guint64 layout_version, GError ** error)
{
  GstVvasTileCompositionLayout *layout;
  GstVideoFormat format;
  gint signed_width;
  gint signed_height;
  guint width;
  guint height;
  guint slot_width;
  guint slot_height;
  guint tile_id;
  gint pixel_stride;
  gint signed_stride;
  gsize stride;
  gsize row_bytes;
  gsize active_span;
  gsize allocation_size;

  g_return_val_if_fail (master_info != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  gst_vvas_tile_composition_layout_ensure_debug_category ();

  format = GST_VIDEO_INFO_FORMAT (master_info);
  if (format != GST_VIDEO_FORMAT_BGR && format != GST_VIDEO_FORMAT_RGB) {
    gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_UNSUPPORTED_FORMAT,
        "unsupported tile composition format %s",
        gst_video_format_to_string (format));
    return NULL;
  }

  if (GST_VIDEO_INFO_N_PLANES (master_info) != 1 ||
      GST_VIDEO_INFO_PLANE_OFFSET (master_info, 0) != 0) {
    gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_GEOMETRY,
        "packed tile composition requires one plane at offset zero");
    return NULL;
  }

  signed_width = GST_VIDEO_INFO_WIDTH (master_info);
  signed_height = GST_VIDEO_INFO_HEIGHT (master_info);
  if (signed_width <= 0 || signed_height <= 0 ||
      (signed_width % 2) || (signed_height % 2)) {
    gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_GEOMETRY,
        "2x2 tile composition dimensions must be positive and even, got %dx%d",
        signed_width, signed_height);
    return NULL;
  }
  width = signed_width;
  height = signed_height;

  pixel_stride = GST_VIDEO_INFO_COMP_PSTRIDE (master_info, 0);
  if (pixel_stride != 3) {
    gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_UNSUPPORTED_FORMAT,
        "packed RGB tile composition requires three-byte pixels, got %d",
        pixel_stride);
    return NULL;
  }

  signed_stride = GST_VIDEO_INFO_PLANE_STRIDE (master_info, 0);
  if (signed_stride <= 0) {
    gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_STRIDE,
        "tile composition stride must be positive, got %d", signed_stride);
    return NULL;
  }
  stride = signed_stride;

  if (!gst_vvas_tile_composition_layout_checked_mul (width, pixel_stride,
          &row_bytes)) {
    gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_OVERFLOW,
        "master row-byte calculation overflowed");
    return NULL;
  }
  if (stride < row_bytes) {
    gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_STRIDE,
        "tile composition stride %" G_GSIZE_FORMAT
        " is smaller than visible row bytes %" G_GSIZE_FORMAT,
        stride, row_bytes);
    return NULL;
  }

  allocation_size = GST_VIDEO_INFO_SIZE (master_info);
  if (!allocation_size) {
    gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_GEOMETRY,
        "tile composition allocation size must be nonzero");
    return NULL;
  }

  /* The final row needs only its visible bytes. Consumers that access padded
   * full rows must impose the stronger stride * height allocation contract. */
  if (!gst_vvas_tile_composition_layout_active_span (height, stride, row_bytes,
          &active_span)) {
    gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_OVERFLOW,
        "master active-span calculation overflowed");
    return NULL;
  }
  if (active_span > allocation_size) {
    gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_OUT_OF_BOUNDS,
        "master active span %" G_GSIZE_FORMAT
        " exceeds allocation %" G_GSIZE_FORMAT, active_span, allocation_size);
    return NULL;
  }

  layout = g_slice_new0 (GstVvasTileCompositionLayout);
  gst_mini_object_init (GST_MINI_OBJECT_CAST (layout), 0,
      gst_vvas_tile_composition_layout_get_type (), NULL, NULL,
      (GstMiniObjectFreeFunction) gst_vvas_tile_composition_layout_free);
  layout->master_info = *master_info;
  layout->allocation_size = allocation_size;
  layout->layout_version = layout_version;

  slot_width = width / 2;
  slot_height = height / 2;
  for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++) {
    GstVvasTileCompositionTile *tile = &layout->tiles[tile_id];
    gsize x_offset;
    gsize y_offset;
    gsize tile_row_bytes;
    gsize tile_active_span;
    gsize tile_end;

    tile->tile_id = tile_id;
    tile->row = tile_id / 2;
    tile->column = tile_id % 2;
    tile->x = tile->column * slot_width;
    tile->y = tile->row * slot_height;
    tile->width = slot_width;
    tile->height = slot_height;

    if (!gst_vvas_tile_composition_layout_checked_mul (tile->x, pixel_stride,
            &x_offset) ||
        !gst_vvas_tile_composition_layout_checked_mul (tile->y, stride,
            &y_offset)
        || !gst_vvas_tile_composition_layout_checked_add (y_offset, x_offset,
            &tile->plane_offset[0])
        || !gst_vvas_tile_composition_layout_checked_mul (tile->width,
            pixel_stride, &tile_row_bytes)
        || !gst_vvas_tile_composition_layout_active_span (tile->height, stride,
            tile_row_bytes, &tile_active_span)
        || !gst_vvas_tile_composition_layout_checked_add (tile->plane_offset[0],
            tile_active_span, &tile_end)) {
      gst_vvas_tile_composition_layout_set_error (error,
          GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_OVERFLOW,
          "tile %u layout calculation overflowed", tile_id);
      gst_vvas_tile_composition_layout_unref (layout);
      return NULL;
    }

    if (tile_end > allocation_size) {
      gst_vvas_tile_composition_layout_set_error (error,
          GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_OUT_OF_BOUNDS,
          "tile %u end %" G_GSIZE_FORMAT
          " exceeds allocation %" G_GSIZE_FORMAT,
          tile_id, tile_end, allocation_size);
      gst_vvas_tile_composition_layout_unref (layout);
      return NULL;
    }
  }

  GST_INFO ("created layout version %" G_GUINT64_FORMAT
      " format=%s master=%ux%u stride=%d allocation=%" G_GSIZE_FORMAT,
      layout_version, gst_video_format_to_string (format), width, height,
      signed_stride, allocation_size);

  return layout;
}

GstVvasTileCompositionLayout *
gst_vvas_tile_composition_layout_ref (GstVvasTileCompositionLayout * layout)
{
  g_return_val_if_fail (layout != NULL, NULL);

  return (GstVvasTileCompositionLayout *)
      gst_mini_object_ref (GST_MINI_OBJECT_CAST (layout));
}

void
gst_vvas_tile_composition_layout_unref (GstVvasTileCompositionLayout * layout)
{
  g_return_if_fail (layout != NULL);

  gst_mini_object_unref (GST_MINI_OBJECT_CAST (layout));
}

const GstVideoInfo *
gst_vvas_tile_composition_layout_get_master_info (const
    GstVvasTileCompositionLayout * layout)
{
  g_return_val_if_fail (layout != NULL, NULL);

  return &layout->master_info;
}

gsize
gst_vvas_tile_composition_layout_get_allocation_size (const
    GstVvasTileCompositionLayout * layout)
{
  g_return_val_if_fail (layout != NULL, 0);

  return layout->allocation_size;
}

guint64
gst_vvas_tile_composition_layout_get_version (const GstVvasTileCompositionLayout
    * layout)
{
  g_return_val_if_fail (layout != NULL, 0);

  return layout->layout_version;
}

const GstVvasTileCompositionTile *
gst_vvas_tile_composition_layout_get_tile (const GstVvasTileCompositionLayout *
    layout, guint tile_id)
{
  g_return_val_if_fail (layout != NULL, NULL);

  if (tile_id >= GST_VVAS_TILE_COMPOSITION_TILE_COUNT)
    return NULL;

  return &layout->tiles[tile_id];
}

gboolean
gst_vvas_tile_composition_layout_prepare_tile_info (const
    GstVvasTileCompositionLayout * layout, guint tile_id,
    const GstVideoInfo * input_info, GstVideoInfo * tile_info, GError ** error)
{
  const GstVvasTileCompositionTile *tile;
  GstVideoFormat input_format;
  GstVideoFormat master_format;
  gint input_width;
  gint input_height;
  gint pixel_stride;
  gint signed_stride;
  gsize row_bytes;
  gsize active_span;
  gsize tile_end;

  g_return_val_if_fail (layout != NULL, FALSE);
  g_return_val_if_fail (input_info != NULL, FALSE);
  g_return_val_if_fail (tile_info != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  gst_vvas_tile_composition_layout_ensure_debug_category ();

  tile = gst_vvas_tile_composition_layout_get_tile (layout, tile_id);
  if (!tile)
    return gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_ARGUMENT,
        "tile id %u is outside 0..%u", tile_id,
        GST_VVAS_TILE_COMPOSITION_TILE_COUNT - 1);

  input_format = GST_VIDEO_INFO_FORMAT (input_info);
  master_format = GST_VIDEO_INFO_FORMAT (&layout->master_info);
  if (input_format != master_format)
    return gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_UNSUPPORTED_FORMAT,
        "tile %u format %s does not match master format %s", tile_id,
        gst_video_format_to_string (input_format),
        gst_video_format_to_string (master_format));

  if (GST_VIDEO_INFO_N_PLANES (input_info) != 1)
    return gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_GEOMETRY,
        "tile %u requires one packed plane", tile_id);

  input_width = GST_VIDEO_INFO_WIDTH (input_info);
  input_height = GST_VIDEO_INFO_HEIGHT (input_info);
  if (input_width <= 0 || input_height <= 0 ||
      (guint) input_width != tile->width || (guint) input_height > tile->height)
    return gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_GEOMETRY,
        "tile %u slot is %ux%u but input is %dx%d", tile_id,
        tile->width, tile->height, input_width, input_height);

  pixel_stride = GST_VIDEO_INFO_COMP_PSTRIDE (input_info, 0);
  signed_stride = GST_VIDEO_INFO_PLANE_STRIDE (&layout->master_info, 0);
  if (pixel_stride != 3 || signed_stride <= 0)
    return gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_STRIDE,
        "tile %u has invalid pixel/master stride", tile_id);

  if (!gst_vvas_tile_composition_layout_checked_mul ((gsize) input_width,
          (gsize) pixel_stride, &row_bytes) ||
      !gst_vvas_tile_composition_layout_active_span (
          (guint) input_height, (gsize) signed_stride, row_bytes,
          &active_span) ||
      !gst_vvas_tile_composition_layout_checked_add (tile->plane_offset[0],
          active_span, &tile_end))
    return gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_OVERFLOW,
        "tile %u active-span calculation overflowed", tile_id);

  if (tile_end > layout->allocation_size)
    return gst_vvas_tile_composition_layout_set_error (error,
        GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_OUT_OF_BOUNDS,
        "tile %u active end %" G_GSIZE_FORMAT
        " exceeds allocation %" G_GSIZE_FORMAT,
        tile_id, tile_end, layout->allocation_size);

  *tile_info = *input_info;
  GST_VIDEO_INFO_PLANE_OFFSET (tile_info, 0) = tile->plane_offset[0];
  GST_VIDEO_INFO_PLANE_STRIDE (tile_info, 0) = signed_stride;
  GST_VIDEO_INFO_SIZE (tile_info) = layout->allocation_size;
  return TRUE;
}
