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

#ifndef __GST_VVAS_TILE_COMPOSITION_COORDINATOR_H__
#define __GST_VVAS_TILE_COMPOSITION_COORDINATOR_H__

#include <gst/gst.h>
#include <gst/vvas/gstvvastilecompositionlayout.h>

G_BEGIN_DECLS

typedef struct _GstVvasTileCompositionCoordinator
    GstVvasTileCompositionCoordinator;
typedef struct _GstVvasTileCompositionLease GstVvasTileCompositionLease;

#define GST_TYPE_VVAS_TILE_COMPOSITION_COORDINATOR                             \
  (gst_vvas_tile_composition_coordinator_get_type ())
#define GST_TYPE_VVAS_TILE_COMPOSITION_LEASE                                   \
  (gst_vvas_tile_composition_lease_get_type ())

typedef enum
{
  GST_VVAS_TILE_COMPOSITION_SLOT_FREE,
  GST_VVAS_TILE_COMPOSITION_SLOT_RESERVING,
  GST_VVAS_TILE_COMPOSITION_SLOT_WRITING,
  GST_VVAS_TILE_COMPOSITION_SLOT_READY,
  GST_VVAS_TILE_COMPOSITION_SLOT_PROCESSING,
  GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM,
  GST_VVAS_TILE_COMPOSITION_SLOT_RETIRING,
  GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING
} GstVvasTileCompositionSlotState;

typedef enum
{
  GST_VVAS_TILE_COMPOSITION_LEASE_TILE,
  GST_VVAS_TILE_COMPOSITION_LEASE_OUTPUT
} GstVvasTileCompositionLeaseRole;

typedef enum
{
  GST_VVAS_TILE_COMPOSITION_RESULT_OK,
  GST_VVAS_TILE_COMPOSITION_RESULT_NO_SLOT,
  GST_VVAS_TILE_COMPOSITION_RESULT_FLUSHING,
  GST_VVAS_TILE_COMPOSITION_RESULT_INCOMPLETE,
  GST_VVAS_TILE_COMPOSITION_RESULT_WAITING,
  GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE,
  GST_VVAS_TILE_COMPOSITION_RESULT_STALE,
  GST_VVAS_TILE_COMPOSITION_RESULT_ABORTED,
  GST_VVAS_TILE_COMPOSITION_RESULT_INVALID
} GstVvasTileCompositionResult;

typedef enum
{
  GST_VVAS_TILE_COMPOSITION_ABORT_NONE,
  GST_VVAS_TILE_COMPOSITION_ABORT_LEASE_DROPPED,
  GST_VVAS_TILE_COMPOSITION_ABORT_EXPLICIT,
  GST_VVAS_TILE_COMPOSITION_ABORT_FLUSHING,
  GST_VVAS_TILE_COMPOSITION_ABORT_PROCESSING_FAILED,
  GST_VVAS_TILE_COMPOSITION_ABORT_INVALID_SET
} GstVvasTileCompositionAbortReason;

typedef struct _GstVvasTileCompositionIdentity
{
  guint slot_id;
  guint64 epoch;
  guint64 layout_version;
  GstVvasTileCompositionLeaseRole role;
  guint tile_id;
} GstVvasTileCompositionIdentity;

typedef struct _GstVvasTileCompositionSlotSnapshot
{
  guint slot_id;
  guint64 epoch;
  guint64 layout_version;
  GstVvasTileCompositionSlotState state;
  guint required_mask;
  guint issued_mask;
  guint ready_mask;
  guint consumed_mask;
  guint cancelled_mask;
  guint active_tile_leases;
  guint active_output_leases;
  gboolean output_returned;
  gboolean storage_reserved;
  gboolean reservation_in_flight;
  GstVvasTileCompositionAbortReason abort_reason;
} GstVvasTileCompositionSlotSnapshot;

typedef struct _GstVvasTileCompositionStats
{
  guint64 epochs_opened;
  guint64 epochs_ready;
  guint64 epochs_published;
  guint64 epochs_aborted;
  guint64 tile_leases_issued;
  guint64 tile_leases_ready;
  guint64 blocked_acquires;
  guint64 publication_waits;
  guint64 publication_wait_timeouts;
  guint64 stale_callbacks;
  guint64 duplicate_callbacks;
  guint64 last_published_epoch;
} GstVvasTileCompositionStats;

typedef gboolean (*GstVvasTileCompositionReserveSlotFunc) (
    const GstVvasTileCompositionIdentity *identity, gpointer user_data);
typedef void (*GstVvasTileCompositionUnreserveSlotFunc) (
    const GstVvasTileCompositionIdentity *identity, gpointer user_data);

GST_EXPORT
GType gst_vvas_tile_composition_coordinator_get_type (
    void) G_GNUC_CONST;

/**
 * gst_vvas_tile_composition_coordinator_new:
 *
 * Creates a coordinator without an external storage reservation callback.
 *
 * Returns: (transfer full) (nullable): a coordinator.
 */
GST_EXPORT
GstVvasTileCompositionCoordinator *gst_vvas_tile_composition_coordinator_new (
    GstVvasTileCompositionLayout *layout,
    guint max_slots,
    guint required_tile_mask,
    GError **error);

/**
 * gst_vvas_tile_composition_coordinator_new_full:
 * @reserve_slot: (scope call) (nullable): called outside the coordinator lock
 * @unreserve_slot: (scope call) (nullable): rolls back a successful reservation
 * @user_data: callback data
 * @notify: (nullable): destroys @user_data with the coordinator
 *
 * The reserve callback must retain any acquired storage by slot identity.
 * When it returns %FALSE it must leave no storage behind. If it succeeds but a
 * concurrent flush rejects the reservation, @unreserve_slot is called.
 *
 * Returns: (transfer full) (nullable): a coordinator.
 */
GST_EXPORT
GstVvasTileCompositionCoordinator *
gst_vvas_tile_composition_coordinator_new_full (
    GstVvasTileCompositionLayout *layout,
    guint max_slots,
    guint required_tile_mask,
    GstVvasTileCompositionReserveSlotFunc reserve_slot,
    GstVvasTileCompositionUnreserveSlotFunc unreserve_slot,
    gpointer user_data,
    GDestroyNotify notify,
    GError **error);

GST_EXPORT
GstVvasTileCompositionCoordinator *gst_vvas_tile_composition_coordinator_ref (
    GstVvasTileCompositionCoordinator *coordinator);

GST_EXPORT
void gst_vvas_tile_composition_coordinator_unref (
    GstVvasTileCompositionCoordinator *coordinator);

GST_EXPORT
GType gst_vvas_tile_composition_lease_get_type (void) G_GNUC_CONST;

GST_EXPORT
GstVvasTileCompositionLease *gst_vvas_tile_composition_lease_ref (
    GstVvasTileCompositionLease *lease);

GST_EXPORT
void gst_vvas_tile_composition_lease_unref (
    GstVvasTileCompositionLease *lease);

GST_EXPORT
GstVvasTileCompositionLeaseRole
gst_vvas_tile_composition_lease_get_role (
    const GstVvasTileCompositionLease *lease);

GST_EXPORT
guint gst_vvas_tile_composition_lease_get_tile_id (
    const GstVvasTileCompositionLease *lease);

GST_EXPORT
void gst_vvas_tile_composition_lease_get_identity (
    const GstVvasTileCompositionLease *lease,
    GstVvasTileCompositionIdentity *identity);

/**
 * gst_vvas_tile_composition_coordinator_acquire_tile:
 * @coordinator: a coordinator
 * @tile_id: required tile index
 * @dont_wait: return NO_SLOT instead of blocking
 * @lease: (out) (transfer full): exact tile lease on success
 *
 * Assigns the oldest eligible epoch missing @tile_id, or reserves a new slot.
 * The call wakes with FLUSHING when the coordinator stops or flushes.
 */
GST_EXPORT
GstVvasTileCompositionResult
gst_vvas_tile_composition_coordinator_acquire_tile (
    GstVvasTileCompositionCoordinator *coordinator,
    guint tile_id,
    gboolean dont_wait,
    GstVvasTileCompositionLease **lease);

GST_EXPORT
GstVvasTileCompositionResult
gst_vvas_tile_composition_coordinator_mark_tile_ready (
    GstVvasTileCompositionLease *lease,
    GstClockTime pts,
    GstClockTime duration);

/**
 * gst_vvas_tile_composition_coordinator_cancel_tile:
 *
 * Aborts the exact epoch but does not release @lease; the caller must still
 * unref its lease.
 */
GST_EXPORT
GstVvasTileCompositionResult gst_vvas_tile_composition_coordinator_cancel_tile (
    GstVvasTileCompositionLease *lease,
    GstVvasTileCompositionAbortReason reason);

GST_EXPORT
GstVvasTileCompositionResult gst_vvas_tile_composition_coordinator_abort_epoch (
    GstVvasTileCompositionCoordinator *coordinator,
    const GstVvasTileCompositionIdentity *identity,
    GstVvasTileCompositionAbortReason reason);

GST_EXPORT
GstVvasTileCompositionResult
gst_vvas_tile_composition_coordinator_begin_processing (
    GstVvasTileCompositionCoordinator *coordinator,
    GstVvasTileCompositionLease *const *tile_leases,
    guint n_tile_leases,
    GstVvasTileCompositionLease **output_lease);

GST_EXPORT
GstVvasTileCompositionResult
gst_vvas_tile_composition_coordinator_wait_until_publishable (
    GstVvasTileCompositionLease *tile_lease, GstClockTime timeout);

/**
 * gst_vvas_tile_composition_coordinator_complete_processing:
 *
 * Publishes the PROCESSING epoch. DUPLICATE means it was already completed.
 * INCOMPLETE from begin_processing means this set is not READY; WAITING means
 * an older unpublished epoch is the ordering barrier.
 */
GST_EXPORT
GstVvasTileCompositionResult
gst_vvas_tile_composition_coordinator_complete_processing (
    GstVvasTileCompositionLease *output_lease);

GST_EXPORT
GstVvasTileCompositionResult
gst_vvas_tile_composition_coordinator_mark_output_returned (
    GstVvasTileCompositionCoordinator *coordinator,
    const GstVvasTileCompositionIdentity *identity);

GST_EXPORT
GstVvasTileCompositionResult
gst_vvas_tile_composition_output_lease_mark_returned (
    GstVvasTileCompositionLease *output_lease);

/*
 * These lifecycle calls can release the final active-epoch self-reference.
 * Callers must hold their own coordinator reference for the complete call.
 */
GST_EXPORT
void gst_vvas_tile_composition_coordinator_set_flushing (
    GstVvasTileCompositionCoordinator *coordinator, gboolean flushing);

GST_EXPORT
void gst_vvas_tile_composition_coordinator_set_tile_flushing (
    GstVvasTileCompositionCoordinator *coordinator,
    guint tile_id,
    gboolean flushing);

GST_EXPORT
void gst_vvas_tile_composition_coordinator_set_stopping (
    GstVvasTileCompositionCoordinator *coordinator);

/**
 * gst_vvas_tile_composition_coordinator_set_pool_flushing:
 * @coordinator: a coordinator
 * @tile_id: producer tile index
 * @flushing: %TRUE while the producer's buffer pool is flushing
 *
 * Reports buffer-pool-level flushing for one tile. Pool flushing is a
 * transient producer-side state that GStreamer toggles during normal
 * playback, so this only makes tile acquisition return FLUSHING for @tile_id
 * and wakes that tile's waiters. It aborts no epoch, releases no reference and
 * does not hold back any other tile.
 */
GST_EXPORT
void gst_vvas_tile_composition_coordinator_set_pool_flushing (
    GstVvasTileCompositionCoordinator *coordinator,
    guint tile_id,
    gboolean flushing);

GST_EXPORT
gboolean gst_vvas_tile_composition_coordinator_get_slot_snapshot (
    GstVvasTileCompositionCoordinator *coordinator,
    guint slot_id,
    GstVvasTileCompositionSlotSnapshot *snapshot);

GST_EXPORT
void gst_vvas_tile_composition_coordinator_get_stats (
    GstVvasTileCompositionCoordinator *coordinator,
    GstVvasTileCompositionStats *stats);

GST_EXPORT
const gchar *gst_vvas_tile_composition_slot_state_name (
    GstVvasTileCompositionSlotState state);

GST_EXPORT
const gchar *gst_vvas_tile_composition_result_name (
    GstVvasTileCompositionResult result);

G_END_DECLS
#endif /* __GST_VVAS_TILE_COMPOSITION_COORDINATOR_H__ */
