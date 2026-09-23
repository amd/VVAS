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
#include <gst/vvas/gstvvastilecompositionleasemeta.h>

static GstVvasTileCompositionCoordinator *
new_coordinator (void)
{
  GstVideoInfo info;
  GstVvasTileCompositionLayout *layout;
  GstVvasTileCompositionCoordinator *coordinator;
  GError *error = NULL;

  gst_video_info_init (&info);
  g_assert_true (gst_video_info_set_format (&info, GST_VIDEO_FORMAT_BGR,
          1920, 1080));
  GST_VIDEO_INFO_PLANE_STRIDE (&info, 0) = 5888;
  GST_VIDEO_INFO_SIZE (&info) = 6359040;
  layout = gst_vvas_tile_composition_layout_new (&info, 1, &error);
  g_assert_no_error (error);
  coordinator =
      gst_vvas_tile_composition_coordinator_new (layout, 2, 0xF, &error);
  gst_vvas_tile_composition_layout_unref (layout);
  g_assert_no_error (error);
  return coordinator;
}

static GstBuffer *
new_buffer_with_tile_lease (GstVvasTileCompositionCoordinator * coordinator,
    GstVvasTileCompositionLease ** result_lease)
{
  GstVvasTileCompositionLease *lease = NULL;
  GstBuffer *buffer;

  g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
      (coordinator, 0, FALSE, &lease), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  buffer = gst_buffer_new_allocate (NULL, 64, NULL);
  g_assert_nonnull (buffer);
  g_assert_nonnull (gst_buffer_add_vvas_tile_composition_lease_meta (buffer,
          lease));
  if (result_lease)
    *result_lease = lease;
  else
    gst_vvas_tile_composition_lease_unref (lease);
  return buffer;
}

static void
test_shallow_copy_preserves_lease (void)
{
  GstVvasTileCompositionCoordinator *coordinator = new_coordinator ();
  GstVvasTileCompositionLease *lease;
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionSlotSnapshot snapshot;
  GstBuffer *buffer = new_buffer_with_tile_lease (coordinator, &lease);
  GstBuffer *copy = gst_buffer_copy (buffer);
  GstVvasTileCompositionLeaseMeta *copy_meta;

  gst_vvas_tile_composition_lease_get_identity (lease, &identity);
  gst_vvas_tile_composition_lease_unref (lease);
  copy_meta = gst_buffer_get_vvas_tile_composition_lease_meta (copy);
  g_assert_nonnull (copy_meta);
  gst_buffer_unref (buffer);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_WRITING);
  gst_buffer_unref (copy);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  gst_vvas_tile_composition_coordinator_unref (coordinator);
}

static void
test_deep_copy_drops_lease (void)
{
  GstVvasTileCompositionCoordinator *coordinator = new_coordinator ();
  GstBuffer *buffer = new_buffer_with_tile_lease (coordinator, NULL);
  GstBuffer *copy = gst_buffer_copy_deep (buffer);

  g_assert_null (gst_buffer_get_vvas_tile_composition_lease_meta (copy));
  gst_buffer_unref (buffer);
  gst_buffer_unref (copy);
  gst_vvas_tile_composition_coordinator_unref (coordinator);
}

static void
test_region_copy_drops_lease (void)
{
  GstVvasTileCompositionCoordinator *coordinator = new_coordinator ();
  GstBuffer *buffer = new_buffer_with_tile_lease (coordinator, NULL);
  GstBuffer *copy = gst_buffer_copy_region (buffer, GST_BUFFER_COPY_ALL,
      4, 16);

  g_assert_null (gst_buffer_get_vvas_tile_composition_lease_meta (copy));
  gst_buffer_unref (buffer);
  gst_buffer_unref (copy);
  gst_vvas_tile_composition_coordinator_unref (coordinator);
}

static void
test_meta_only_copy_drops_lease (void)
{
  GstVvasTileCompositionCoordinator *coordinator = new_coordinator ();
  GstBuffer *buffer = new_buffer_with_tile_lease (coordinator, NULL);
  GstBuffer *dest = gst_buffer_new_allocate (NULL, 64, NULL);

  g_assert_true (gst_buffer_copy_into (dest, buffer, GST_BUFFER_COPY_META,
          0, -1));
  g_assert_null (gst_buffer_get_vvas_tile_composition_lease_meta (dest));
  gst_buffer_unref (buffer);
  gst_buffer_unref (dest);
  gst_vvas_tile_composition_coordinator_unref (coordinator);
}

static void
test_output_copy_retires_on_final_meta (void)
{
  GstVvasTileCompositionCoordinator *coordinator = new_coordinator ();
  GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionLease *output = NULL;
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionSlotSnapshot snapshot;
  GstBuffer *buffer;
  GstBuffer *copy;
  guint i;

  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    gst_vvas_tile_composition_coordinator_acquire_tile (coordinator, i, FALSE,
        &tiles[i]);
    gst_vvas_tile_composition_coordinator_mark_tile_ready (tiles[i], i,
        GST_SECOND / 30);
  }
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
      (coordinator, tiles, GST_VVAS_TILE_COMPOSITION_TILE_COUNT, &output), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++)
    gst_vvas_tile_composition_lease_unref (tiles[i]);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_complete_processing
      (output), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  gst_vvas_tile_composition_lease_get_identity (output, &identity);

  buffer = gst_buffer_new_allocate (NULL, 64, NULL);
  gst_buffer_add_vvas_tile_composition_lease_meta (buffer, output);
  gst_vvas_tile_composition_lease_unref (output);
  copy = gst_buffer_copy (buffer);
  gst_buffer_unref (buffer);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==,
      GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM);
  gst_buffer_unref (copy);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  gst_vvas_tile_composition_coordinator_unref (coordinator);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/vvas/tile-composition-lease-meta/shallow-copy",
      test_shallow_copy_preserves_lease);
  g_test_add_func ("/vvas/tile-composition-lease-meta/deep-copy",
      test_deep_copy_drops_lease);
  g_test_add_func ("/vvas/tile-composition-lease-meta/region-copy",
      test_region_copy_drops_lease);
  g_test_add_func ("/vvas/tile-composition-lease-meta/meta-only-copy",
      test_meta_only_copy_drops_lease);
  g_test_add_func ("/vvas/tile-composition-lease-meta/output-final-meta",
      test_output_copy_retires_on_final_meta);
  return g_test_run ();
}
