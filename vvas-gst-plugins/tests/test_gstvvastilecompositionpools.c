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

#include <gst/allocators/gstdmabuf.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/vvas/gstvvasallocator.h>
#include <gst/vvas/gstvvasmasterpool.h>
#include <gst/vvas/gstvvastilecompositionleasemeta.h>
#include <gst/vvas/gstvvasslavepool.h>

typedef struct _PoolFixture
{
  GstVvasTileCompositionLayout *layout;
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasMasterPool *master_pool;
  GstBufferPool *producer[GST_VVAS_TILE_COMPOSITION_TILE_COUNT];
} PoolFixture;

typedef struct _AcquireThread
{
  GstBufferPool *pool;
  GstFlowReturn flow;
  GstBuffer *buffer;
} AcquireThread;

static gboolean
reserve_master (const GstVvasTileCompositionIdentity * identity,
    gpointer user_data)
{
  return gst_vvas_master_pool_reserve_slot (GST_VVAS_MASTER_POOL (user_data),
      identity);
}

static void
unreserve_master (const GstVvasTileCompositionIdentity * identity,
    gpointer user_data)
{
  gst_vvas_master_pool_unreserve_slot (GST_VVAS_MASTER_POOL (user_data),
      identity);
}

static gpointer
acquire_thread (gpointer user_data)
{
  AcquireThread *data = user_data;

  data->flow = gst_buffer_pool_acquire_buffer (data->pool, &data->buffer, NULL);
  return NULL;
}

static void
assert_all_slots_available (PoolFixture * fixture)
{
  guint slot_id;

  for (slot_id = 0; slot_id < 4; slot_id++) {
    GstVvasTileCompositionSlotSnapshot coordinator_slot;
    GstVvasMasterSlotSnapshot master_slot;

    g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
        (fixture->coordinator, slot_id, &coordinator_slot));
    g_assert_cmpint (coordinator_slot.state, ==,
        GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
    g_assert_true (gst_vvas_master_pool_get_slot_snapshot (fixture->master_pool,
            slot_id, &master_slot));
    g_assert_true (master_slot.available);
    g_assert_false (master_slot.quarantined);
    g_assert_false (master_slot.reserved);
    g_assert_false (master_slot.transferred);
  }
}

static void
fixture_init (PoolFixture * fixture)
{
  GstVideoInfo master_info;
  GstAllocationParams params;
  GstAllocator *allocator;
  GstCaps *master_caps;
  GstStructure *config;
  const gchar *xclbin = g_getenv ("VVAS_TEST_XCLBIN");
  GError *error = NULL;
  guint tile_id;

  g_assert_nonnull (xclbin);
  gst_video_info_init (&master_info);
  g_assert_true (gst_video_info_set_format (&master_info,
          GST_VIDEO_FORMAT_BGR, 1920, 1080));
  GST_VIDEO_INFO_PLANE_STRIDE (&master_info, 0) = 5760;
  GST_VIDEO_INFO_SIZE (&master_info) = 6220800;
  fixture->layout =
      gst_vvas_tile_composition_layout_new (&master_info, 1, &error);
  g_assert_no_error (error);
  fixture->master_pool =
      GST_VVAS_MASTER_POOL (gst_vvas_master_pool_new (fixture->layout, 4));
  g_assert_nonnull (fixture->master_pool);

  allocator = gst_vvas_allocator_new (0, (gchar *) xclbin, TRUE, 0);
  g_assert_nonnull (allocator);
  gst_allocation_params_init (&params);
  master_caps = gst_video_info_to_caps (&master_info);
  config = gst_buffer_pool_get_config (GST_BUFFER_POOL (fixture->master_pool));
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  gst_buffer_pool_config_set_params (config, master_caps,
      GST_VIDEO_INFO_SIZE (&master_info), 4, 4);
  gst_buffer_pool_config_set_allocator (config, allocator, &params);
  g_assert_true (gst_buffer_pool_set_config (GST_BUFFER_POOL
          (fixture->master_pool), config));
  g_assert_true (gst_buffer_pool_set_active (GST_BUFFER_POOL
          (fixture->master_pool), TRUE));
  gst_object_unref (allocator);
  gst_caps_unref (master_caps);

  fixture->coordinator =
      gst_vvas_tile_composition_coordinator_new_full (fixture->layout, 4, 0xF,
      reserve_master, unreserve_master, gst_object_ref (fixture->master_pool),
      (GDestroyNotify) gst_object_unref, &error);
  g_assert_no_error (error);
  g_assert_nonnull (fixture->coordinator);

  for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++) {
    GstVideoInfo tile_info;
    GstCaps *tile_caps;

    gst_video_info_init (&tile_info);
    g_assert_true (gst_video_info_set_format (&tile_info,
            GST_VIDEO_FORMAT_BGR, 960, 540));
    tile_caps = gst_video_info_to_caps (&tile_info);
    fixture->producer[tile_id] =
        gst_vvas_slave_pool_new (fixture->coordinator, fixture->master_pool,
        fixture->layout, tile_id);
    g_assert_nonnull (fixture->producer[tile_id]);
    config = gst_buffer_pool_get_config (fixture->producer[tile_id]);
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
    gst_buffer_pool_config_set_params (config, tile_caps,
        GST_VIDEO_INFO_SIZE (&master_info), 0, 4);
    g_assert_true (gst_buffer_pool_set_config (fixture->producer[tile_id],
            config));
    g_assert_true (gst_buffer_pool_set_active (fixture->producer[tile_id],
            TRUE));
    gst_caps_unref (tile_caps);
  }
  assert_all_slots_available (fixture);
}

static void
fixture_clear (PoolFixture * fixture)
{
  guint tile_id;

  gst_vvas_tile_composition_coordinator_set_stopping (fixture->coordinator);
  for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++) {
    g_assert_true (gst_buffer_pool_set_active (fixture->producer[tile_id],
            FALSE));
    gst_object_unref (fixture->producer[tile_id]);
  }
  g_assert_true (gst_buffer_pool_set_active (GST_BUFFER_POOL
          (fixture->master_pool), FALSE));
  gst_vvas_tile_composition_coordinator_unref (fixture->coordinator);
  gst_object_unref (fixture->master_pool);
  gst_vvas_tile_composition_layout_unref (fixture->layout);
}

static void
test_pool_lifecycle (void)
{
  PoolFixture fixture = { 0 };
  GstBuffer *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] = { NULL };
  GstVvasTileCompositionLease *leases[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionLease *output = NULL;
  GstVvasTileCompositionIdentity output_identity;
  GstBuffer *master;
  guint tile_id;

  fixture_init (&fixture);
  for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++) {
    GstVvasTileCompositionLeaseMeta *lease_meta;
    GstVideoMeta *video_meta;
    const GstVvasTileCompositionTile *tile =
        gst_vvas_tile_composition_layout_get_tile (fixture.layout, tile_id);

    g_assert_cmpint (gst_buffer_pool_acquire_buffer (fixture.producer[tile_id],
            &tiles[tile_id], NULL), ==, GST_FLOW_OK);
    g_assert_true (tiles[tile_id]->pool == fixture.producer[tile_id]);
    g_assert_true (gst_is_dmabuf_memory (gst_buffer_peek_memory (tiles[tile_id],
                0)));
    video_meta = gst_buffer_get_video_meta (tiles[tile_id]);
    g_assert_nonnull (video_meta);
    g_assert_cmpuint (video_meta->offset[0], ==, tile->plane_offset[0]);
    lease_meta =
        gst_buffer_get_vvas_tile_composition_lease_meta (tiles[tile_id]);
    g_assert_nonnull (lease_meta);
    leases[tile_id] = lease_meta->lease;
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_tile_ready
        (leases[tile_id], tile_id, GST_SECOND / 30), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  }

  g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
      (fixture.coordinator, leases, GST_VVAS_TILE_COMPOSITION_TILE_COUNT,
          &output), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  gst_vvas_tile_composition_lease_get_identity (output, &output_identity);
  master = gst_vvas_master_pool_take_slot_buffer (fixture.master_pool,
      &output_identity);
  g_assert_nonnull (master);
  for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++)
    gst_buffer_unref (tiles[tile_id]);
  g_assert_true (gst_buffer_is_writable (master));
  g_assert_nonnull (gst_buffer_add_vvas_tile_composition_lease_meta (master,
          output));
  gst_vvas_tile_composition_lease_unref (output);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_complete_processing
      (gst_buffer_get_vvas_tile_composition_lease_meta (master)->lease), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  gst_buffer_unref (master);
  assert_all_slots_available (&fixture);

  for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++) {
    GstBuffer *wrapper;
    GstBuffer *deep;

    g_assert_cmpint (gst_buffer_pool_acquire_buffer (fixture.producer[tile_id],
            &wrapper, NULL), ==, GST_FLOW_OK);
    deep = gst_buffer_copy_deep (wrapper);
    g_assert_null (gst_buffer_get_vvas_tile_composition_lease_meta (deep));
    gst_buffer_unref (wrapper);
    gst_buffer_unref (deep);
  }
  assert_all_slots_available (&fixture);
  fixture_clear (&fixture);
}

static void
test_pool_flush_wakes_blocked_acquire (void)
{
  PoolFixture fixture = { 0 };
  GstBuffer *wrappers[4] = { NULL };
  GstVvasTileCompositionStats stats;
  GstBufferPoolAcquireParams params = { 0 };
  GstBuffer *other = NULL;
  AcquireThread data = { 0 };
  GThread *thread;
  guint i;

  fixture_init (&fixture);
  for (i = 0; i < 4; i++) {
    g_assert_cmpint (gst_buffer_pool_acquire_buffer (fixture.producer[0],
            &wrappers[i], NULL), ==, GST_FLOW_OK);
  }
  data.pool = fixture.producer[0];
  data.flow = GST_FLOW_ERROR;
  thread = g_thread_new ("pool-blocked-acquire", acquire_thread, &data);
  for (i = 0; i < 1000; i++) {
    gst_vvas_tile_composition_coordinator_get_stats (fixture.coordinator,
        &stats);
    if (stats.blocked_acquires)
      break;
    g_usleep (1000);
  }
  g_assert_cmpuint (stats.blocked_acquires, >, 0);
  gst_buffer_pool_set_flushing (fixture.producer[0], TRUE);
  /* Join before anything else can signal the coordinator, so only the
   * pool-flush broadcast can have released the blocked acquire. */
  g_thread_join (thread);
  g_assert_cmpint (data.flow, ==, GST_FLOW_FLUSHING);
  g_assert_null (data.buffer);
  /* Pool flushing is producer-local, so another tile must still be able to
   * join the epochs this one opened. */
  params.flags = GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT;
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (fixture.producer[1],
          &other, &params), ==, GST_FLOW_OK);
  g_assert_nonnull (other);
  gst_buffer_unref (other);
  other = NULL;
  gst_buffer_pool_set_flushing (GST_BUFFER_POOL (fixture.master_pool), TRUE);
  for (i = 0; i < 4; i++)
    gst_buffer_unref (wrappers[i]);
  gst_buffer_pool_set_flushing (GST_BUFFER_POOL (fixture.master_pool), FALSE);
  gst_buffer_pool_set_flushing (fixture.producer[0], FALSE);
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (fixture.producer[1],
          &other, NULL), ==, GST_FLOW_OK);
  gst_buffer_unref (other);
  assert_all_slots_available (&fixture);
  fixture_clear (&fixture);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/vvas/tile-composition-pools/lifecycle",
      test_pool_lifecycle);
  g_test_add_func ("/vvas/tile-composition-pools/flush-wakeup",
      test_pool_flush_wakes_blocked_acquire);
  return g_test_run ();
}
