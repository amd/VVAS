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

#include "gstvvastilecompositioncoordinator.h"

GST_DEBUG_CATEGORY_STATIC (gst_vvas_tile_composition_coordinator_debug);
#define GST_CAT_DEFAULT gst_vvas_tile_composition_coordinator_debug

typedef struct _GstVvasTileCompositionSlot
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
  gboolean epoch_ref_held;
  gboolean storage_reserved;
  gboolean reservation_in_flight;
  GstClockTime tile_pts[GST_VVAS_TILE_COMPOSITION_TILE_COUNT];
  GstClockTime tile_duration[GST_VVAS_TILE_COMPOSITION_TILE_COUNT];
  GstVvasTileCompositionAbortReason abort_reason;
} GstVvasTileCompositionSlot;

struct _GstVvasTileCompositionCoordinator
{
  GstMiniObject parent;
  GMutex lock;
  GCond condition;
  GstVvasTileCompositionLayout *layout;
  GstVvasTileCompositionSlot *slots;
  guint max_slots;
  guint required_tile_mask;
  guint64 next_epoch;
  guint64 last_published_epoch;
  guint64 flush_generation;
  guint64 tile_flush_generation[GST_VVAS_TILE_COMPOSITION_TILE_COUNT];
  guint tile_flushing_mask;
  guint pool_flushing_mask;
  gboolean flushing;
  gboolean stopping;
  GstVvasTileCompositionReserveSlotFunc reserve_slot;
  GstVvasTileCompositionUnreserveSlotFunc unreserve_slot;
  gpointer storage_user_data;
  GDestroyNotify storage_notify;
  GstVvasTileCompositionStats stats;
};

struct _GstVvasTileCompositionLease
{
  GstMiniObject parent;
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionIdentity identity;
};

GST_DEFINE_MINI_OBJECT_TYPE (GstVvasTileCompositionCoordinator,
    gst_vvas_tile_composition_coordinator);
GST_DEFINE_MINI_OBJECT_TYPE (GstVvasTileCompositionLease,
    gst_vvas_tile_composition_lease);

static void
gst_vvas_tile_composition_coordinator_free (GstVvasTileCompositionCoordinator *
    coordinator);
static void gst_vvas_tile_composition_lease_free (GstVvasTileCompositionLease *
    lease);

static void
gst_vvas_tile_composition_coordinator_ensure_debug_category (void)
{
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized)) {
    GST_DEBUG_CATEGORY_INIT (gst_vvas_tile_composition_coordinator_debug,
        "vvas_tile_composition", 0,
        "VVAS tile composition coordinator and layout");
    g_once_init_leave (&initialized, 1);
  }
}

const gchar *
gst_vvas_tile_composition_slot_state_name (GstVvasTileCompositionSlotState
    state)
{
  switch (state) {
    case GST_VVAS_TILE_COMPOSITION_SLOT_FREE:
      return "FREE";
    case GST_VVAS_TILE_COMPOSITION_SLOT_RESERVING:
      return "RESERVING";
    case GST_VVAS_TILE_COMPOSITION_SLOT_WRITING:
      return "WRITING";
    case GST_VVAS_TILE_COMPOSITION_SLOT_READY:
      return "READY";
    case GST_VVAS_TILE_COMPOSITION_SLOT_PROCESSING:
      return "PROCESSING";
    case GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM:
      return "DOWNSTREAM";
    case GST_VVAS_TILE_COMPOSITION_SLOT_RETIRING:
      return "RETIRING";
    case GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING:
      return "ABORTING";
    default:
      return "UNKNOWN";
  }
}

const gchar *
gst_vvas_tile_composition_result_name (GstVvasTileCompositionResult result)
{
  switch (result) {
    case GST_VVAS_TILE_COMPOSITION_RESULT_OK:
      return "OK";
    case GST_VVAS_TILE_COMPOSITION_RESULT_NO_SLOT:
      return "NO_SLOT";
    case GST_VVAS_TILE_COMPOSITION_RESULT_FLUSHING:
      return "FLUSHING";
    case GST_VVAS_TILE_COMPOSITION_RESULT_INCOMPLETE:
      return "INCOMPLETE";
    case GST_VVAS_TILE_COMPOSITION_RESULT_WAITING:
      return "WAITING";
    case GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE:
      return "DUPLICATE";
    case GST_VVAS_TILE_COMPOSITION_RESULT_STALE:
      return "STALE";
    case GST_VVAS_TILE_COMPOSITION_RESULT_ABORTED:
      return "ABORTED";
    case GST_VVAS_TILE_COMPOSITION_RESULT_INVALID:
      return "INVALID";
    default:
      return "UNKNOWN";
  }
}

static void
gst_vvas_tile_composition_slot_transition (GstVvasTileCompositionCoordinator *
    coordinator, GstVvasTileCompositionSlot * slot,
    GstVvasTileCompositionSlotState state)
{
  GstVvasTileCompositionSlotState previous = slot->state;

  slot->state = state;
  GST_DEBUG ("slot=%u epoch=%" G_GUINT64_FORMAT " state=%s->%s "
      "masks=req:%x issued:%x ready:%x consumed:%x cancelled:%x leases=%u/%u "
      "storage=%d reserve-in-flight=%d",
      slot->slot_id, slot->epoch,
      gst_vvas_tile_composition_slot_state_name (previous),
      gst_vvas_tile_composition_slot_state_name (state), slot->required_mask,
      slot->issued_mask, slot->ready_mask, slot->consumed_mask,
      slot->cancelled_mask, slot->active_tile_leases,
      slot->active_output_leases, slot->storage_reserved,
      slot->reservation_in_flight);
  g_cond_broadcast (&coordinator->condition);
}

static guint
gst_vvas_tile_composition_mask_count (guint mask)
{
  guint count = 0;

  while (mask) {
    count += mask & 1U;
    mask >>= 1;
  }
  return count;
}

static GstVvasTileCompositionLease *
gst_vvas_tile_composition_lease_new_locked (GstVvasTileCompositionCoordinator *
    coordinator, GstVvasTileCompositionSlot * slot,
    GstVvasTileCompositionLeaseRole role, guint tile_id)
{
  GstVvasTileCompositionLease *lease =
      g_slice_new0 (GstVvasTileCompositionLease);

  gst_mini_object_init (GST_MINI_OBJECT_CAST (lease), 0,
      gst_vvas_tile_composition_lease_get_type (), NULL, NULL,
      (GstMiniObjectFreeFunction) gst_vvas_tile_composition_lease_free);
  lease->coordinator = gst_vvas_tile_composition_coordinator_ref (coordinator);
  lease->identity.slot_id = slot->slot_id;
  lease->identity.epoch = slot->epoch;
  lease->identity.layout_version = slot->layout_version;
  lease->identity.role = role;
  lease->identity.tile_id = tile_id;
  return lease;
}

static GstVvasTileCompositionResult
    gst_vvas_tile_composition_validate_identity_locked
    (GstVvasTileCompositionCoordinator * coordinator,
    const GstVvasTileCompositionIdentity * identity,
    GstVvasTileCompositionSlot ** result_slot)
{
  GstVvasTileCompositionSlot *slot;

  if (!identity || identity->slot_id >= coordinator->max_slots)
    return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;

  slot = &coordinator->slots[identity->slot_id];
  if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_FREE ||
      slot->epoch != identity->epoch ||
      slot->layout_version != identity->layout_version) {
    coordinator->stats.stale_callbacks++;
    return GST_VVAS_TILE_COMPOSITION_RESULT_STALE;
  }

  if (result_slot)
    *result_slot = slot;
  return GST_VVAS_TILE_COMPOSITION_RESULT_OK;
}

static gboolean
gst_vvas_tile_composition_reset_slot_locked (GstVvasTileCompositionCoordinator *
    coordinator, GstVvasTileCompositionSlot * slot,
    GstVvasTileCompositionIdentity * unreserve_identity,
    gboolean * unreserve_needed)
{
  gboolean drop_epoch_ref = slot->epoch_ref_held;
  guint slot_id = slot->slot_id;
  guint64 epoch = slot->epoch;

  if (unreserve_needed)
    *unreserve_needed = slot->storage_reserved &&
        coordinator->unreserve_slot != NULL;
  if (unreserve_identity && slot->storage_reserved) {
    unreserve_identity->slot_id = slot->slot_id;
    unreserve_identity->epoch = slot->epoch;
    unreserve_identity->layout_version = slot->layout_version;
    unreserve_identity->role = GST_VVAS_TILE_COMPOSITION_LEASE_TILE;
    unreserve_identity->tile_id = G_MAXUINT;
  }

  slot->required_mask = 0;
  slot->issued_mask = 0;
  slot->ready_mask = 0;
  slot->consumed_mask = 0;
  slot->cancelled_mask = 0;
  slot->active_tile_leases = 0;
  slot->active_output_leases = 0;
  slot->output_returned = FALSE;
  slot->epoch_ref_held = FALSE;
  slot->storage_reserved = FALSE;
  slot->reservation_in_flight = FALSE;
  slot->abort_reason = GST_VVAS_TILE_COMPOSITION_ABORT_NONE;
  gst_vvas_tile_composition_slot_transition (coordinator, slot,
      GST_VVAS_TILE_COMPOSITION_SLOT_FREE);
  GST_DEBUG ("slot=%u epoch=%" G_GUINT64_FORMAT " returned to FREE",
      slot_id, epoch);
  g_cond_broadcast (&coordinator->condition);
  return drop_epoch_ref;
}

static gboolean
    gst_vvas_tile_composition_try_free_slot_locked
    (GstVvasTileCompositionCoordinator * coordinator,
    GstVvasTileCompositionSlot * slot,
    GstVvasTileCompositionIdentity * unreserve_identity,
    gboolean * unreserve_needed)
{
  if (slot->active_tile_leases || slot->active_output_leases ||
      slot->reservation_in_flight)
    return FALSE;

  if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING)
    return gst_vvas_tile_composition_reset_slot_locked (coordinator, slot,
        unreserve_identity, unreserve_needed);

  if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_RETIRING
      && slot->output_returned)
    return gst_vvas_tile_composition_reset_slot_locked (coordinator, slot,
        unreserve_identity, unreserve_needed);

  return FALSE;
}

static void
gst_vvas_tile_composition_abort_slot_locked (GstVvasTileCompositionCoordinator *
    coordinator, GstVvasTileCompositionSlot * slot,
    GstVvasTileCompositionAbortReason reason)
{
  if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_FREE ||
      slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM ||
      slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_RETIRING ||
      slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING)
    return;

  slot->abort_reason = reason;
  coordinator->stats.epochs_aborted++;
  gst_vvas_tile_composition_slot_transition (coordinator, slot,
      GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING);
}

static GstVvasTileCompositionSlot
    * gst_vvas_tile_composition_find_oldest_eligible_locked
    (GstVvasTileCompositionCoordinator * coordinator, guint tile_mask,
    gboolean * wait_for_reservation)
{
  GstVvasTileCompositionSlot *best = NULL;
  guint i;

  *wait_for_reservation = FALSE;
  for (i = 0; i < coordinator->max_slots; i++) {
    GstVvasTileCompositionSlot *slot = &coordinator->slots[i];

    if ((slot->state != GST_VVAS_TILE_COMPOSITION_SLOT_WRITING &&
            slot->state != GST_VVAS_TILE_COMPOSITION_SLOT_RESERVING) ||
        !(slot->required_mask & tile_mask) || (slot->issued_mask & tile_mask))
      continue;

    if (!best || slot->epoch < best->epoch)
      best = slot;
  }

  if (best && best->state == GST_VVAS_TILE_COMPOSITION_SLOT_RESERVING)
    *wait_for_reservation = TRUE;
  return best;
}

static GstVvasTileCompositionSlot *
gst_vvas_tile_composition_find_free_locked (GstVvasTileCompositionCoordinator *
    coordinator)
{
  guint i;

  for (i = 0; i < coordinator->max_slots; i++) {
    if (coordinator->slots[i].state == GST_VVAS_TILE_COMPOSITION_SLOT_FREE)
      return &coordinator->slots[i];
  }
  return NULL;
}

GstVvasTileCompositionCoordinator *
gst_vvas_tile_composition_coordinator_new (GstVvasTileCompositionLayout *
    layout, guint max_slots, guint required_tile_mask, GError ** error)
{
  return gst_vvas_tile_composition_coordinator_new_full (layout, max_slots,
      required_tile_mask, NULL, NULL, NULL, NULL, error);
}

GstVvasTileCompositionCoordinator *
gst_vvas_tile_composition_coordinator_new_full (GstVvasTileCompositionLayout *
    layout, guint max_slots, guint required_tile_mask,
    GstVvasTileCompositionReserveSlotFunc reserve_slot,
    GstVvasTileCompositionUnreserveSlotFunc unreserve_slot, gpointer user_data,
    GDestroyNotify notify, GError ** error)
{
  GstVvasTileCompositionCoordinator *coordinator;
  guint valid_mask = (1U << GST_VVAS_TILE_COMPOSITION_TILE_COUNT) - 1;
  guint i;

  g_return_val_if_fail (layout != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  gst_vvas_tile_composition_coordinator_ensure_debug_category ();
  if (!max_slots || !required_tile_mask || (required_tile_mask & ~valid_mask)) {
    g_set_error (error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED,
        "invalid coordinator configuration slots=%u required-mask=0x%x",
        max_slots, required_tile_mask);
    return NULL;
  }

  coordinator = g_slice_new0 (GstVvasTileCompositionCoordinator);
  gst_mini_object_init (GST_MINI_OBJECT_CAST (coordinator), 0,
      gst_vvas_tile_composition_coordinator_get_type (), NULL, NULL,
      (GstMiniObjectFreeFunction) gst_vvas_tile_composition_coordinator_free);
  g_mutex_init (&coordinator->lock);
  g_cond_init (&coordinator->condition);
  coordinator->layout = gst_vvas_tile_composition_layout_ref (layout);
  coordinator->max_slots = max_slots;
  coordinator->required_tile_mask = required_tile_mask;
  coordinator->next_epoch = 1;
  coordinator->reserve_slot = reserve_slot;
  coordinator->unreserve_slot = unreserve_slot;
  coordinator->storage_user_data = user_data;
  coordinator->storage_notify = notify;
  coordinator->slots = g_new0 (GstVvasTileCompositionSlot, max_slots);

  for (i = 0; i < max_slots; i++) {
    coordinator->slots[i].slot_id = i;
    coordinator->slots[i].state = GST_VVAS_TILE_COMPOSITION_SLOT_FREE;
    coordinator->slots[i].epoch = 0;
  }

  GST_INFO ("created coordinator layout=%" G_GUINT64_FORMAT
      " slots=%u required-mask=0x%x",
      gst_vvas_tile_composition_layout_get_version (layout), max_slots,
      required_tile_mask);
  return coordinator;
}

GstVvasTileCompositionCoordinator *
gst_vvas_tile_composition_coordinator_ref (GstVvasTileCompositionCoordinator *
    coordinator)
{
  g_return_val_if_fail (coordinator != NULL, NULL);
  return (GstVvasTileCompositionCoordinator *)
      gst_mini_object_ref (GST_MINI_OBJECT_CAST (coordinator));
}

void
gst_vvas_tile_composition_coordinator_unref (GstVvasTileCompositionCoordinator *
    coordinator)
{
  g_return_if_fail (coordinator != NULL);
  gst_mini_object_unref (GST_MINI_OBJECT_CAST (coordinator));
}

GstVvasTileCompositionLease *
gst_vvas_tile_composition_lease_ref (GstVvasTileCompositionLease * lease)
{
  g_return_val_if_fail (lease != NULL, NULL);
  return (GstVvasTileCompositionLease *)
      gst_mini_object_ref (GST_MINI_OBJECT_CAST (lease));
}

void
gst_vvas_tile_composition_lease_unref (GstVvasTileCompositionLease * lease)
{
  g_return_if_fail (lease != NULL);
  gst_mini_object_unref (GST_MINI_OBJECT_CAST (lease));
}

GstVvasTileCompositionLeaseRole
gst_vvas_tile_composition_lease_get_role (const GstVvasTileCompositionLease *
    lease)
{
  g_return_val_if_fail (lease != NULL, GST_VVAS_TILE_COMPOSITION_LEASE_TILE);
  return lease->identity.role;
}

guint
gst_vvas_tile_composition_lease_get_tile_id (const GstVvasTileCompositionLease *
    lease)
{
  g_return_val_if_fail (lease != NULL, G_MAXUINT);
  return lease->identity.tile_id;
}

void
gst_vvas_tile_composition_lease_get_identity (const GstVvasTileCompositionLease
    * lease, GstVvasTileCompositionIdentity * identity)
{
  g_return_if_fail (lease != NULL);
  g_return_if_fail (identity != NULL);
  *identity = lease->identity;
}

GstVvasTileCompositionResult
    gst_vvas_tile_composition_coordinator_acquire_tile
    (GstVvasTileCompositionCoordinator * coordinator, guint tile_id,
    gboolean dont_wait, GstVvasTileCompositionLease ** result_lease) {
  GstVvasTileCompositionSlot *slot;
  GstVvasTileCompositionLease *lease;
  GstVvasTileCompositionIdentity reservation_identity = { 0 };
  guint tile_mask;
  gboolean wait_for_reservation;
  gboolean reservation_succeeded;
  gboolean reservation_flushed;
  gboolean reservation_identity_matches;
  gboolean drop_epoch_ref;
  gboolean unreserve_needed = FALSE;
  guint64 reservation_flush_generation = 0;
  guint64 reservation_tile_flush_generation = 0;
  GstVvasTileCompositionIdentity unreserve_identity = { 0 };

  g_return_val_if_fail (coordinator != NULL,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  g_return_val_if_fail (result_lease != NULL,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  *result_lease = NULL;

  if (tile_id >= GST_VVAS_TILE_COMPOSITION_TILE_COUNT)
    return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
  tile_mask = 1U << tile_id;
  if (!(coordinator->required_tile_mask & tile_mask))
    return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;

  g_mutex_lock (&coordinator->lock);
  while (TRUE) {
    if (coordinator->flushing || coordinator->stopping) {
      g_mutex_unlock (&coordinator->lock);
      return GST_VVAS_TILE_COMPOSITION_RESULT_FLUSHING;
    }
    if ((coordinator->tile_flushing_mask & tile_mask) ||
        (coordinator->pool_flushing_mask & tile_mask)) {
      g_mutex_unlock (&coordinator->lock);
      return GST_VVAS_TILE_COMPOSITION_RESULT_FLUSHING;
    }
    /* Only a pipeline flush holds back the other tiles. A producer pool
     * flushes routinely while playing, and stalling every tile on it would
     * turn one producer's transient state into a whole-composition stall. */
    if (coordinator->tile_flushing_mask) {
      if (dont_wait) {
        g_mutex_unlock (&coordinator->lock);
        return GST_VVAS_TILE_COMPOSITION_RESULT_NO_SLOT;
      }
      coordinator->stats.blocked_acquires++;
      g_cond_wait (&coordinator->condition, &coordinator->lock);
      continue;
    }

    slot = gst_vvas_tile_composition_find_oldest_eligible_locked (coordinator,
        tile_mask, &wait_for_reservation);
    if (slot && wait_for_reservation) {
      if (dont_wait) {
        g_mutex_unlock (&coordinator->lock);
        return GST_VVAS_TILE_COMPOSITION_RESULT_NO_SLOT;
      }
      coordinator->stats.blocked_acquires++;
      g_cond_wait (&coordinator->condition, &coordinator->lock);
      continue;
    }

    if (!slot) {
      slot = gst_vvas_tile_composition_find_free_locked (coordinator);
      if (!slot) {
        if (dont_wait) {
          g_mutex_unlock (&coordinator->lock);
          return GST_VVAS_TILE_COMPOSITION_RESULT_NO_SLOT;
        }
        coordinator->stats.blocked_acquires++;
        g_cond_wait (&coordinator->condition, &coordinator->lock);
        continue;
      }

      if (coordinator->next_epoch == G_MAXUINT64) {
        g_mutex_unlock (&coordinator->lock);
        return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
      }

      slot->epoch = coordinator->next_epoch++;
      slot->layout_version =
          gst_vvas_tile_composition_layout_get_version (coordinator->layout);
      slot->required_mask = coordinator->required_tile_mask;
      slot->issued_mask = tile_mask;
      slot->epoch_ref_held = TRUE;
      slot->reservation_in_flight = TRUE;
      reservation_flush_generation = coordinator->flush_generation;
      reservation_tile_flush_generation =
          coordinator->tile_flush_generation[tile_id];
      gst_vvas_tile_composition_coordinator_ref (coordinator);
      gst_vvas_tile_composition_slot_transition (coordinator, slot,
          GST_VVAS_TILE_COMPOSITION_SLOT_RESERVING);
      coordinator->stats.epochs_opened++;

      reservation_identity.slot_id = slot->slot_id;
      reservation_identity.epoch = slot->epoch;
      reservation_identity.layout_version = slot->layout_version;
      reservation_identity.role = GST_VVAS_TILE_COMPOSITION_LEASE_TILE;
      reservation_identity.tile_id = tile_id;
      g_mutex_unlock (&coordinator->lock);

      reservation_succeeded = !coordinator->reserve_slot ||
          coordinator->reserve_slot (&reservation_identity,
          coordinator->storage_user_data);

      g_mutex_lock (&coordinator->lock);
      reservation_identity_matches =
          slot->epoch == reservation_identity.epoch &&
          slot->layout_version == reservation_identity.layout_version;
      if (reservation_identity_matches)
        slot->reservation_in_flight = FALSE;
      /* The lock was dropped across the storage reservation, so a pool flush
       * may have started meanwhile. Issuing a lease then would hand a buffer
       * to a producer whose pool is flushing; the epoch opened here holds only
       * this tile, so dropping it costs nothing. */
      reservation_flushed = coordinator->flushing || coordinator->stopping ||
          (coordinator->pool_flushing_mask & tile_mask) ||
          coordinator->flush_generation != reservation_flush_generation ||
          coordinator->tile_flush_generation[tile_id] !=
          reservation_tile_flush_generation;
      if (slot->state != GST_VVAS_TILE_COMPOSITION_SLOT_RESERVING ||
          !reservation_identity_matches ||
          !reservation_succeeded || reservation_flushed) {
        if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_RESERVING &&
            slot->epoch == reservation_identity.epoch) {
          slot->cancelled_mask |= tile_mask;
          gst_vvas_tile_composition_abort_slot_locked (coordinator, slot,
              reservation_flushed ?
              GST_VVAS_TILE_COMPOSITION_ABORT_FLUSHING :
              GST_VVAS_TILE_COMPOSITION_ABORT_EXPLICIT);
        }
        drop_epoch_ref =
            gst_vvas_tile_composition_try_free_slot_locked (coordinator, slot,
            &unreserve_identity, &unreserve_needed);
        g_mutex_unlock (&coordinator->lock);

        if (unreserve_needed)
          coordinator->unreserve_slot (&unreserve_identity,
              coordinator->storage_user_data);
        if (reservation_succeeded && coordinator->unreserve_slot)
          coordinator->unreserve_slot (&reservation_identity,
              coordinator->storage_user_data);
        if (drop_epoch_ref)
          gst_vvas_tile_composition_coordinator_unref (coordinator);
        return reservation_flushed ?
            GST_VVAS_TILE_COMPOSITION_RESULT_FLUSHING :
            GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
      }

      slot->storage_reserved = TRUE;
      gst_vvas_tile_composition_slot_transition (coordinator, slot,
          GST_VVAS_TILE_COMPOSITION_SLOT_WRITING);
    } else {
      slot->issued_mask |= tile_mask;
    }

    slot->active_tile_leases++;
    coordinator->stats.tile_leases_issued++;
    lease = gst_vvas_tile_composition_lease_new_locked (coordinator, slot,
        GST_VVAS_TILE_COMPOSITION_LEASE_TILE, tile_id);
    GST_LOG ("issued slot=%u epoch=%" G_GUINT64_FORMAT " tile=%u",
        slot->slot_id, slot->epoch, tile_id);
    *result_lease = lease;
    g_mutex_unlock (&coordinator->lock);
    return GST_VVAS_TILE_COMPOSITION_RESULT_OK;
  }
}

GstVvasTileCompositionResult
    gst_vvas_tile_composition_coordinator_mark_tile_ready
    (GstVvasTileCompositionLease * lease, GstClockTime pts,
    GstClockTime duration) {
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionSlot *slot = NULL;
  GstVvasTileCompositionResult result;
  guint tile_mask;

  g_return_val_if_fail (lease != NULL,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  if (lease->identity.role != GST_VVAS_TILE_COMPOSITION_LEASE_TILE
      || lease->identity.tile_id >= GST_VVAS_TILE_COMPOSITION_TILE_COUNT)
    return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;

  coordinator = lease->coordinator;
  tile_mask = 1U << lease->identity.tile_id;
  g_mutex_lock (&coordinator->lock);
  result = gst_vvas_tile_composition_validate_identity_locked (coordinator,
      &lease->identity, &slot);
  if (result != GST_VVAS_TILE_COMPOSITION_RESULT_OK)
    goto done;
  if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING) {
    result = GST_VVAS_TILE_COMPOSITION_RESULT_ABORTED;
    goto done;
  }
  if (!(slot->issued_mask & tile_mask)) {
    result = GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
    goto done;
  }
  if (!slot->storage_reserved) {
    GST_ERROR ("tile ready without reserved storage slot=%u epoch=%"
        G_GUINT64_FORMAT " tile=%u", slot->slot_id, slot->epoch,
        lease->identity.tile_id);
    result = GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
    goto done;
  }
  if (slot->ready_mask & tile_mask) {
    coordinator->stats.duplicate_callbacks++;
    result = GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE;
    goto done;
  }
  if (slot->state != GST_VVAS_TILE_COMPOSITION_SLOT_WRITING) {
    result = GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
    goto done;
  }

  slot->ready_mask |= tile_mask;
  slot->tile_pts[lease->identity.tile_id] = pts;
  slot->tile_duration[lease->identity.tile_id] = duration;
  coordinator->stats.tile_leases_ready++;
  if ((slot->ready_mask & slot->required_mask) == slot->required_mask) {
    gst_vvas_tile_composition_slot_transition (coordinator, slot,
        GST_VVAS_TILE_COMPOSITION_SLOT_READY);
    coordinator->stats.epochs_ready++;
    g_cond_broadcast (&coordinator->condition);
  }
  result = GST_VVAS_TILE_COMPOSITION_RESULT_OK;

done:
  g_mutex_unlock (&coordinator->lock);
  return result;
}

GstVvasTileCompositionResult
gst_vvas_tile_composition_coordinator_cancel_tile (GstVvasTileCompositionLease *
    lease, GstVvasTileCompositionAbortReason reason)
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionSlot *slot = NULL;
  GstVvasTileCompositionResult result;
  guint tile_mask;

  g_return_val_if_fail (lease != NULL,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  if (lease->identity.role != GST_VVAS_TILE_COMPOSITION_LEASE_TILE
      || lease->identity.tile_id >= GST_VVAS_TILE_COMPOSITION_TILE_COUNT)
    return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;

  coordinator = lease->coordinator;
  tile_mask = 1U << lease->identity.tile_id;
  g_mutex_lock (&coordinator->lock);
  result = gst_vvas_tile_composition_validate_identity_locked (coordinator,
      &lease->identity, &slot);
  if (result != GST_VVAS_TILE_COMPOSITION_RESULT_OK)
    goto done;
  if (slot->consumed_mask & tile_mask) {
    coordinator->stats.duplicate_callbacks++;
    result = GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE;
    goto done;
  }
  if (slot->cancelled_mask & tile_mask) {
    coordinator->stats.duplicate_callbacks++;
    result = GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE;
    goto done;
  }

  slot->cancelled_mask |= tile_mask;
  gst_vvas_tile_composition_abort_slot_locked (coordinator, slot,
      reason == GST_VVAS_TILE_COMPOSITION_ABORT_NONE ?
      GST_VVAS_TILE_COMPOSITION_ABORT_EXPLICIT : reason);
  result = GST_VVAS_TILE_COMPOSITION_RESULT_OK;

done:
  g_mutex_unlock (&coordinator->lock);
  return result;
}

GstVvasTileCompositionResult
    gst_vvas_tile_composition_coordinator_abort_epoch
    (GstVvasTileCompositionCoordinator * coordinator,
    const GstVvasTileCompositionIdentity * identity,
    GstVvasTileCompositionAbortReason reason)
{
  GstVvasTileCompositionSlot *slot = NULL;
  GstVvasTileCompositionResult result;
  gboolean drop_epoch_ref = FALSE;
  gboolean unreserve_needed = FALSE;
  GstVvasTileCompositionIdentity unreserve_identity = { 0 };

  g_return_val_if_fail (coordinator != NULL,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  g_return_val_if_fail (identity != NULL,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);

  g_mutex_lock (&coordinator->lock);
  result =
      gst_vvas_tile_composition_validate_identity_locked (coordinator, identity,
      &slot);
  if (result != GST_VVAS_TILE_COMPOSITION_RESULT_OK)
    goto done;
  if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM ||
      slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_RETIRING) {
    result = GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
    goto done;
  }
  if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING) {
    coordinator->stats.duplicate_callbacks++;
    result = GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE;
    goto done;
  }

  gst_vvas_tile_composition_abort_slot_locked (coordinator, slot,
      reason == GST_VVAS_TILE_COMPOSITION_ABORT_NONE ?
      GST_VVAS_TILE_COMPOSITION_ABORT_EXPLICIT : reason);
  drop_epoch_ref =
      gst_vvas_tile_composition_try_free_slot_locked (coordinator, slot,
      &unreserve_identity, &unreserve_needed);
  result = GST_VVAS_TILE_COMPOSITION_RESULT_OK;

done:
  g_mutex_unlock (&coordinator->lock);
  if (unreserve_needed)
    coordinator->unreserve_slot (&unreserve_identity,
        coordinator->storage_user_data);
  if (drop_epoch_ref)
    gst_vvas_tile_composition_coordinator_unref (coordinator);
  return result;
}

static gboolean
    gst_vvas_tile_composition_has_older_unpublished_locked
    (GstVvasTileCompositionCoordinator * coordinator,
    GstVvasTileCompositionSlot * slot)
{
  guint i;

  for (i = 0; i < coordinator->max_slots; i++) {
    GstVvasTileCompositionSlot *older = &coordinator->slots[i];

    if (older->state != GST_VVAS_TILE_COMPOSITION_SLOT_FREE &&
        older->state != GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING &&
        older->state != GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM &&
        older->state != GST_VVAS_TILE_COMPOSITION_SLOT_RETIRING &&
        older->epoch < slot->epoch)
      return TRUE;
  }
  return FALSE;
}

GstVvasTileCompositionResult
    gst_vvas_tile_composition_coordinator_begin_processing
    (GstVvasTileCompositionCoordinator * coordinator,
    GstVvasTileCompositionLease * const *tile_leases, guint n_tile_leases,
    GstVvasTileCompositionLease ** result_output_lease)
{
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionSlot *slot = NULL;
  GstVvasTileCompositionLease *output_lease;
  GstVvasTileCompositionResult result;
  guint expected_count;
  guint tile_mask = 0;
  guint i;

  g_return_val_if_fail (coordinator != NULL,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  g_return_val_if_fail (tile_leases != NULL,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  g_return_val_if_fail (result_output_lease != NULL,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  *result_output_lease = NULL;

  expected_count =
      gst_vvas_tile_composition_mask_count (coordinator->required_tile_mask);
  if (n_tile_leases != expected_count || !n_tile_leases)
    return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
  if (!tile_leases[0])
    return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;

  gst_vvas_tile_composition_lease_get_identity (tile_leases[0], &identity);
  for (i = 0; i < n_tile_leases; i++) {
    GstVvasTileCompositionIdentity current;
    guint current_mask;

    if (!tile_leases[i] ||
        tile_leases[i]->coordinator != coordinator ||
        tile_leases[i]->identity.role != GST_VVAS_TILE_COMPOSITION_LEASE_TILE ||
        tile_leases[i]->identity.tile_id >=
        GST_VVAS_TILE_COMPOSITION_TILE_COUNT)
      return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;

    current = tile_leases[i]->identity;
    if (current.slot_id != identity.slot_id ||
        current.epoch != identity.epoch ||
        current.layout_version != identity.layout_version)
      return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;

    current_mask = 1U << current.tile_id;
    if (tile_mask & current_mask)
      return GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE;
    tile_mask |= current_mask;
  }

  if (tile_mask != coordinator->required_tile_mask)
    return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;

  g_mutex_lock (&coordinator->lock);
  result =
      gst_vvas_tile_composition_validate_identity_locked (coordinator,
      &identity, &slot);
  if (result != GST_VVAS_TILE_COMPOSITION_RESULT_OK)
    goto done;
  if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING
      || slot->cancelled_mask) {
    result = GST_VVAS_TILE_COMPOSITION_RESULT_ABORTED;
    goto done;
  }
  if (slot->state != GST_VVAS_TILE_COMPOSITION_SLOT_READY ||
      slot->ready_mask != slot->required_mask) {
    result = GST_VVAS_TILE_COMPOSITION_RESULT_INCOMPLETE;
    goto done;
  }

  if (gst_vvas_tile_composition_has_older_unpublished_locked (coordinator,
          slot)) {
    coordinator->stats.publication_waits++;
    GST_LOG ("publication barrier slot=%u epoch=%" G_GUINT64_FORMAT,
        slot->slot_id, slot->epoch);
    result = GST_VVAS_TILE_COMPOSITION_RESULT_WAITING;
    goto done;
  }

  slot->consumed_mask = slot->required_mask;
  slot->active_output_leases++;
  slot->output_returned = FALSE;
  gst_vvas_tile_composition_slot_transition (coordinator, slot,
      GST_VVAS_TILE_COMPOSITION_SLOT_PROCESSING);
  output_lease = gst_vvas_tile_composition_lease_new_locked (coordinator, slot,
      GST_VVAS_TILE_COMPOSITION_LEASE_OUTPUT, G_MAXUINT);
  *result_output_lease = output_lease;
  result = GST_VVAS_TILE_COMPOSITION_RESULT_OK;

done:
  g_mutex_unlock (&coordinator->lock);
  return result;
}

GstVvasTileCompositionResult
    gst_vvas_tile_composition_coordinator_wait_until_publishable
    (GstVvasTileCompositionLease * tile_lease, GstClockTime timeout) {
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionSlot *slot = NULL;
  GstVvasTileCompositionResult result;
  gint64 now;
  gint64 timeout_us;
  gint64 deadline;

  g_return_val_if_fail (tile_lease != NULL,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  if (tile_lease->identity.role != GST_VVAS_TILE_COMPOSITION_LEASE_TILE)
    return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;

  coordinator = tile_lease->coordinator;
  now = g_get_monotonic_time ();
  timeout_us = timeout == GST_CLOCK_TIME_NONE ? G_MAXINT64 :
      (gint64) MIN (timeout / 1000, (GstClockTime) G_MAXINT64);
  deadline = timeout_us == G_MAXINT64 || timeout_us > G_MAXINT64 - now ?
      G_MAXINT64 : now + timeout_us;

  g_mutex_lock (&coordinator->lock);
  while (TRUE) {
    result = gst_vvas_tile_composition_validate_identity_locked (coordinator,
        &tile_lease->identity, &slot);
    if (result != GST_VVAS_TILE_COMPOSITION_RESULT_OK)
      break;
    if (coordinator->flushing || coordinator->stopping ||
        (coordinator->tile_flushing_mask &
            (1U << tile_lease->identity.tile_id))) {
      result = GST_VVAS_TILE_COMPOSITION_RESULT_FLUSHING;
      break;
    }
    if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING) {
      result = GST_VVAS_TILE_COMPOSITION_RESULT_ABORTED;
      break;
    }
    if (slot->state != GST_VVAS_TILE_COMPOSITION_SLOT_READY) {
      result = GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
      break;
    }
    if (!gst_vvas_tile_composition_has_older_unpublished_locked (coordinator,
            slot)) {
      result = GST_VVAS_TILE_COMPOSITION_RESULT_OK;
      break;
    }
    if (!timeout || !g_cond_wait_until (&coordinator->condition,
            &coordinator->lock, deadline)) {
      coordinator->stats.publication_wait_timeouts++;
      GST_DEBUG ("publication wait timed out slot=%u epoch=%"
          G_GUINT64_FORMAT, slot->slot_id, slot->epoch);
      result = GST_VVAS_TILE_COMPOSITION_RESULT_WAITING;
      break;
    }
  }
  g_mutex_unlock (&coordinator->lock);
  return result;
}

GstVvasTileCompositionResult
    gst_vvas_tile_composition_coordinator_complete_processing
    (GstVvasTileCompositionLease * output_lease) {
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasTileCompositionSlot *slot = NULL;
  GstVvasTileCompositionResult result;

  g_return_val_if_fail (output_lease != NULL,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  if (output_lease->identity.role != GST_VVAS_TILE_COMPOSITION_LEASE_OUTPUT)
    return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;

  coordinator = output_lease->coordinator;
  g_mutex_lock (&coordinator->lock);
  result = gst_vvas_tile_composition_validate_identity_locked (coordinator,
      &output_lease->identity, &slot);
  if (result != GST_VVAS_TILE_COMPOSITION_RESULT_OK)
    goto done;
  if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING) {
    result = slot->abort_reason == GST_VVAS_TILE_COMPOSITION_ABORT_FLUSHING ?
        GST_VVAS_TILE_COMPOSITION_RESULT_FLUSHING :
        GST_VVAS_TILE_COMPOSITION_RESULT_ABORTED;
    goto done;
  }
  if (slot->state != GST_VVAS_TILE_COMPOSITION_SLOT_PROCESSING) {
    coordinator->stats.duplicate_callbacks++;
    result = GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE;
    goto done;
  }
  if (slot->epoch <= coordinator->last_published_epoch) {
    gst_vvas_tile_composition_abort_slot_locked (coordinator, slot,
        GST_VVAS_TILE_COMPOSITION_ABORT_INVALID_SET);
    result = GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
    goto done;
  }

  gst_vvas_tile_composition_slot_transition (coordinator, slot,
      GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM);
  coordinator->last_published_epoch = slot->epoch;
  coordinator->stats.last_published_epoch = slot->epoch;
  coordinator->stats.epochs_published++;
  result = GST_VVAS_TILE_COMPOSITION_RESULT_OK;

done:
  g_mutex_unlock (&coordinator->lock);
  return result;
}

GstVvasTileCompositionResult
    gst_vvas_tile_composition_coordinator_mark_output_returned
    (GstVvasTileCompositionCoordinator * coordinator,
    const GstVvasTileCompositionIdentity * identity)
{
  GstVvasTileCompositionSlot *slot = NULL;
  GstVvasTileCompositionResult result;
  gboolean drop_epoch_ref = FALSE;
  gboolean unreserve_needed = FALSE;
  GstVvasTileCompositionIdentity unreserve_identity = { 0 };

  g_return_val_if_fail (coordinator != NULL,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  if (!identity || identity->role != GST_VVAS_TILE_COMPOSITION_LEASE_OUTPUT)
    return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;

  g_mutex_lock (&coordinator->lock);
  result =
      gst_vvas_tile_composition_validate_identity_locked (coordinator, identity,
      &slot);
  if (result != GST_VVAS_TILE_COMPOSITION_RESULT_OK)
    goto done;
  if (slot->state != GST_VVAS_TILE_COMPOSITION_SLOT_PROCESSING &&
      slot->state != GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM &&
      slot->state != GST_VVAS_TILE_COMPOSITION_SLOT_ABORTING &&
      slot->state != GST_VVAS_TILE_COMPOSITION_SLOT_RETIRING) {
    result = GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
    goto done;
  }
  if (slot->output_returned) {
    coordinator->stats.duplicate_callbacks++;
    result = GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE;
    goto done;
  }

  slot->output_returned = TRUE;
  if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_PROCESSING) {
    gst_vvas_tile_composition_abort_slot_locked (coordinator, slot,
        GST_VVAS_TILE_COMPOSITION_ABORT_PROCESSING_FAILED);
  } else if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM) {
    gst_vvas_tile_composition_slot_transition (coordinator, slot,
        GST_VVAS_TILE_COMPOSITION_SLOT_RETIRING);
  }
  drop_epoch_ref =
      gst_vvas_tile_composition_try_free_slot_locked (coordinator, slot,
      &unreserve_identity, &unreserve_needed);
  result = GST_VVAS_TILE_COMPOSITION_RESULT_OK;

done:
  g_mutex_unlock (&coordinator->lock);
  if (unreserve_needed)
    coordinator->unreserve_slot (&unreserve_identity,
        coordinator->storage_user_data);
  if (drop_epoch_ref)
    gst_vvas_tile_composition_coordinator_unref (coordinator);
  return result;
}

GstVvasTileCompositionResult
    gst_vvas_tile_composition_output_lease_mark_returned
    (GstVvasTileCompositionLease * output_lease) {
  g_return_val_if_fail (output_lease != NULL,
      GST_VVAS_TILE_COMPOSITION_RESULT_INVALID);
  if (output_lease->identity.role != GST_VVAS_TILE_COMPOSITION_LEASE_OUTPUT)
    return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;

  return
      gst_vvas_tile_composition_coordinator_mark_output_returned
      (output_lease->coordinator, &output_lease->identity);
}

void gst_vvas_tile_composition_coordinator_set_flushing
    (GstVvasTileCompositionCoordinator * coordinator, gboolean flushing)
{
  guint drop_epoch_refs = 0;
  guint unreserve_count = 0;
  guint i;
  GstVvasTileCompositionIdentity *unreserve_identities;

  g_return_if_fail (coordinator != NULL);
  unreserve_identities =
      g_new0 (GstVvasTileCompositionIdentity, coordinator->max_slots);
  g_mutex_lock (&coordinator->lock);
  if (!flushing && coordinator->stopping) {
    g_mutex_unlock (&coordinator->lock);
    g_free (unreserve_identities);
    return;
  }

  if (flushing && !coordinator->flushing)
    coordinator->flush_generation++;
  coordinator->flushing = flushing;
  if (flushing) {
    for (i = 0; i < coordinator->max_slots; i++) {
      GstVvasTileCompositionSlot *slot = &coordinator->slots[i];
      gboolean unreserve_needed = FALSE;

      gst_vvas_tile_composition_abort_slot_locked (coordinator, slot,
          GST_VVAS_TILE_COMPOSITION_ABORT_FLUSHING);
      if (gst_vvas_tile_composition_try_free_slot_locked (coordinator, slot,
              &unreserve_identities[unreserve_count], &unreserve_needed))
        drop_epoch_refs++;
      if (unreserve_needed)
        unreserve_count++;
    }
  }
  g_cond_broadcast (&coordinator->condition);
  g_mutex_unlock (&coordinator->lock);

  for (i = 0; i < unreserve_count; i++)
    coordinator->unreserve_slot (&unreserve_identities[i],
        coordinator->storage_user_data);
  g_free (unreserve_identities);
  while (drop_epoch_refs--)
    gst_vvas_tile_composition_coordinator_unref (coordinator);
}

void gst_vvas_tile_composition_coordinator_set_tile_flushing
    (GstVvasTileCompositionCoordinator * coordinator, guint tile_id,
    gboolean flushing)
{
  GstVvasTileCompositionIdentity *unreserve_identities;
  guint drop_epoch_refs = 0;
  guint unreserve_count = 0;
  guint tile_mask;
  guint i;

  g_return_if_fail (coordinator != NULL);
  g_return_if_fail (tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT);
  tile_mask = 1U << tile_id;
  unreserve_identities =
      g_new0 (GstVvasTileCompositionIdentity, coordinator->max_slots);

  g_mutex_lock (&coordinator->lock);
  if (flushing) {
    if (!(coordinator->tile_flushing_mask & tile_mask))
      coordinator->tile_flush_generation[tile_id]++;
    coordinator->tile_flushing_mask |= tile_mask;
    for (i = 0; i < coordinator->max_slots; i++) {
      GstVvasTileCompositionSlot *slot = &coordinator->slots[i];
      gboolean unreserve_needed = FALSE;

      if (slot->state != GST_VVAS_TILE_COMPOSITION_SLOT_FREE &&
          slot->state != GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM &&
          slot->state != GST_VVAS_TILE_COMPOSITION_SLOT_RETIRING)
        gst_vvas_tile_composition_abort_slot_locked (coordinator, slot,
            GST_VVAS_TILE_COMPOSITION_ABORT_FLUSHING);
      if (gst_vvas_tile_composition_try_free_slot_locked (coordinator, slot,
              &unreserve_identities[unreserve_count], &unreserve_needed))
        drop_epoch_refs++;
      if (unreserve_needed)
        unreserve_count++;
    }
  } else {
    coordinator->tile_flushing_mask &= ~tile_mask;
  }
  g_cond_broadcast (&coordinator->condition);
  g_mutex_unlock (&coordinator->lock);

  for (i = 0; i < unreserve_count; i++)
    coordinator->unreserve_slot (&unreserve_identities[i],
        coordinator->storage_user_data);
  g_free (unreserve_identities);
  while (drop_epoch_refs--)
    gst_vvas_tile_composition_coordinator_unref (coordinator);
}

void gst_vvas_tile_composition_coordinator_set_pool_flushing
    (GstVvasTileCompositionCoordinator * coordinator, guint tile_id,
    gboolean flushing)
{
  guint tile_mask;

  g_return_if_fail (coordinator != NULL);
  g_return_if_fail (tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT);
  tile_mask = 1U << tile_id;

  g_mutex_lock (&coordinator->lock);
  if (flushing)
    coordinator->pool_flushing_mask |= tile_mask;
  else
    coordinator->pool_flushing_mask &= ~tile_mask;
  g_cond_broadcast (&coordinator->condition);
  g_mutex_unlock (&coordinator->lock);
}

void gst_vvas_tile_composition_coordinator_set_stopping
    (GstVvasTileCompositionCoordinator * coordinator)
{
  GstVvasTileCompositionIdentity *unreserve_identities;
  guint drop_epoch_refs = 0;
  guint unreserve_count = 0;
  guint i;

  g_return_if_fail (coordinator != NULL);

  g_mutex_lock (&coordinator->lock);
  coordinator->stopping = TRUE;
  g_mutex_unlock (&coordinator->lock);
  gst_vvas_tile_composition_coordinator_set_flushing (coordinator, TRUE);

  unreserve_identities =
      g_new0 (GstVvasTileCompositionIdentity, coordinator->max_slots);
  g_mutex_lock (&coordinator->lock);
  for (i = 0; i < coordinator->max_slots; i++) {
    GstVvasTileCompositionSlot *slot = &coordinator->slots[i];
    gboolean unreserve_needed = FALSE;

    if ((slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM ||
            slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_RETIRING) &&
        !slot->output_returned) {
      GST_WARNING ("force output return during stop slot=%u epoch=%"
          G_GUINT64_FORMAT " state=%s storage=%d leases=%u/%u",
          slot->slot_id, slot->epoch,
          gst_vvas_tile_composition_slot_state_name (slot->state),
          slot->storage_reserved, slot->active_tile_leases,
          slot->active_output_leases);
      slot->output_returned = TRUE;
      if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM)
        gst_vvas_tile_composition_slot_transition (coordinator, slot,
            GST_VVAS_TILE_COMPOSITION_SLOT_RETIRING);
    }

    if (gst_vvas_tile_composition_try_free_slot_locked (coordinator, slot,
            &unreserve_identities[unreserve_count], &unreserve_needed))
      drop_epoch_refs++;
    if (unreserve_needed)
      unreserve_count++;
  }
  g_cond_broadcast (&coordinator->condition);
  g_mutex_unlock (&coordinator->lock);

  for (i = 0; i < unreserve_count; i++)
    coordinator->unreserve_slot (&unreserve_identities[i],
        coordinator->storage_user_data);
  g_free (unreserve_identities);
  while (drop_epoch_refs--)
    gst_vvas_tile_composition_coordinator_unref (coordinator);
}

gboolean
    gst_vvas_tile_composition_coordinator_get_slot_snapshot
    (GstVvasTileCompositionCoordinator * coordinator, guint slot_id,
    GstVvasTileCompositionSlotSnapshot * snapshot) {
  GstVvasTileCompositionSlot *slot;

  g_return_val_if_fail (coordinator != NULL, FALSE);
  g_return_val_if_fail (snapshot != NULL, FALSE);
  if (slot_id >= coordinator->max_slots)
    return FALSE;

  g_mutex_lock (&coordinator->lock);
  slot = &coordinator->slots[slot_id];
  snapshot->slot_id = slot->slot_id;
  snapshot->epoch = slot->epoch;
  snapshot->layout_version = slot->layout_version;
  snapshot->state = slot->state;
  snapshot->required_mask = slot->required_mask;
  snapshot->issued_mask = slot->issued_mask;
  snapshot->ready_mask = slot->ready_mask;
  snapshot->consumed_mask = slot->consumed_mask;
  snapshot->cancelled_mask = slot->cancelled_mask;
  snapshot->active_tile_leases = slot->active_tile_leases;
  snapshot->active_output_leases = slot->active_output_leases;
  snapshot->output_returned = slot->output_returned;
  snapshot->storage_reserved = slot->storage_reserved;
  snapshot->reservation_in_flight = slot->reservation_in_flight;
  snapshot->abort_reason = slot->abort_reason;
  g_mutex_unlock (&coordinator->lock);
  return TRUE;
}

void gst_vvas_tile_composition_coordinator_get_stats
    (GstVvasTileCompositionCoordinator * coordinator,
    GstVvasTileCompositionStats * stats)
{
  g_return_if_fail (coordinator != NULL);
  g_return_if_fail (stats != NULL);

  g_mutex_lock (&coordinator->lock);
  *stats = coordinator->stats;
  g_mutex_unlock (&coordinator->lock);
}

static void
gst_vvas_tile_composition_lease_free (GstVvasTileCompositionLease * lease)
{
  GstVvasTileCompositionCoordinator *coordinator = lease->coordinator;
  GstVvasTileCompositionSlot *slot = NULL;
  GstVvasTileCompositionIdentity unreserve_identity = { 0 };
  GstVvasTileCompositionResult result;
  gboolean drop_epoch_ref = FALSE;
  gboolean unreserve_needed = FALSE;
  guint tile_mask = 0;

  g_mutex_lock (&coordinator->lock);
  result = gst_vvas_tile_composition_validate_identity_locked (coordinator,
      &lease->identity, &slot);
  if (result == GST_VVAS_TILE_COMPOSITION_RESULT_OK) {
    if (lease->identity.role == GST_VVAS_TILE_COMPOSITION_LEASE_TILE) {
      tile_mask = 1U << lease->identity.tile_id;
      if (!(slot->consumed_mask & tile_mask)) {
        slot->cancelled_mask |= tile_mask;
        gst_vvas_tile_composition_abort_slot_locked (coordinator, slot,
            GST_VVAS_TILE_COMPOSITION_ABORT_LEASE_DROPPED);
      }
      if (slot->active_tile_leases)
        slot->active_tile_leases--;
      else
        GST_ERROR ("tile lease underflow slot=%u epoch=%" G_GUINT64_FORMAT,
            slot->slot_id, slot->epoch);
    } else {
      if (!slot->output_returned)
        slot->output_returned = TRUE;
      if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_PROCESSING) {
        GST_ERROR ("output lease released before processing completed "
            "slot=%u epoch=%" G_GUINT64_FORMAT, slot->slot_id, slot->epoch);
        gst_vvas_tile_composition_abort_slot_locked (coordinator, slot,
            GST_VVAS_TILE_COMPOSITION_ABORT_PROCESSING_FAILED);
      } else if (slot->state == GST_VVAS_TILE_COMPOSITION_SLOT_DOWNSTREAM) {
        gst_vvas_tile_composition_slot_transition (coordinator, slot,
            GST_VVAS_TILE_COMPOSITION_SLOT_RETIRING);
      }

      if (slot->active_output_leases)
        slot->active_output_leases--;
      else
        GST_ERROR ("output lease underflow slot=%u epoch=%" G_GUINT64_FORMAT,
            slot->slot_id, slot->epoch);
    }
    drop_epoch_ref =
        gst_vvas_tile_composition_try_free_slot_locked (coordinator, slot,
        &unreserve_identity, &unreserve_needed);
    g_cond_broadcast (&coordinator->condition);
  }
  g_mutex_unlock (&coordinator->lock);

  if (unreserve_needed)
    coordinator->unreserve_slot (&unreserve_identity,
        coordinator->storage_user_data);
  if (drop_epoch_ref)
    gst_vvas_tile_composition_coordinator_unref (coordinator);
  gst_vvas_tile_composition_coordinator_unref (coordinator);
  g_slice_free (GstVvasTileCompositionLease, lease);
}

static void
gst_vvas_tile_composition_coordinator_free (GstVvasTileCompositionCoordinator *
    coordinator)
{
  guint i;

  gst_vvas_tile_composition_coordinator_ensure_debug_category ();
  for (i = 0; i < coordinator->max_slots; i++) {
    if (coordinator->slots[i].state != GST_VVAS_TILE_COMPOSITION_SLOT_FREE)
      GST_ERROR ("destroying coordinator with slot=%u state=%s epoch=%"
          G_GUINT64_FORMAT, i,
          gst_vvas_tile_composition_slot_state_name (coordinator->
              slots[i].state), coordinator->slots[i].epoch);
  }

  GST_INFO ("destroy coordinator opened=%" G_GUINT64_FORMAT
      " ready=%" G_GUINT64_FORMAT " published=%" G_GUINT64_FORMAT
      " aborted=%" G_GUINT64_FORMAT " stale=%" G_GUINT64_FORMAT,
      coordinator->stats.epochs_opened, coordinator->stats.epochs_ready,
      coordinator->stats.epochs_published, coordinator->stats.epochs_aborted,
      coordinator->stats.stale_callbacks);

  if (coordinator->storage_notify)
    coordinator->storage_notify (coordinator->storage_user_data);
  gst_vvas_tile_composition_layout_unref (coordinator->layout);
  g_free (coordinator->slots);
  g_cond_clear (&coordinator->condition);
  g_mutex_clear (&coordinator->lock);
  g_slice_free (GstVvasTileCompositionCoordinator, coordinator);
}
