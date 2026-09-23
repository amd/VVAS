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

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/vvas/gstvvastilecompositionlayout.h>

static void
init_video_info (GstVideoInfo * info, GstVideoFormat format, guint width,
    guint height, gint stride, gsize size)
{
  gboolean ret;

  gst_video_info_init (info);
  ret = gst_video_info_set_format (info, format, width, height);
  g_assert_true (ret);
  GST_VIDEO_INFO_PLANE_OFFSET (info, 0) = 0;
  GST_VIDEO_INFO_PLANE_STRIDE (info, 0) = stride;
  GST_VIDEO_INFO_SIZE (info) = size;
}

static void
test_valid_bgr_probed_layout (void)
{
  GstVideoInfo master_info;
  GstVideoInfo input_info;
  GstVideoInfo tile_info;
  GstVvasTileCompositionLayout *layout;
  GstVvasTileCompositionLayout *layout_ref;
  const GstVideoInfo *stored_master_info;
  const GstVvasTileCompositionTile *tile;
  const guint expected_x[] = { 0, 960, 0, 960 };
  const guint expected_y[] = { 0, 0, 540, 540 };
  const gsize expected_offset[] = { 0, 2880, 3179520, 3182400 };
  guint tile_id;
  GError *error = NULL;

  init_video_info (&master_info, GST_VIDEO_FORMAT_BGR, 1920, 1080, 5888,
      6359040);
  layout = gst_vvas_tile_composition_layout_new (&master_info, 7, &error);
  g_assert_no_error (error);
  g_assert_nonnull (layout);
  g_assert_cmpuint (gst_vvas_tile_composition_layout_get_version (layout), ==,
      7);
  g_assert_cmpuint (gst_vvas_tile_composition_layout_get_allocation_size
      (layout), ==, 6359040);

  stored_master_info =
      gst_vvas_tile_composition_layout_get_master_info (layout);
  g_assert_nonnull (stored_master_info);
  g_assert_cmpint (GST_VIDEO_INFO_FORMAT (stored_master_info), ==,
      GST_VIDEO_FORMAT_BGR);
  g_assert_cmpint (GST_VIDEO_INFO_PLANE_STRIDE (stored_master_info, 0), ==,
      5888);

  for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++) {
    tile = gst_vvas_tile_composition_layout_get_tile (layout, tile_id);
    g_assert_nonnull (tile);
    g_assert_cmpuint (tile->tile_id, ==, tile_id);
    g_assert_cmpuint (tile->row, ==, tile_id / 2);
    g_assert_cmpuint (tile->column, ==, tile_id % 2);
    g_assert_cmpuint (tile->x, ==, expected_x[tile_id]);
    g_assert_cmpuint (tile->y, ==, expected_y[tile_id]);
    g_assert_cmpuint (tile->width, ==, 960);
    g_assert_cmpuint (tile->height, ==, 540);
    g_assert_cmpuint (tile->plane_offset[0], ==, expected_offset[tile_id]);
  }
  g_assert_null (gst_vvas_tile_composition_layout_get_tile (layout, 4));

  init_video_info (&input_info, GST_VIDEO_FORMAT_BGR, 960, 536, 2880, 1543680);
  g_assert_true (gst_vvas_tile_composition_layout_prepare_tile_info (layout, 3,
          &input_info, &tile_info, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (GST_VIDEO_INFO_PLANE_OFFSET (&tile_info, 0), ==, 3182400);
  g_assert_cmpint (GST_VIDEO_INFO_PLANE_STRIDE (&tile_info, 0), ==, 5888);
  g_assert_cmpuint (GST_VIDEO_INFO_SIZE (&tile_info), ==, 6359040);
  g_assert_cmpuint (GST_VIDEO_INFO_WIDTH (&tile_info), ==, 960);
  g_assert_cmpuint (GST_VIDEO_INFO_HEIGHT (&tile_info), ==, 536);

  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (layout), ==, 1);
  layout_ref = gst_vvas_tile_composition_layout_ref (layout);
  g_assert_true (layout_ref == layout);
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (layout), ==, 2);
  gst_vvas_tile_composition_layout_unref (layout_ref);
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (layout), ==, 1);
  gst_vvas_tile_composition_layout_unref (layout);
}

static void
test_valid_rgb_packed_layout (void)
{
  GstVideoInfo master_info;
  GstVideoInfo input_info;
  GstVideoInfo tile_info;
  GstVvasTileCompositionLayout *layout;
  GError *error = NULL;

  init_video_info (&master_info, GST_VIDEO_FORMAT_RGB, 1920, 1080, 5760,
      6220800);
  layout = gst_vvas_tile_composition_layout_new (&master_info, 8, &error);
  g_assert_no_error (error);
  g_assert_nonnull (layout);

  init_video_info (&input_info, GST_VIDEO_FORMAT_RGB, 960, 540, 2880, 1555200);
  g_assert_true (gst_vvas_tile_composition_layout_prepare_tile_info (layout, 2,
          &input_info, &tile_info, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (GST_VIDEO_INFO_PLANE_OFFSET (&tile_info, 0), ==, 3110400);
  g_assert_cmpint (GST_VIDEO_INFO_PLANE_STRIDE (&tile_info, 0), ==, 5760);

  gst_vvas_tile_composition_layout_unref (layout);
}

static void
test_reject_invalid_master_layouts (void)
{
  GstVideoInfo info;
  GstVvasTileCompositionLayout *layout;
  GError *error = NULL;

  init_video_info (&info, GST_VIDEO_FORMAT_BGR, 1919, 1080, 5757, 6217560);
  layout = gst_vvas_tile_composition_layout_new (&info, 1, &error);
  g_assert_null (layout);
  g_assert_error (error, GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR,
      GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_GEOMETRY);
  g_clear_error (&error);

  init_video_info (&info, GST_VIDEO_FORMAT_NV12, 1920, 1080, 1920, 3110400);
  layout = gst_vvas_tile_composition_layout_new (&info, 1, &error);
  g_assert_null (layout);
  g_assert_error (error, GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR,
      GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_UNSUPPORTED_FORMAT);
  g_clear_error (&error);

  init_video_info (&info, GST_VIDEO_FORMAT_BGR, 1920, 1080, 5759, 6220800);
  layout = gst_vvas_tile_composition_layout_new (&info, 1, &error);
  g_assert_null (layout);
  g_assert_error (error, GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR,
      GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_STRIDE);
  g_clear_error (&error);

  init_video_info (&info, GST_VIDEO_FORMAT_BGR, 1920, 1080, 5888, 6358911);
  layout = gst_vvas_tile_composition_layout_new (&info, 1, &error);
  g_assert_null (layout);
  g_assert_error (error, GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR,
      GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_OUT_OF_BOUNDS);
  g_clear_error (&error);

  init_video_info (&info, GST_VIDEO_FORMAT_BGR, 1920, 1080, 5888, 6359040);
  GST_VIDEO_INFO_WIDTH (&info) = 0;
  layout = gst_vvas_tile_composition_layout_new (&info, 1, &error);
  g_assert_null (layout);
  g_assert_error (error, GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR,
      GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_GEOMETRY);
  g_clear_error (&error);

  init_video_info (&info, GST_VIDEO_FORMAT_BGR, 1920, 1080, 5888, 6359040);
  GST_VIDEO_INFO_HEIGHT (&info) = 0;
  layout = gst_vvas_tile_composition_layout_new (&info, 1, &error);
  g_assert_null (layout);
  g_assert_error (error, GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR,
      GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_GEOMETRY);
  g_clear_error (&error);

  init_video_info (&info, GST_VIDEO_FORMAT_BGR, 1920, 1080, 0, 6359040);
  layout = gst_vvas_tile_composition_layout_new (&info, 1, &error);
  g_assert_null (layout);
  g_assert_error (error, GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR,
      GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_STRIDE);
  g_clear_error (&error);

  init_video_info (&info, GST_VIDEO_FORMAT_BGR, 1920, 1080, -5888, 6359040);
  layout = gst_vvas_tile_composition_layout_new (&info, 1, &error);
  g_assert_null (layout);
  g_assert_error (error, GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR,
      GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_STRIDE);
  g_clear_error (&error);

  init_video_info (&info, GST_VIDEO_FORMAT_BGR, 1920, 1080, 5888, 6359040);
  GST_VIDEO_INFO_PLANE_OFFSET (&info, 0) = 32;
  layout = gst_vvas_tile_composition_layout_new (&info, 1, &error);
  g_assert_null (layout);
  g_assert_error (error, GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR,
      GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_GEOMETRY);
  g_clear_error (&error);
}

static void
test_reject_invalid_tile_layouts (void)
{
  GstVideoInfo master_info;
  GstVideoInfo input_info;
  GstVideoInfo tile_info;
  GstVvasTileCompositionLayout *layout;
  GError *error = NULL;

  init_video_info (&master_info, GST_VIDEO_FORMAT_BGR, 1920, 1080, 5888,
      6359040);
  layout = gst_vvas_tile_composition_layout_new (&master_info, 1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (layout);

  init_video_info (&input_info, GST_VIDEO_FORMAT_RGB, 960, 536, 2880, 1543680);
  g_assert_false (gst_vvas_tile_composition_layout_prepare_tile_info (layout, 0,
          &input_info, &tile_info, &error));
  g_assert_error (error, GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR,
      GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_UNSUPPORTED_FORMAT);
  g_clear_error (&error);

  init_video_info (&input_info, GST_VIDEO_FORMAT_BGR, 960, 541, 2880, 1558080);
  g_assert_false (gst_vvas_tile_composition_layout_prepare_tile_info (layout, 0,
          &input_info, &tile_info, &error));
  g_assert_error (error, GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR,
      GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_GEOMETRY);
  g_clear_error (&error);

  init_video_info (&input_info, GST_VIDEO_FORMAT_BGR, 959, 536, 2877, 1542072);
  g_assert_false (gst_vvas_tile_composition_layout_prepare_tile_info (layout, 0,
          &input_info, &tile_info, &error));
  g_assert_error (error, GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR,
      GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_GEOMETRY);
  g_clear_error (&error);

  init_video_info (&input_info, GST_VIDEO_FORMAT_BGR, 960, 540, 2880, 1555200);
  g_assert_true (gst_vvas_tile_composition_layout_prepare_tile_info (layout, 3,
          &input_info, &tile_info, &error));
  g_assert_no_error (error);

  init_video_info (&input_info, GST_VIDEO_FORMAT_BGR, 960, 1, 2880, 2880);
  g_assert_true (gst_vvas_tile_composition_layout_prepare_tile_info (layout, 3,
          &input_info, &tile_info, &error));
  g_assert_no_error (error);

  g_assert_false (gst_vvas_tile_composition_layout_prepare_tile_info (layout, 4,
          &input_info, &tile_info, &error));
  g_assert_error (error, GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR,
      GST_VVAS_TILE_COMPOSITION_LAYOUT_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  gst_vvas_tile_composition_layout_unref (layout);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/vvas/tile-composition-layout/valid-bgr-probed",
      test_valid_bgr_probed_layout);
  g_test_add_func ("/vvas/tile-composition-layout/valid-rgb-packed",
      test_valid_rgb_packed_layout);
  g_test_add_func ("/vvas/tile-composition-layout/reject-master",
      test_reject_invalid_master_layouts);
  g_test_add_func ("/vvas/tile-composition-layout/reject-tile",
      test_reject_invalid_tile_layouts);

  return g_test_run ();
}
