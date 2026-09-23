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
#include <gst/vvas/gstvvastilecompositioncoordinator.h>

typedef struct _BlockedAcquireData
{
  GstVvasTileCompositionCoordinator *coordinator;
  guint tile_id;
  GstVvasTileCompositionResult result;
  GstVvasTileCompositionLease *lease;
} BlockedAcquireData;

typedef struct _ReservationGate
{
  GMutex lock;
  GCond condition;
  gboolean entered;
  gboolean allow;
  gboolean succeed;
  guint reserve_count;
  guint unreserve_count;
} ReservationGate;

typedef struct _EpochRecord
{
  GstVvasTileCompositionLease *leases[GST_VVAS_TILE_COMPOSITION_TILE_COUNT];
  guint mask;
} EpochRecord;

typedef struct _ConcurrentRun
{
  GstVvasTileCompositionCoordinator *coordinator;
  GMutex lock;
  GCond condition;
  GHashTable *records;
  guint epochs;
  gboolean failed;
} ConcurrentRun;

typedef struct _ProducerData
{
  ConcurrentRun *run;
  guint tile_id;
  guint32 seed;
} ProducerData;

typedef struct _FaultStorage
{
  GMutex lock;
  GCond condition;
  gboolean fail_next;
  gboolean block_next;
  gboolean entered;
  gboolean release;
  guint reserve_attempts;
  guint reserve_count;
  guint unreserve_count;
} FaultStorage;

static void
fail_on_composition_error (GstDebugCategory * category, GstDebugLevel level,
    const gchar * file, const gchar * function, gint line, GObject * object,
    GstDebugMessage * message, gpointer user_data)
{
  (void) file;
  (void) function;
  (void) line;
  (void) object;
  (void) user_data;

  if (level == GST_LEVEL_ERROR &&
      g_strcmp0 (gst_debug_category_get_name (category),
          "vvas_tile_composition") == 0) {
    g_printerr ("unexpected vvas_tile_composition error: %s\n",
        gst_debug_message_get (message));
    g_test_fail ();
  }
}

static gboolean
reserve_slot_with_gate (const GstVvasTileCompositionIdentity * identity,
    gpointer user_data)
{
  ReservationGate *gate = user_data;

  (void) identity;
  g_mutex_lock (&gate->lock);
  gate->entered = TRUE;
  gate->reserve_count++;
  g_cond_broadcast (&gate->condition);
  while (!gate->allow)
    g_cond_wait (&gate->condition, &gate->lock);
  g_mutex_unlock (&gate->lock);
  return gate->succeed;
}

static void
unreserve_slot_with_gate (const GstVvasTileCompositionIdentity * identity,
    gpointer user_data)
{
  ReservationGate *gate = user_data;

  (void) identity;
  g_mutex_lock (&gate->lock);
  gate->unreserve_count++;
  g_mutex_unlock (&gate->lock);
}

static void
reservation_gate_free (gpointer user_data)
{
  ReservationGate *gate = user_data;

  g_cond_clear (&gate->condition);
  g_mutex_clear (&gate->lock);
  g_free (gate);
}

static gboolean
reserve_fault_storage (const GstVvasTileCompositionIdentity * identity,
    gpointer user_data)
{
  FaultStorage *storage = user_data;
  gboolean succeed = TRUE;

  (void) identity;
  g_mutex_lock (&storage->lock);
  storage->reserve_attempts++;
  if (storage->fail_next) {
    storage->fail_next = FALSE;
    succeed = FALSE;
  } else {
    storage->reserve_count++;
  }
  if (succeed && storage->block_next) {
    storage->entered = TRUE;
    g_cond_broadcast (&storage->condition);
    while (!storage->release)
      g_cond_wait (&storage->condition, &storage->lock);
    storage->block_next = FALSE;
  }
  g_mutex_unlock (&storage->lock);
  return succeed;
}

static void
unreserve_fault_storage (const GstVvasTileCompositionIdentity * identity,
    gpointer user_data)
{
  FaultStorage *storage = user_data;

  (void) identity;
  g_mutex_lock (&storage->lock);
  storage->unreserve_count++;
  g_mutex_unlock (&storage->lock);
}

static void
fault_storage_free (gpointer user_data)
{
  FaultStorage *storage = user_data;

  g_cond_clear (&storage->condition);
  g_mutex_clear (&storage->lock);
  g_free (storage);
}

static void
coordinator_finalized (gpointer user_data, GstMiniObject * object)
{
  gboolean *finalized = user_data;

  (void) object;
  *finalized = TRUE;
}

static void
assert_final_coordinator_ref_and_unref (GstVvasTileCompositionCoordinator *
    coordinator)
{
  gboolean finalized = FALSE;

  gst_mini_object_weak_ref (GST_MINI_OBJECT_CAST (coordinator),
      coordinator_finalized, &finalized);
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (coordinator), ==, 1);
  gst_vvas_tile_composition_coordinator_unref (coordinator);
  g_assert_true (finalized);
}

static GstVvasTileCompositionLayout *
new_layout (void)
{
  GstVideoInfo master_info;
  GstVvasTileCompositionLayout *layout;
  GError *error = NULL;
  gboolean ret;

  gst_video_info_init (&master_info);
  ret = gst_video_info_set_format (&master_info, GST_VIDEO_FORMAT_BGR,
      1920, 1080);
  g_assert_true (ret);
  GST_VIDEO_INFO_PLANE_STRIDE (&master_info, 0) = 5888;
  GST_VIDEO_INFO_SIZE (&master_info) = 6359040;

  layout = gst_vvas_tile_composition_layout_new (&master_info, 1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (layout);
  return layout;
}

static GstVvasTileCompositionCoordinator *
new_coordinator (guint max_slots, guint required_mask,
    GstVvasTileCompositionLayout ** result_layout)
{
  GstVvasTileCompositionLayout *layout;
  GstVvasTileCompositionCoordinator *coordinator;
  GError *error = NULL;

  layout = new_layout ();
  coordinator = gst_vvas_tile_composition_coordinator_new (layout, max_slots,
      required_mask, &error);
  g_assert_no_error (error);
  g_assert_nonnull (coordinator);

  if (result_layout)
    *result_layout = layout;
  else
    gst_vvas_tile_composition_layout_unref (layout);
  return coordinator;
}

static GstVvasTileCompositionCoordinator *
new_storage_coordinator (guint max_slots, ReservationGate ** result_gate)
{
  GstVvasTileCompositionLayout *layout;
  GstVvasTileCompositionCoordinator *coordinator;
  ReservationGate *gate;
  GError *error = NULL;

  layout = new_layout ();
  gate = g_new0 (ReservationGate, 1);
  g_mutex_init (&gate->lock);
  g_cond_init (&gate->condition);
  gate->allow = TRUE;
  gate->succeed = TRUE;
  coordinator =
      gst_vvas_tile_composition_coordinator_new_full (layout, max_slots, 0xF,
      reserve_slot_with_gate, unreserve_slot_with_gate, gate,
      reservation_gate_free, &error);
  gst_vvas_tile_composition_layout_unref (layout);
  g_assert_no_error (error);
  g_assert_nonnull (coordinator);
  *result_gate = gate;
  return coordinator;
}

static GstVvasTileCompositionCoordinator *
new_fault_coordinator (guint max_slots, FaultStorage ** result_storage)
{
  GstVvasTileCompositionLayout *layout;
  GstVvasTileCompositionCoordinator *coordinator;
  FaultStorage *storage;
  GError *error = NULL;

  layout = new_layout ();
  storage = g_new0 (FaultStorage, 1);
  g_mutex_init (&storage->lock);
  g_cond_init (&storage->condition);
  coordinator =
      gst_vvas_tile_composition_coordinator_new_full (layout, max_slots, 0xF,
      reserve_fault_storage, unreserve_fault_storage, storage,
      fault_storage_free, &error);
  gst_vvas_tile_composition_layout_unref (layout);
  g_assert_no_error (error);
  g_assert_nonnull (coordinator);
  *result_storage = storage;
  return coordinator;
}

static void
assert_same_epoch (GstVvasTileCompositionLease * const *leases, guint n_leases)
{
  GstVvasTileCompositionIdentity first;
  guint i;

  g_assert_cmpuint (n_leases, >, 0);
  gst_vvas_tile_composition_lease_get_identity (leases[0], &first);
  for (i = 1; i < n_leases; i++) {
    GstVvasTileCompositionIdentity current;

    gst_vvas_tile_composition_lease_get_identity (leases[i], &current);
    g_assert_cmpuint (current.slot_id, ==, first.slot_id);
    g_assert_cmpuint (current.epoch, ==, first.epoch);
    g_assert_cmpuint (current.layout_version, ==, first.layout_version);
  }
}

static void
test_normal_epoch_lifecycle (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLayout *layout;
  GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionLease *output = NULL;
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionSlotSnapshot snapshot;
  GstVvasTileCompositionStats stats;
  guint i;

  coordinator = new_coordinator (2, 0xF, &layout);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
        (coordinator, i, FALSE, &tiles[i]), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  }
  assert_same_epoch (tiles, GST_VVAS_TILE_COMPOSITION_TILE_COUNT);
  gst_vvas_tile_composition_lease_get_identity (tiles[0], &identity);

  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_tile_ready
        (tiles[i], i * GST_MSECOND, GST_SECOND / 30), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  }
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_READY);
  g_assert_cmpuint (snapshot.ready_mask, ==, 0xF);

  g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_tile_ready (tiles
          [0], 0, GST_SECOND / 30), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
      (coordinator, tiles, GST_VVAS_TILE_COMPOSITION_TILE_COUNT, &output), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  g_assert_nonnull (output);

  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++)
    gst_vvas_tile_composition_lease_unref (tiles[i]);

  g_assert_cmpint (gst_vvas_tile_composition_coordinator_complete_processing
      (output), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  gst_vvas_tile_composition_lease_get_identity (output, &identity);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_output_returned
      (coordinator, &identity), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  gst_vvas_tile_composition_lease_unref (output);

  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  gst_vvas_tile_composition_coordinator_get_stats (coordinator, &stats);
  g_assert_cmpuint (stats.epochs_opened, ==, 1);
  g_assert_cmpuint (stats.epochs_ready, ==, 1);
  g_assert_cmpuint (stats.epochs_published, ==, 1);
  g_assert_cmpuint (stats.duplicate_callbacks, ==, 1);

  gst_vvas_tile_composition_coordinator_unref (coordinator);
  gst_vvas_tile_composition_layout_unref (layout);
}

static void
test_fast_producer_oldest_assignment (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *tile0[3] = { NULL };
  GstVvasTileCompositionLease *tile1[3] = { NULL };
  GstVvasTileCompositionLease *extra = NULL;
  GstVvasTileCompositionIdentity id0;
  GstVvasTileCompositionIdentity id1;
  guint i;

  coordinator = new_coordinator (3, 0xF, NULL);
  for (i = 0; i < 3; i++) {
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
        (coordinator, 0, FALSE, &tile0[i]), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  }
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
      (coordinator, 0, TRUE, &extra), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_NO_SLOT);
  g_assert_null (extra);

  for (i = 0; i < 3; i++) {
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
        (coordinator, 1, FALSE, &tile1[i]), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
    gst_vvas_tile_composition_lease_get_identity (tile0[i], &id0);
    gst_vvas_tile_composition_lease_get_identity (tile1[i], &id1);
    g_assert_cmpuint (id1.epoch, ==, id0.epoch);
    g_assert_cmpuint (id1.slot_id, ==, id0.slot_id);
  }

  for (i = 0; i < 3; i++) {
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_cancel_tile (tile0
            [i], GST_VVAS_TILE_COMPOSITION_ABORT_EXPLICIT), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
    gst_vvas_tile_composition_lease_unref (tile0[i]);
    gst_vvas_tile_composition_lease_unref (tile1[i]);
  }
  gst_vvas_tile_composition_coordinator_unref (coordinator);
}

static void
test_abort_epoch_writing_lifecycle (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *tile = NULL;
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionSlotSnapshot snapshot;
  GstVvasTileCompositionStats stats;
  ReservationGate *gate;

  coordinator = new_storage_coordinator (1, &gate);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
      (coordinator, 0, FALSE, &tile), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  gst_vvas_tile_composition_lease_get_identity (tile, &identity);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_true (snapshot.storage_reserved);

  g_assert_cmpint (gst_vvas_tile_composition_coordinator_abort_epoch
      (coordinator, &identity, GST_VVAS_TILE_COMPOSITION_ABORT_EXPLICIT), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_abort_epoch
      (coordinator, &identity, GST_VVAS_TILE_COMPOSITION_ABORT_EXPLICIT), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_tile_ready (tile,
          0, GST_SECOND / 30), ==, GST_VVAS_TILE_COMPOSITION_RESULT_ABORTED);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING);
  g_assert_cmpint (snapshot.abort_reason, ==,
      GST_VVAS_TILE_COMPOSITION_ABORT_EXPLICIT);
  g_assert_true (snapshot.storage_reserved);

  gst_vvas_tile_composition_lease_unref (tile);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  g_assert_false (snapshot.storage_reserved);
  g_assert_cmpuint (gate->unreserve_count, ==, 1);
  gst_vvas_tile_composition_coordinator_get_stats (coordinator, &stats);
  g_assert_cmpuint (stats.epochs_aborted, ==, 1);
  g_assert_cmpuint (stats.duplicate_callbacks, ==, 1);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_abort_epoch
      (coordinator, &identity, GST_VVAS_TILE_COMPOSITION_ABORT_EXPLICIT), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_STALE);
  assert_final_coordinator_ref_and_unref (coordinator);
}

static void
test_abort_epoch_ready_lifecycle (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionLease *output = NULL;
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionSlotSnapshot snapshot;
  ReservationGate *gate;
  guint i;

  coordinator = new_storage_coordinator (1, &gate);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
        (coordinator, i, FALSE, &tiles[i]), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_tile_ready
        (tiles[i], i, GST_SECOND / 30), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  }
  gst_vvas_tile_composition_lease_get_identity (tiles[0], &identity);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_abort_epoch
      (coordinator, &identity, GST_VVAS_TILE_COMPOSITION_ABORT_EXPLICIT), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
      (coordinator, tiles, GST_VVAS_TILE_COMPOSITION_TILE_COUNT, &output), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_ABORTED);
  g_assert_null (output);

  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++)
    gst_vvas_tile_composition_lease_unref (tiles[i]);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  g_assert_false (snapshot.storage_reserved);
  g_assert_cmpuint (gate->unreserve_count, ==, 1);
  assert_final_coordinator_ref_and_unref (coordinator);
}

static void
test_abort_epoch_processing_lifecycle (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionLease *output = NULL;
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionSlotSnapshot snapshot;
  ReservationGate *gate;
  guint i;

  coordinator = new_storage_coordinator (1, &gate);
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
  gst_vvas_tile_composition_lease_get_identity (output, &identity);

  g_assert_cmpint (gst_vvas_tile_composition_coordinator_abort_epoch
      (coordinator, &identity,
          GST_VVAS_TILE_COMPOSITION_ABORT_PROCESSING_FAILED), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING);
  g_assert_true (snapshot.storage_reserved);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_complete_processing
      (output), ==, GST_VVAS_TILE_COMPOSITION_RESULT_ABORTED);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_output_returned
      (coordinator, &identity), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  gst_vvas_tile_composition_lease_unref (output);

  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  g_assert_false (snapshot.storage_reserved);
  g_assert_cmpuint (gate->unreserve_count, ==, 1);
  assert_final_coordinator_ref_and_unref (coordinator);
}

static void
test_abort_epoch_rejects_downstream (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionLease *output = NULL;
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionSlotSnapshot snapshot;
  ReservationGate *gate;
  guint i;

  coordinator = new_storage_coordinator (1, &gate);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    gst_vvas_tile_composition_coordinator_acquire_tile (coordinator, i, FALSE,
        &tiles[i]);
    gst_vvas_tile_composition_coordinator_mark_tile_ready (tiles[i], i,
        GST_SECOND / 30);
  }
  gst_vvas_tile_composition_coordinator_begin_processing (coordinator, tiles,
      GST_VVAS_TILE_COMPOSITION_TILE_COUNT, &output);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++)
    gst_vvas_tile_composition_lease_unref (tiles[i]);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_complete_processing
      (output), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  gst_vvas_tile_composition_lease_get_identity (output, &identity);

  g_assert_cmpint (gst_vvas_tile_composition_coordinator_abort_epoch
      (coordinator, &identity, GST_VVAS_TILE_COMPOSITION_ABORT_EXPLICIT), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==,
      GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM);
  g_assert_true (snapshot.storage_reserved);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_output_returned
      (coordinator, &identity), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  gst_vvas_tile_composition_lease_unref (output);

  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  g_assert_false (snapshot.storage_reserved);
  g_assert_cmpuint (gate->unreserve_count, ==, 1);
  assert_final_coordinator_ref_and_unref (coordinator);
}

static void
test_output_lease_drop_unreserves (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionLease *output = NULL;
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionSlotSnapshot snapshot;
  ReservationGate *gate;
  guint i;

  coordinator = new_storage_coordinator (1, &gate);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    gst_vvas_tile_composition_coordinator_acquire_tile (coordinator, i, FALSE,
        &tiles[i]);
    gst_vvas_tile_composition_coordinator_mark_tile_ready (tiles[i], i,
        GST_SECOND / 30);
  }
  gst_vvas_tile_composition_coordinator_begin_processing (coordinator, tiles,
      GST_VVAS_TILE_COMPOSITION_TILE_COUNT, &output);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++)
    gst_vvas_tile_composition_lease_unref (tiles[i]);
  gst_vvas_tile_composition_coordinator_complete_processing (output);
  gst_vvas_tile_composition_lease_get_identity (output, &identity);

  gst_vvas_tile_composition_lease_unref (output);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  g_assert_false (snapshot.output_returned);
  g_assert_false (snapshot.storage_reserved);
  g_assert_cmpuint (gate->unreserve_count, ==, 1);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_output_returned
      (coordinator, &identity), ==, GST_VVAS_TILE_COMPOSITION_RESULT_STALE);
  assert_final_coordinator_ref_and_unref (coordinator);
}

static void
test_stopping_retires_unreturned_output (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionLease *output = NULL;
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionSlotSnapshot snapshot;
  ReservationGate *gate;
  guint i;

  coordinator = new_storage_coordinator (1, &gate);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    gst_vvas_tile_composition_coordinator_acquire_tile (coordinator, i, FALSE,
        &tiles[i]);
    gst_vvas_tile_composition_coordinator_mark_tile_ready (tiles[i], i,
        GST_SECOND / 30);
  }
  gst_vvas_tile_composition_coordinator_begin_processing (coordinator, tiles,
      GST_VVAS_TILE_COMPOSITION_TILE_COUNT, &output);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++)
    gst_vvas_tile_composition_lease_unref (tiles[i]);
  gst_vvas_tile_composition_coordinator_complete_processing (output);
  gst_vvas_tile_composition_lease_get_identity (output, &identity);
  gst_vvas_tile_composition_coordinator_set_stopping (coordinator);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_RETIRING);
  g_assert_true (snapshot.output_returned);
  g_assert_true (snapshot.storage_reserved);
  g_assert_cmpuint (snapshot.active_output_leases, ==, 1);
  gst_vvas_tile_composition_lease_unref (output);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  g_assert_cmpuint (gate->unreserve_count, ==, 1);
  assert_final_coordinator_ref_and_unref (coordinator);
}

static void
test_drop_aborts_whole_epoch (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionSlotSnapshot snapshot;
  guint i;

  coordinator = new_coordinator (1, 0xF, NULL);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
        (coordinator, i, FALSE, &tiles[i]), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  }
  gst_vvas_tile_composition_lease_get_identity (tiles[0], &identity);
  for (i = 0; i < 3; i++) {
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_tile_ready
        (tiles[i], i, GST_SECOND / 30), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  }

  gst_vvas_tile_composition_lease_unref (tiles[3]);
  tiles[3] = NULL;
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING);
  g_assert_cmpuint (snapshot.cancelled_mask, ==, 1U << 3);

  for (i = 0; i < 3; i++) {
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_tile_ready
        (tiles[i], i, GST_SECOND / 30), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_ABORTED);
    gst_vvas_tile_composition_lease_unref (tiles[i]);
  }
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  gst_vvas_tile_composition_coordinator_unref (coordinator);
}

static void
test_ready_but_unconsumed_drop_aborts (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionSlotSnapshot snapshot;
  guint i;

  coordinator = new_coordinator (1, 0xF, NULL);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    gst_vvas_tile_composition_coordinator_acquire_tile (coordinator, i, FALSE,
        &tiles[i]);
    gst_vvas_tile_composition_coordinator_mark_tile_ready (tiles[i], i,
        GST_SECOND / 30);
  }
  gst_vvas_tile_composition_lease_get_identity (tiles[0], &identity);
  gst_vvas_tile_composition_lease_unref (tiles[3]);
  tiles[3] = NULL;
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING);

  for (i = 0; i < 3; i++)
    gst_vvas_tile_composition_lease_unref (tiles[i]);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, identity.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  gst_vvas_tile_composition_coordinator_unref (coordinator);
}

static void
test_begin_processing_rejects_invalid_sets (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionCoordinator *other_coordinator;
  GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionLease *other = NULL;
  GstVvasTileCompositionLease *invalid[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionLease *output = NULL;
  guint i;

  coordinator = new_coordinator (2, 0xF, NULL);
  other_coordinator = new_coordinator (1, 0xF, NULL);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    gst_vvas_tile_composition_coordinator_acquire_tile (coordinator, i, FALSE,
        &tiles[i]);
    gst_vvas_tile_composition_coordinator_mark_tile_ready (tiles[i], i,
        GST_SECOND / 30);
  }
  gst_vvas_tile_composition_coordinator_acquire_tile (other_coordinator, 0,
      FALSE, &other);

  g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
      (coordinator, tiles, 3, &output), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  g_assert_null (output);

  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++)
    invalid[i] = tiles[i];
  invalid[3] = tiles[2];
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
      (coordinator, invalid, GST_VVAS_TILE_COMPOSITION_TILE_COUNT, &output), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE);
  g_assert_null (output);

  invalid[3] = other;
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
      (coordinator, invalid, GST_VVAS_TILE_COMPOSITION_TILE_COUNT, &output), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  g_assert_null (output);

  g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
      (coordinator, GST_VVAS_TILE_COMPOSITION_TILE_COUNT, TRUE, &output), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  g_assert_null (output);

  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++)
    gst_vvas_tile_composition_lease_unref (tiles[i]);
  gst_vvas_tile_composition_lease_unref (other);
  gst_vvas_tile_composition_coordinator_unref (other_coordinator);
  gst_vvas_tile_composition_coordinator_unref (coordinator);
}

static void
test_output_release_and_stale_identity (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionLease *output = NULL;
  GstVvasTileCompositionLease *next = NULL;
  GstVvasTileCompositionIdentity old_output;
  GstVvasTileCompositionIdentity next_identity;
  GstVvasTileCompositionSlotSnapshot snapshot;
  guint i;

  coordinator = new_coordinator (1, 0xF, NULL);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
        (coordinator, i, FALSE, &tiles[i]), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_tile_ready
        (tiles[i], i, GST_SECOND / 30), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  }
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
      (coordinator, tiles, GST_VVAS_TILE_COMPOSITION_TILE_COUNT, &output), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++)
    gst_vvas_tile_composition_lease_unref (tiles[i]);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_complete_processing
      (output), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  gst_vvas_tile_composition_lease_get_identity (output, &old_output);

  gst_vvas_tile_composition_lease_unref (output);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, old_output.slot_id, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  g_assert_false (snapshot.output_returned);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_output_returned
      (coordinator, &old_output), ==, GST_VVAS_TILE_COMPOSITION_RESULT_STALE);

  g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
      (coordinator, 0, FALSE, &next), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  gst_vvas_tile_composition_lease_get_identity (next, &next_identity);
  g_assert_cmpuint (next_identity.epoch, >, old_output.epoch);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_output_returned
      (coordinator, &old_output), ==, GST_VVAS_TILE_COMPOSITION_RESULT_STALE);
  gst_vvas_tile_composition_lease_unref (next);
  gst_vvas_tile_composition_coordinator_unref (coordinator);
}

static gpointer
blocked_acquire_thread (gpointer user_data)
{
  BlockedAcquireData *data = user_data;

  data->result =
      gst_vvas_tile_composition_coordinator_acquire_tile (data->coordinator,
      data->tile_id, FALSE, &data->lease);
  return NULL;
}

static void
epoch_record_free (gpointer user_data)
{
  EpochRecord *record = user_data;
  guint tile_id;

  for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++) {
    if (record->leases[tile_id])
      gst_vvas_tile_composition_lease_unref (record->leases[tile_id]);
  }
  g_free (record);
}

static gpointer
concurrent_producer_thread (gpointer user_data)
{
  ProducerData *producer = user_data;
  ConcurrentRun *run = producer->run;
  GRand *random = g_rand_new_with_seed (producer->seed);
  guint produced;

  for (produced = 0; produced < run->epochs; produced++) {
    GstVvasTileCompositionLease *lease = NULL;
    GstVvasTileCompositionIdentity identity;
    GstVvasTileCompositionResult result;
    EpochRecord *record;
    guint64 lookup_epoch;

    result =
        gst_vvas_tile_composition_coordinator_acquire_tile (run->coordinator,
        producer->tile_id, FALSE, &lease);
    if (result != GST_VVAS_TILE_COMPOSITION_RESULT_OK)
      goto failed;
    g_usleep (g_rand_int_range (random, 0, 2000));
    result = gst_vvas_tile_composition_coordinator_mark_tile_ready (lease,
        produced * GST_SECOND / 30, GST_SECOND / 30);
    if (result != GST_VVAS_TILE_COMPOSITION_RESULT_OK) {
      gst_vvas_tile_composition_lease_unref (lease);
      goto failed;
    }

    gst_vvas_tile_composition_lease_get_identity (lease, &identity);
    lookup_epoch = identity.epoch;
    g_mutex_lock (&run->lock);
    record = g_hash_table_lookup (run->records, &lookup_epoch);
    if (!record) {
      guint64 *stored_epoch = g_new (guint64, 1);

      *stored_epoch = identity.epoch;
      record = g_new0 (EpochRecord, 1);
      g_hash_table_insert (run->records, stored_epoch, record);
    }
    if (record->leases[producer->tile_id] ||
        (record->mask & (1U << producer->tile_id))) {
      g_mutex_unlock (&run->lock);
      gst_vvas_tile_composition_lease_unref (lease);
      goto failed;
    }
    record->leases[producer->tile_id] = lease;
    record->mask |= 1U << producer->tile_id;
    g_cond_broadcast (&run->condition);
    g_mutex_unlock (&run->lock);
  }

  g_rand_free (random);
  return NULL;

failed:
  g_mutex_lock (&run->lock);
  run->failed = TRUE;
  g_cond_broadcast (&run->condition);
  g_mutex_unlock (&run->lock);
  g_rand_free (random);
  return NULL;
}

static void
test_flush_wakes_blocked_producer (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *first = NULL;
  GstVvasTileCompositionStats stats;
  BlockedAcquireData data = { 0 };
  GThread *thread;
  guint attempts;

  coordinator = new_coordinator (1, 0xF, NULL);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
      (coordinator, 0, FALSE, &first), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);

  data.coordinator = coordinator;
  data.tile_id = 0;
  data.result = GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
  thread = g_thread_new ("blocked-acquire", blocked_acquire_thread, &data);

  for (attempts = 0; attempts < 1000; attempts++) {
    gst_vvas_tile_composition_coordinator_get_stats (coordinator, &stats);
    if (stats.blocked_acquires)
      break;
    g_usleep (1000);
  }
  g_assert_cmpuint (stats.blocked_acquires, >, 0);
  gst_vvas_tile_composition_coordinator_set_flushing (coordinator, TRUE);
  g_thread_join (thread);
  g_assert_cmpint (data.result, ==, GST_VVAS_TILE_COMPOSITION_RESULT_FLUSHING);
  g_assert_null (data.lease);

  gst_vvas_tile_composition_lease_unref (first);
  gst_vvas_tile_composition_coordinator_set_flushing (coordinator, FALSE);
  assert_final_coordinator_ref_and_unref (coordinator);
}

static void
test_reservation_barrier_and_rollback (void)
{
  GstVvasTileCompositionLayout *layout;
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *leases[2] = { NULL };
  GstVvasTileCompositionIdentity first;
  GstVvasTileCompositionIdentity second;
  GstVvasTileCompositionSlotSnapshot snapshot;
  GstVvasTileCompositionStats stats;
  BlockedAcquireData data[2] = { {0} };
  ReservationGate *gate;
  GThread *threads[2];
  GError *error = NULL;
  guint attempts;

  layout = new_layout ();
  gate = g_new0 (ReservationGate, 1);
  g_mutex_init (&gate->lock);
  g_cond_init (&gate->condition);
  gate->succeed = TRUE;
  coordinator = gst_vvas_tile_composition_coordinator_new_full (layout, 1, 0xF,
      reserve_slot_with_gate, unreserve_slot_with_gate, gate,
      reservation_gate_free, &error);
  g_assert_no_error (error);
  g_assert_nonnull (coordinator);
  gst_vvas_tile_composition_layout_unref (layout);

  data[0].coordinator = coordinator;
  data[0].tile_id = 0;
  threads[0] = g_thread_new ("reserve-owner", blocked_acquire_thread, &data[0]);

  g_mutex_lock (&gate->lock);
  while (!gate->entered)
    g_cond_wait (&gate->condition, &gate->lock);
  g_mutex_unlock (&gate->lock);

  data[1].coordinator = coordinator;
  data[1].tile_id = 1;
  threads[1] =
      g_thread_new ("reserve-waiter", blocked_acquire_thread, &data[1]);
  for (attempts = 0; attempts < 1000; attempts++) {
    gst_vvas_tile_composition_coordinator_get_stats (coordinator, &stats);
    if (stats.blocked_acquires)
      break;
    g_usleep (1000);
  }
  g_assert_cmpuint (stats.blocked_acquires, >, 0);

  g_mutex_lock (&gate->lock);
  gate->allow = TRUE;
  g_cond_broadcast (&gate->condition);
  g_mutex_unlock (&gate->lock);
  g_thread_join (threads[0]);
  g_thread_join (threads[1]);

  g_assert_cmpint (data[0].result, ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  g_assert_cmpint (data[1].result, ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  leases[0] = data[0].lease;
  leases[1] = data[1].lease;
  gst_vvas_tile_composition_lease_get_identity (leases[0], &first);
  gst_vvas_tile_composition_lease_get_identity (leases[1], &second);
  g_assert_cmpuint (first.slot_id, ==, second.slot_id);
  g_assert_cmpuint (first.epoch, ==, second.epoch);
  g_assert_cmpuint (gate->reserve_count, ==, 1);
  g_assert_cmpuint (gate->unreserve_count, ==, 0);

  gst_vvas_tile_composition_coordinator_cancel_tile (leases[0],
      GST_VVAS_TILE_COMPOSITION_ABORT_EXPLICIT);
  gst_vvas_tile_composition_lease_unref (leases[0]);
  gst_vvas_tile_composition_lease_unref (leases[1]);
  g_assert_cmpuint (gate->unreserve_count, ==, 1);
  assert_final_coordinator_ref_and_unref (coordinator);

  layout = new_layout ();
  gate = g_new0 (ReservationGate, 1);
  g_mutex_init (&gate->lock);
  g_cond_init (&gate->condition);
  gate->allow = TRUE;
  gate->succeed = FALSE;
  coordinator = gst_vvas_tile_composition_coordinator_new_full (layout, 1, 0xF,
      reserve_slot_with_gate, unreserve_slot_with_gate, gate,
      reservation_gate_free, &error);
  g_assert_no_error (error);
  gst_vvas_tile_composition_layout_unref (layout);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
      (coordinator, 0, FALSE, &leases[0]), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  g_assert_null (leases[0]);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, 0, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  assert_final_coordinator_ref_and_unref (coordinator);
}

static void
test_flush_during_reservation_blocks_slot_reuse (void)
{
  GstVvasTileCompositionLayout *layout;
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *new_lease = NULL;
  GstVvasTileCompositionSlotSnapshot snapshot;
  BlockedAcquireData data = { 0 };
  ReservationGate *gate;
  GThread *thread;
  GError *error = NULL;

  layout = new_layout ();
  gate = g_new0 (ReservationGate, 1);
  g_mutex_init (&gate->lock);
  g_cond_init (&gate->condition);
  gate->succeed = TRUE;
  coordinator = gst_vvas_tile_composition_coordinator_new_full (layout, 1, 0xF,
      reserve_slot_with_gate, unreserve_slot_with_gate, gate,
      reservation_gate_free, &error);
  gst_vvas_tile_composition_layout_unref (layout);
  g_assert_no_error (error);

  data.coordinator = coordinator;
  data.tile_id = 0;
  data.result = GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
  thread = g_thread_new ("reserve-flush-owner", blocked_acquire_thread, &data);

  g_mutex_lock (&gate->lock);
  while (!gate->entered)
    g_cond_wait (&gate->condition, &gate->lock);
  g_mutex_unlock (&gate->lock);

  gst_vvas_tile_composition_coordinator_set_flushing (coordinator, TRUE);
  gst_vvas_tile_composition_coordinator_set_flushing (coordinator, FALSE);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, 0, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING);
  g_assert_true (snapshot.reservation_in_flight);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
      (coordinator, 1, TRUE, &new_lease), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_NO_SLOT);
  g_assert_null (new_lease);
  g_assert_cmpuint (gate->reserve_count, ==, 1);

  g_mutex_lock (&gate->lock);
  gate->allow = TRUE;
  g_cond_broadcast (&gate->condition);
  g_mutex_unlock (&gate->lock);
  g_thread_join (thread);

  g_assert_cmpint (data.result, ==, GST_VVAS_TILE_COMPOSITION_RESULT_FLUSHING);
  g_assert_null (data.lease);
  g_assert_cmpuint (gate->unreserve_count, ==, 1);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, 0, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  g_assert_false (snapshot.reservation_in_flight);

  g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
      (coordinator, 1, TRUE, &new_lease), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  g_assert_cmpuint (gate->reserve_count, ==, 2);
  gst_vvas_tile_composition_lease_unref (new_lease);
  g_assert_cmpuint (gate->unreserve_count, ==, 2);
  assert_final_coordinator_ref_and_unref (coordinator);
}

static void
test_publication_order_barrier (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *tiles[2][GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { {NULL} };
  GstVvasTileCompositionLease *outputs[2] = { NULL };
  GstVvasTileCompositionIdentity identities[2];
  GstVvasTileCompositionStats stats;
  guint tile_id;
  guint epoch_idx;

  coordinator = new_coordinator (2, 0xF, NULL);
  for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++) {
    for (epoch_idx = 0; epoch_idx < 2; epoch_idx++) {
      g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
          (coordinator, tile_id, FALSE, &tiles[epoch_idx][tile_id]), ==,
          GST_VVAS_TILE_COMPOSITION_RESULT_OK);
      g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_tile_ready
          (tiles[epoch_idx][tile_id], tile_id, GST_SECOND / 30), ==,
          GST_VVAS_TILE_COMPOSITION_RESULT_OK);
    }
  }
  assert_same_epoch (tiles[0], GST_VVAS_TILE_COMPOSITION_TILE_COUNT);
  assert_same_epoch (tiles[1], GST_VVAS_TILE_COMPOSITION_TILE_COUNT);

  g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
      (coordinator, tiles[1], GST_VVAS_TILE_COMPOSITION_TILE_COUNT,
          &outputs[1]), ==, GST_VVAS_TILE_COMPOSITION_RESULT_WAITING);
  g_assert_null (outputs[1]);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
      (coordinator, tiles[0], GST_VVAS_TILE_COMPOSITION_TILE_COUNT,
          &outputs[0]), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_wait_until_publishable
      (tiles[1]
          [0], GST_MSECOND), ==, GST_VVAS_TILE_COMPOSITION_RESULT_WAITING);
  gst_vvas_tile_composition_coordinator_get_stats (coordinator, &stats);
  g_assert_cmpuint (stats.publication_waits, ==, 1);
  g_assert_cmpuint (stats.publication_wait_timeouts, ==, 1);
  for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++)
    gst_vvas_tile_composition_lease_unref (tiles[0][tile_id]);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_complete_processing
      (outputs[0]), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  gst_vvas_tile_composition_lease_get_identity (outputs[0], &identities[0]);
  gst_vvas_tile_composition_coordinator_mark_output_returned (coordinator,
      &identities[0]);
  gst_vvas_tile_composition_lease_unref (outputs[0]);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_wait_until_publishable
      (tiles[1]
          [0], GST_SECOND), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);

  g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
      (coordinator, tiles[1], GST_VVAS_TILE_COMPOSITION_TILE_COUNT,
          &outputs[1]), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++)
    gst_vvas_tile_composition_lease_unref (tiles[1][tile_id]);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_complete_processing
      (outputs[1]), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  gst_vvas_tile_composition_lease_get_identity (outputs[1], &identities[1]);
  g_assert_cmpuint (identities[1].epoch, >, identities[0].epoch);
  gst_vvas_tile_composition_coordinator_mark_output_returned (coordinator,
      &identities[1]);
  gst_vvas_tile_composition_lease_unref (outputs[1]);
  gst_vvas_tile_composition_coordinator_unref (coordinator);
}

static void
test_reference_lifetime_and_output_refs (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionLease *output = NULL;
  GstVvasTileCompositionLease *output_ref;
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionSlotSnapshot snapshot;
  gboolean finalized = FALSE;
  guint i;

  coordinator = new_coordinator (1, 0xF, NULL);
  gst_mini_object_weak_ref (GST_MINI_OBJECT_CAST (coordinator),
      coordinator_finalized, &finalized);
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (coordinator), ==, 1);

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
  gst_vvas_tile_composition_coordinator_complete_processing (output);
  gst_vvas_tile_composition_lease_get_identity (output, &identity);
  output_ref = gst_vvas_tile_composition_lease_ref (output);
  gst_vvas_tile_composition_coordinator_mark_output_returned (coordinator,
      &identity);

  gst_vvas_tile_composition_lease_unref (output);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, 0, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_RETIRING);
  g_assert_cmpuint (snapshot.active_output_leases, ==, 1);
  gst_vvas_tile_composition_lease_unref (output_ref);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, 0, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (coordinator), ==, 1);

  gst_vvas_tile_composition_coordinator_unref (coordinator);
  g_assert_true (finalized);
}

static void
test_flush_preserves_downstream (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionLease *output = NULL;
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionSlotSnapshot snapshot;
  GstVvasTileCompositionStats stats;
  guint i;

  coordinator = new_coordinator (1, 0xF, NULL);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    gst_vvas_tile_composition_coordinator_acquire_tile (coordinator, i, FALSE,
        &tiles[i]);
    gst_vvas_tile_composition_coordinator_mark_tile_ready (tiles[i], i,
        GST_SECOND / 30);
  }
  gst_vvas_tile_composition_coordinator_begin_processing (coordinator, tiles,
      GST_VVAS_TILE_COMPOSITION_TILE_COUNT, &output);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++)
    gst_vvas_tile_composition_lease_unref (tiles[i]);
  gst_vvas_tile_composition_coordinator_complete_processing (output);
  gst_vvas_tile_composition_lease_get_identity (output, &identity);

  gst_vvas_tile_composition_coordinator_set_flushing (coordinator, TRUE);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, 0, &snapshot));
  g_assert_cmpint (snapshot.state, ==,
      GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM);
  gst_vvas_tile_composition_coordinator_get_stats (coordinator, &stats);
  g_assert_cmpuint (stats.epochs_aborted, ==, 0);

  gst_vvas_tile_composition_coordinator_mark_output_returned (coordinator,
      &identity);
  gst_vvas_tile_composition_lease_unref (output);
  gst_vvas_tile_composition_coordinator_set_flushing (coordinator, FALSE);
  assert_final_coordinator_ref_and_unref (coordinator);
}

static void
test_flush_during_processing_returns_flushing (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionLease *output = NULL;
  GstVvasTileCompositionSlotSnapshot snapshot;
  ReservationGate *gate;
  guint i;

  coordinator = new_storage_coordinator (1, &gate);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
        (coordinator, i, FALSE, &tiles[i]), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_tile_ready
        (tiles[i], i, GST_SECOND / 30), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  }
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
      (coordinator, tiles, GST_VVAS_TILE_COMPOSITION_TILE_COUNT, &output), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++)
    gst_vvas_tile_composition_lease_unref (tiles[i]);

  gst_vvas_tile_composition_coordinator_set_flushing (coordinator, TRUE);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_complete_processing
      (output), ==, GST_VVAS_TILE_COMPOSITION_RESULT_FLUSHING);
  gst_vvas_tile_composition_lease_unref (output);
  g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
      (coordinator, 0, &snapshot));
  g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  g_assert_cmpuint (gate->unreserve_count, ==, 1);
  gst_vvas_tile_composition_coordinator_set_flushing (coordinator, FALSE);
  assert_final_coordinator_ref_and_unref (coordinator);
}

static void
test_out_of_order_output_release (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *outputs[3] = { NULL };
  GstVvasTileCompositionIdentity identities[3];
  guint epoch_idx;
  guint tile_id;

  coordinator = new_coordinator (3, 0xF, NULL);
  for (epoch_idx = 0; epoch_idx < 3; epoch_idx++) {
    GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
        { NULL };

    for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++) {
      g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
          (coordinator, tile_id, FALSE, &tiles[tile_id]), ==,
          GST_VVAS_TILE_COMPOSITION_RESULT_OK);
      g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_tile_ready
          (tiles[tile_id], tile_id, GST_SECOND / 30), ==,
          GST_VVAS_TILE_COMPOSITION_RESULT_OK);
    }
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
        (coordinator, tiles, GST_VVAS_TILE_COMPOSITION_TILE_COUNT,
            &outputs[epoch_idx]), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
    for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++)
      gst_vvas_tile_composition_lease_unref (tiles[tile_id]);
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_complete_processing
        (outputs[epoch_idx]), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
    gst_vvas_tile_composition_lease_get_identity (outputs[epoch_idx],
        &identities[epoch_idx]);
  }

  for (epoch_idx = 3; epoch_idx > 0; epoch_idx--) {
    guint index = epoch_idx - 1;

    g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_output_returned
        (coordinator, &identities[index]), ==,
        GST_VVAS_TILE_COMPOSITION_RESULT_OK);
    gst_vvas_tile_composition_lease_unref (outputs[index]);
  }
  gst_vvas_tile_composition_coordinator_unref (coordinator);
}

static void
test_randomized_fault_lifecycle (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  FaultStorage *storage;
  guint iteration;
  guint slot_id;

  coordinator = new_fault_coordinator (4, &storage);
  for (iteration = 0; iteration < 200; iteration++) {
    GstVvasTileCompositionLease *tiles[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
        { NULL };
    GstVvasTileCompositionLease *output = NULL;
    GstVvasTileCompositionIdentity identity;
    guint action = iteration < 6 ? iteration :
        (guint) g_test_rand_int_range (0, 6);
    guint tile_id;

    switch (action) {
      case 0:
        for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT;
            tile_id++) {
          g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
              (coordinator, tile_id, FALSE, &tiles[tile_id]), ==,
              GST_VVAS_TILE_COMPOSITION_RESULT_OK);
          g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_tile_ready
              (tiles[tile_id], iteration, GST_SECOND / 30), ==,
              GST_VVAS_TILE_COMPOSITION_RESULT_OK);
        }
        g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
            (coordinator, tiles, GST_VVAS_TILE_COMPOSITION_TILE_COUNT, &output),
            ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
        for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT;
            tile_id++)
          gst_vvas_tile_composition_lease_unref (tiles[tile_id]);
        g_assert_cmpint
            (gst_vvas_tile_composition_coordinator_complete_processing (output),
            ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
        gst_vvas_tile_composition_lease_get_identity (output, &identity);
        if (g_test_rand_bit ()) {
          g_assert_cmpint
              (gst_vvas_tile_composition_coordinator_mark_output_returned
              (coordinator, &identity), ==,
              GST_VVAS_TILE_COMPOSITION_RESULT_OK);
          gst_vvas_tile_composition_lease_unref (output);
        } else {
          gst_vvas_tile_composition_lease_unref (output);
        }
        break;
      case 1:
        for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT;
            tile_id++)
          g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
              (coordinator, tile_id, FALSE, &tiles[tile_id]), ==,
              GST_VVAS_TILE_COMPOSITION_RESULT_OK);
        tile_id =
            (guint) g_test_rand_int_range (0,
            GST_VVAS_TILE_COMPOSITION_TILE_COUNT);
        gst_vvas_tile_composition_lease_unref (tiles[tile_id]);
        tiles[tile_id] = NULL;
        for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT;
            tile_id++) {
          if (tiles[tile_id])
            gst_vvas_tile_composition_lease_unref (tiles[tile_id]);
        }
        break;
      case 2:
        for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT;
            tile_id++)
          g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
              (coordinator, tile_id, FALSE, &tiles[tile_id]), ==,
              GST_VVAS_TILE_COMPOSITION_RESULT_OK);
        tile_id =
            (guint) g_test_rand_int_range (0,
            GST_VVAS_TILE_COMPOSITION_TILE_COUNT);
        g_assert_cmpint (gst_vvas_tile_composition_coordinator_cancel_tile
            (tiles[tile_id], GST_VVAS_TILE_COMPOSITION_ABORT_EXPLICIT), ==,
            GST_VVAS_TILE_COMPOSITION_RESULT_OK);
        for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT;
            tile_id++)
          gst_vvas_tile_composition_lease_unref (tiles[tile_id]);
        break;
      case 3:
        tile_id = (guint) g_test_rand_int_range (1,
            GST_VVAS_TILE_COMPOSITION_TILE_COUNT + 1);
        while (tile_id--) {
          guint current = GST_VVAS_TILE_COMPOSITION_TILE_COUNT - tile_id - 1;

          g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
              (coordinator, current, FALSE, &tiles[current]), ==,
              GST_VVAS_TILE_COMPOSITION_RESULT_OK);
        }
        gst_vvas_tile_composition_coordinator_set_flushing (coordinator, TRUE);
        for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT;
            tile_id++) {
          if (tiles[tile_id])
            gst_vvas_tile_composition_lease_unref (tiles[tile_id]);
        }
        gst_vvas_tile_composition_coordinator_set_flushing (coordinator, FALSE);
        break;
      case 4:
        g_mutex_lock (&storage->lock);
        storage->fail_next = TRUE;
        g_mutex_unlock (&storage->lock);
        g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
            (coordinator, iteration % GST_VVAS_TILE_COMPOSITION_TILE_COUNT,
                FALSE, &tiles[0]), ==,
            GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
        g_assert_null (tiles[0]);
        break;
      case 5:
      {
        BlockedAcquireData data = { 0 };
        GThread *thread;

        g_mutex_lock (&storage->lock);
        storage->block_next = TRUE;
        storage->entered = FALSE;
        storage->release = FALSE;
        g_mutex_unlock (&storage->lock);
        data.coordinator = coordinator;
        data.tile_id = iteration % GST_VVAS_TILE_COMPOSITION_TILE_COUNT;
        data.result = GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
        thread = g_thread_new ("random-reserve-flush",
            blocked_acquire_thread, &data);
        g_mutex_lock (&storage->lock);
        while (!storage->entered)
          g_cond_wait (&storage->condition, &storage->lock);
        g_mutex_unlock (&storage->lock);
        g_usleep ((gulong) g_test_rand_int_range (0, 2000));
        gst_vvas_tile_composition_coordinator_set_flushing (coordinator, TRUE);
        gst_vvas_tile_composition_coordinator_set_flushing (coordinator, FALSE);
        g_mutex_lock (&storage->lock);
        storage->release = TRUE;
        g_cond_broadcast (&storage->condition);
        g_mutex_unlock (&storage->lock);
        g_thread_join (thread);
        g_assert_cmpint (data.result, ==,
            GST_VVAS_TILE_COMPOSITION_RESULT_FLUSHING);
        g_assert_null (data.lease);
        break;
      }
      default:
        g_assert_not_reached ();
    }

    for (slot_id = 0; slot_id < 4; slot_id++) {
      GstVvasTileCompositionSlotSnapshot snapshot;

      g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
          (coordinator, slot_id, &snapshot));
      g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
    }
  }

  g_mutex_lock (&storage->lock);
  g_assert_cmpuint (storage->reserve_attempts, >, storage->reserve_count);
  g_assert_cmpuint (storage->reserve_count, ==, storage->unreserve_count);
  g_mutex_unlock (&storage->lock);
  assert_final_coordinator_ref_and_unref (coordinator);
}

static void
test_randomized_concurrent_producers (void)
{
  ConcurrentRun run = { 0 };
  ProducerData producers[GST_VVAS_TILE_COMPOSITION_TILE_COUNT];
  GThread *threads[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] = { NULL };
  GstVvasTileCompositionStats stats;
  guint64 expected_epoch;
  guint tile_id;
  guint slot_id;

  run.coordinator = new_coordinator (8, 0xF, NULL);
  run.epochs = 50;
  g_mutex_init (&run.lock);
  g_cond_init (&run.condition);
  run.records = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free,
      epoch_record_free);

  for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++) {
    producers[tile_id].run = &run;
    producers[tile_id].tile_id = tile_id;
    producers[tile_id].seed = g_test_rand_int ();
    threads[tile_id] = g_thread_new ("tile-composition-producer",
        concurrent_producer_thread, &producers[tile_id]);
  }

  for (expected_epoch = 1; expected_epoch <= run.epochs; expected_epoch++) {
    EpochRecord *record;
    gpointer stored_key = NULL;
    gpointer stored_value = NULL;
    GstVvasTileCompositionLease *output = NULL;
    GstVvasTileCompositionIdentity identity;

    g_mutex_lock (&run.lock);
    while (TRUE) {
      record = g_hash_table_lookup (run.records, &expected_epoch);
      if (run.failed || (record && record->mask == 0xF))
        break;
      g_cond_wait (&run.condition, &run.lock);
    }
    g_assert_false (run.failed);
    g_assert_true (g_hash_table_lookup_extended (run.records, &expected_epoch,
            &stored_key, &stored_value));
    g_hash_table_steal (run.records, &expected_epoch);
    g_mutex_unlock (&run.lock);

    record = stored_value;
    assert_same_epoch (record->leases, GST_VVAS_TILE_COMPOSITION_TILE_COUNT);
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_begin_processing
        (run.coordinator, record->leases, GST_VVAS_TILE_COMPOSITION_TILE_COUNT,
            &output), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
    for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++) {
      gst_vvas_tile_composition_lease_unref (record->leases[tile_id]);
      record->leases[tile_id] = NULL;
    }
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_complete_processing
        (output), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
    gst_vvas_tile_composition_lease_get_identity (output, &identity);
    g_assert_cmpuint (identity.epoch, ==, expected_epoch);
    g_usleep (g_test_rand_int_range (0, 1000));
    g_assert_cmpint (gst_vvas_tile_composition_coordinator_mark_output_returned
        (run.coordinator, &identity), ==, GST_VVAS_TILE_COMPOSITION_RESULT_OK);
    gst_vvas_tile_composition_lease_unref (output);
    g_free (stored_key);
    g_free (record);
  }

  for (tile_id = 0; tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile_id++)
    g_thread_join (threads[tile_id]);
  g_assert_false (run.failed);
  g_assert_cmpuint (g_hash_table_size (run.records), ==, 0);
  gst_vvas_tile_composition_coordinator_get_stats (run.coordinator, &stats);
  g_assert_cmpuint (stats.epochs_opened, ==, run.epochs);
  g_assert_cmpuint (stats.epochs_published, ==, run.epochs);
  g_assert_cmpuint (stats.last_published_epoch, ==, run.epochs);
  for (slot_id = 0; slot_id < 8; slot_id++) {
    GstVvasTileCompositionSlotSnapshot snapshot;

    g_assert_true (gst_vvas_tile_composition_coordinator_get_slot_snapshot
        (run.coordinator, slot_id, &snapshot));
    g_assert_cmpint (snapshot.state, ==, GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  }

  g_hash_table_unref (run.records);
  g_cond_clear (&run.condition);
  g_mutex_clear (&run.lock);
  gst_vvas_tile_composition_coordinator_unref (run.coordinator);
}

static void
test_required_mask_rejects_inactive_tile (void)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionLease *lease = NULL;

  coordinator = new_coordinator (1, 0x3, NULL);
  g_assert_cmpint (gst_vvas_tile_composition_coordinator_acquire_tile
      (coordinator, 2, TRUE, &lease), ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  g_assert_null (lease);
  gst_vvas_tile_composition_coordinator_unref (coordinator);
}

int
main (int argc, char **argv)
{
  gint result;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  gst_debug_set_default_threshold (GST_LEVEL_ERROR);
  gst_debug_add_log_function (fail_on_composition_error, NULL, NULL);

  g_test_add_func ("/vvas/tile-composition-coordinator/normal",
      test_normal_epoch_lifecycle);
  g_test_add_func ("/vvas/tile-composition-coordinator/fast-slow",
      test_fast_producer_oldest_assignment);
  g_test_add_func ("/vvas/tile-composition-coordinator/abort-writing",
      test_abort_epoch_writing_lifecycle);
  g_test_add_func ("/vvas/tile-composition-coordinator/abort-ready",
      test_abort_epoch_ready_lifecycle);
  g_test_add_func ("/vvas/tile-composition-coordinator/abort-processing",
      test_abort_epoch_processing_lifecycle);
  g_test_add_func ("/vvas/tile-composition-coordinator/abort-downstream",
      test_abort_epoch_rejects_downstream);
  g_test_add_func ("/vvas/tile-composition-coordinator/output-lease-drop",
      test_output_lease_drop_unreserves);
  g_test_add_func
      ("/vvas/tile-composition-coordinator/stopping-output-retirement",
      test_stopping_retires_unreturned_output);
  g_test_add_func ("/vvas/tile-composition-coordinator/drop-abort",
      test_drop_aborts_whole_epoch);
  g_test_add_func ("/vvas/tile-composition-coordinator/ready-drop-abort",
      test_ready_but_unconsumed_drop_aborts);
  g_test_add_func ("/vvas/tile-composition-coordinator/invalid-processing-set",
      test_begin_processing_rejects_invalid_sets);
  g_test_add_func ("/vvas/tile-composition-coordinator/stale-output",
      test_output_release_and_stale_identity);
  g_test_add_func ("/vvas/tile-composition-coordinator/flush-wakeup",
      test_flush_wakes_blocked_producer);
  g_test_add_func ("/vvas/tile-composition-coordinator/reservation-barrier",
      test_reservation_barrier_and_rollback);
  g_test_add_func ("/vvas/tile-composition-coordinator/reservation-flush-race",
      test_flush_during_reservation_blocks_slot_reuse);
  g_test_add_func ("/vvas/tile-composition-coordinator/publication-order",
      test_publication_order_barrier);
  g_test_add_func ("/vvas/tile-composition-coordinator/reference-lifetime",
      test_reference_lifetime_and_output_refs);
  g_test_add_func ("/vvas/tile-composition-coordinator/flush-downstream",
      test_flush_preserves_downstream);
  g_test_add_func ("/vvas/tile-composition-coordinator/flush-processing",
      test_flush_during_processing_returns_flushing);
  g_test_add_func ("/vvas/tile-composition-coordinator/output-release-order",
      test_out_of_order_output_release);
  g_test_add_func
      ("/vvas/tile-composition-coordinator/randomized-fault-lifecycle",
      test_randomized_fault_lifecycle);
  g_test_add_func ("/vvas/tile-composition-coordinator/randomized-concurrent",
      test_randomized_concurrent_producers);
  g_test_add_func ("/vvas/tile-composition-coordinator/required-mask",
      test_required_mask_rejects_inactive_tile);

  result = g_test_run ();
  gst_debug_remove_log_function (fail_on_composition_error);
  return result;
}
