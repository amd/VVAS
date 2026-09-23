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

#include <string.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>
#include "gstvvasallocator.h"
#include "gstvvasmasterpool.h"

GST_DEBUG_CATEGORY_STATIC (gst_vvas_master_pool_debug);
#define GST_CAT_DEFAULT gst_vvas_master_pool_debug

#define GST_VVAS_MASTER_POOL_ACQUIRE_SLOT \
  ((GstBufferPoolAcquireFlags) GST_BUFFER_POOL_ACQUIRE_FLAG_LAST)

typedef struct _GstVvasMasterSlotMeta
{
  GstMeta meta;
  guint slot_id;
} GstVvasMasterSlotMeta;

struct _GstVvasMasterPoolPrivate
{
  GMutex lock;
  GCond condition;
  GstVvasTileCompositionLayout *layout;
  GstVideoInfo master_info;
  GstAllocator *allocator;
  GstAllocationParams params;
  GstBuffer **available;
  GstBuffer **quarantined;
  GstBuffer **reserved;
  GstVvasTileCompositionIdentity *reserved_identity;
  GstVvasTileCompositionIdentity *transferred_identity;
  gboolean *reserved_valid;
  gboolean *transferred_valid;
  guint max_slots;
  guint next_alloc_slot;
  gboolean add_videometa;
  gboolean stopping;
};

#define parent_class gst_vvas_master_pool_parent_class
G_DEFINE_TYPE_WITH_CODE (GstVvasMasterPool, gst_vvas_master_pool,
    GST_TYPE_VIDEO_BUFFER_POOL, G_ADD_PRIVATE (GstVvasMasterPool);
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "vvasmasterbufferpool", 0,
        "VVAS tile composition master buffer pool"));

static GType
gst_vvas_master_slot_meta_api_get_type (void)
{
  static GType type = 0;
  static const gchar *tags[] = { GST_META_TAG_MEMORY_STR, NULL };

  if (g_once_init_enter (&type)) {
    GType registered =
        gst_meta_api_type_register ("GstVvasMasterSlotMetaAPI", tags);

    g_once_init_leave (&type, registered);
  }
  return type;
}

static gboolean
gst_vvas_master_slot_meta_init (GstMeta * meta, gpointer params,
    GstBuffer * buffer)
{
  GstVvasMasterSlotMeta *slot_meta = (GstVvasMasterSlotMeta *) meta;

  (void) params;
  (void) buffer;
  slot_meta->slot_id = G_MAXUINT;
  return TRUE;
}

static const GstMetaInfo *
gst_vvas_master_slot_meta_get_info (void)
{
  static const GstMetaInfo *info = NULL;

  if (g_once_init_enter ((GstMetaInfo **) & info)) {
    const GstMetaInfo *registered =
        gst_meta_register (gst_vvas_master_slot_meta_api_get_type (),
        "GstVvasMasterSlotMeta", sizeof (GstVvasMasterSlotMeta),
        gst_vvas_master_slot_meta_init, NULL, NULL);

    g_once_init_leave ((GstMetaInfo **) & info, (GstMetaInfo *) registered);
  }
  return info;
}

static GstVvasMasterSlotMeta *
gst_vvas_master_slot_meta_get (GstBuffer * buffer)
{
  return (GstVvasMasterSlotMeta *) gst_buffer_get_meta (buffer,
      gst_vvas_master_slot_meta_api_get_type ());
}

static gboolean
gst_vvas_tile_composition_storage_identity_equal (const
    GstVvasTileCompositionIdentity * first,
    const GstVvasTileCompositionIdentity * second)
{
  return first->slot_id == second->slot_id &&
      first->epoch == second->epoch &&
      first->layout_version == second->layout_version;
}

static void
gst_vvas_master_pool_promote_quarantined_locked (GstVvasMasterPoolPrivate *
    priv, guint slot_id)
{
  if (priv->quarantined[slot_id] &&
      gst_buffer_is_all_memory_writable (priv->quarantined[slot_id])) {
    priv->available[slot_id] = g_steal_pointer (&priv->quarantined[slot_id]);
  }
}

static gboolean
gst_vvas_master_pool_set_config (GstBufferPool * pool, GstStructure * config)
{
  GstVvasMasterPool *master_pool = GST_VVAS_MASTER_POOL_CAST (pool);
  GstVvasMasterPoolPrivate *priv = master_pool->priv;
  const GstVideoInfo *layout_info;
  GstVideoInfo caps_info;
  GstAllocationParams params;
  GstAllocator *allocator = NULL;
  GstCaps *caps = NULL;
  guint size;
  guint min_buffers;
  guint max_buffers;

  if (!gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers,
          &max_buffers) || !caps)
    return FALSE;
  if (!gst_video_info_from_caps (&caps_info, caps))
    return FALSE;

  layout_info = gst_vvas_tile_composition_layout_get_master_info (priv->layout);
  if (GST_VIDEO_INFO_FORMAT (&caps_info) !=
      GST_VIDEO_INFO_FORMAT (layout_info) ||
      GST_VIDEO_INFO_WIDTH (&caps_info) !=
      GST_VIDEO_INFO_WIDTH (layout_info) ||
      GST_VIDEO_INFO_HEIGHT (&caps_info) !=
      GST_VIDEO_INFO_HEIGHT (layout_info)) {
    GST_ERROR_OBJECT (pool,
        "caps do not match immutable tile composition layout");
    return FALSE;
  }

  gst_allocation_params_init (&params);
  gst_buffer_pool_config_get_allocator (config, &allocator, &params);
  if (!allocator || !GST_IS_VVAS_ALLOCATOR (allocator)) {
    GST_ERROR_OBJECT (pool, "master pool requires a VVAS/XRT allocator");
    return FALSE;
  }
  gst_object_replace ((GstObject **) & priv->allocator,
      (GstObject *) allocator);
  priv->params = params;

  priv->add_videometa = gst_buffer_pool_config_has_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_META);
  if (!priv->add_videometa) {
    GST_ERROR_OBJECT (pool, "master pool requires GstVideoMeta");
    return FALSE;
  }

  priv->master_info = *layout_info;
  gst_buffer_pool_config_set_params (config, caps,
      gst_vvas_tile_composition_layout_get_allocation_size (priv->layout),
      priv->max_slots, priv->max_slots);
  GST_INFO_OBJECT (pool, "configured %u immutable master slots size=%zu "
      "stride=%d format=%s", priv->max_slots,
      gst_vvas_tile_composition_layout_get_allocation_size (priv->layout),
      GST_VIDEO_INFO_PLANE_STRIDE (layout_info, 0),
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (layout_info)));

  return GST_BUFFER_POOL_CLASS (parent_class)->set_config (pool, config);
}

static GstFlowReturn
gst_vvas_master_pool_alloc_buffer (GstBufferPool * pool,
    GstBuffer ** buffer, GstBufferPoolAcquireParams * params)
{
  GstVvasMasterPool *master_pool = GST_VVAS_MASTER_POOL_CAST (pool);
  GstVvasMasterPoolPrivate *priv = master_pool->priv;
  GstVvasMasterSlotMeta *slot_meta;
  guint slot_id;

  (void) params;
  g_mutex_lock (&priv->lock);
  slot_id = priv->next_alloc_slot++;
  g_mutex_unlock (&priv->lock);
  if (slot_id >= priv->max_slots) {
    GST_ERROR_OBJECT (pool, "allocated more than %u physical slots",
        priv->max_slots);
    return GST_FLOW_ERROR;
  }

  *buffer = gst_buffer_new_allocate (priv->allocator,
      gst_vvas_tile_composition_layout_get_allocation_size (priv->layout),
      &priv->params);
  if (!*buffer)
    return GST_FLOW_ERROR;

  if (!gst_buffer_add_video_meta_full (*buffer, GST_VIDEO_FRAME_FLAG_NONE,
          GST_VIDEO_INFO_FORMAT (&priv->master_info),
          GST_VIDEO_INFO_WIDTH (&priv->master_info),
          GST_VIDEO_INFO_HEIGHT (&priv->master_info),
          GST_VIDEO_INFO_N_PLANES (&priv->master_info),
          priv->master_info.offset, priv->master_info.stride))
    goto error;

  slot_meta = (GstVvasMasterSlotMeta *) gst_buffer_add_meta (*buffer,
      gst_vvas_master_slot_meta_get_info (), NULL);
  if (!slot_meta)
    goto error;
  slot_meta->slot_id = slot_id;
  GST_DEBUG_OBJECT (pool, "allocated physical master slot=%u buffer=%p",
      slot_id, *buffer);
  return GST_FLOW_OK;

error:
  gst_buffer_unref (*buffer);
  *buffer = NULL;
  return GST_FLOW_ERROR;
}

static GstFlowReturn
gst_vvas_master_pool_acquire_buffer (GstBufferPool * pool,
    GstBuffer ** buffer, GstBufferPoolAcquireParams * params)
{
  GstVvasMasterPool *master_pool = GST_VVAS_MASTER_POOL_CAST (pool);
  GstVvasMasterPoolPrivate *priv = master_pool->priv;
  gboolean specific_slot = params &&
      (params->flags & GST_VVAS_MASTER_POOL_ACQUIRE_SLOT);
  gboolean dont_wait = params &&
      (params->flags & GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT);
  guint slot_id = G_MAXUINT;
  guint i;

  if (specific_slot) {
    if (params->start < 0 || (guint64) params->start >= priv->max_slots)
      return GST_FLOW_ERROR;
    slot_id = (guint) params->start;
  }

  g_mutex_lock (&priv->lock);
  while (TRUE) {
    if (priv->stopping || GST_BUFFER_POOL_IS_FLUSHING (pool)) {
      g_mutex_unlock (&priv->lock);
      return GST_FLOW_FLUSHING;
    }

    if (specific_slot) {
      gst_vvas_master_pool_promote_quarantined_locked (priv, slot_id);
      if (priv->available[slot_id])
        break;
    } else {
      for (i = 0; i < priv->max_slots; i++) {
        gst_vvas_master_pool_promote_quarantined_locked (priv, i);
        if (priv->available[i]) {
          slot_id = i;
          break;
        }
      }
      if (slot_id != G_MAXUINT)
        break;
    }

    if (dont_wait) {
      g_mutex_unlock (&priv->lock);
      return GST_FLOW_EOS;
    }
    if (!g_cond_wait_until (&priv->condition, &priv->lock,
            g_get_monotonic_time () + G_TIME_SPAN_SECOND)) {
      if (specific_slot)
        GST_WARNING_OBJECT (pool, "coordinator slot=%u is FREE but physical "
            "storage is still retained", slot_id);
      else
        GST_WARNING_OBJECT (pool, "coordinator has a FREE slot but every "
            "physical storage slot is retained");
    }
  }

  *buffer = g_steal_pointer (&priv->available[slot_id]);
  g_mutex_unlock (&priv->lock);
  GST_LOG_OBJECT (pool, "acquired physical master slot=%u buffer=%p",
      slot_id, *buffer);
  return GST_FLOW_OK;
}

static void
gst_vvas_master_pool_release_buffer (GstBufferPool * pool, GstBuffer * buffer)
{
  GstVvasMasterPool *master_pool = GST_VVAS_MASTER_POOL_CAST (pool);
  GstVvasMasterPoolPrivate *priv = master_pool->priv;
  GstVvasMasterSlotMeta *slot_meta = gst_vvas_master_slot_meta_get (buffer);

  if (!slot_meta || slot_meta->slot_id >= priv->max_slots) {
    GST_ERROR_OBJECT (pool, "released master buffer has no physical slot");
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_TAG_MEMORY);
    GST_BUFFER_POOL_CLASS (parent_class)->release_buffer (pool, buffer);
    return;
  }
  if (!gst_buffer_is_all_memory_writable (buffer)) {
    GST_ERROR_OBJECT (pool, "master slot=%u returned with shared memory",
        slot_meta->slot_id);
    g_mutex_lock (&priv->lock);
    if (!priv->quarantined[slot_meta->slot_id])
      priv->quarantined[slot_meta->slot_id] = buffer;
    else
      GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_TAG_MEMORY);
    g_cond_broadcast (&priv->condition);
    g_mutex_unlock (&priv->lock);
    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_TAG_MEMORY))
      GST_BUFFER_POOL_CLASS (parent_class)->release_buffer (pool, buffer);
    return;
  }

  g_mutex_lock (&priv->lock);
  if (priv->available[slot_meta->slot_id]) {
    g_mutex_unlock (&priv->lock);
    GST_ERROR_OBJECT (pool, "duplicate release of physical slot=%u",
        slot_meta->slot_id);
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_TAG_MEMORY);
    GST_BUFFER_POOL_CLASS (parent_class)->release_buffer (pool, buffer);
    return;
  }
  priv->available[slot_meta->slot_id] = buffer;
  g_cond_broadcast (&priv->condition);
  g_mutex_unlock (&priv->lock);
  GST_LOG_OBJECT (pool, "released physical master slot=%u buffer=%p",
      slot_meta->slot_id, buffer);
}

static void
gst_vvas_master_pool_free_buffer (GstBufferPool * pool, GstBuffer * buffer)
{
  GstVvasMasterPool *master_pool = GST_VVAS_MASTER_POOL_CAST (pool);
  GstVvasMasterPoolPrivate *priv = master_pool->priv;
  GstVvasMasterSlotMeta *slot_meta = gst_vvas_master_slot_meta_get (buffer);

  if (slot_meta && slot_meta->slot_id < priv->max_slots) {
    g_mutex_lock (&priv->lock);
    if (priv->available[slot_meta->slot_id] == buffer)
      priv->available[slot_meta->slot_id] = NULL;
    if (priv->quarantined[slot_meta->slot_id] == buffer)
      priv->quarantined[slot_meta->slot_id] = NULL;
    g_mutex_unlock (&priv->lock);
  }
  GST_BUFFER_POOL_CLASS (parent_class)->free_buffer (pool, buffer);
}

static gboolean
gst_vvas_master_pool_start (GstBufferPool * pool)
{
  GstVvasMasterPool *master_pool = GST_VVAS_MASTER_POOL_CAST (pool);
  GstVvasMasterPoolPrivate *priv = master_pool->priv;
  guint i;

  g_mutex_lock (&priv->lock);
  for (i = 0; i < priv->max_slots; i++) {
    if (priv->available[i] || priv->quarantined[i] ||
        priv->reserved_valid[i] || priv->transferred_valid[i]) {
      g_mutex_unlock (&priv->lock);
      GST_ERROR_OBJECT (pool, "refusing start with leftover slot=%u", i);
      return FALSE;
    }
  }
  priv->stopping = FALSE;
  priv->next_alloc_slot = 0;
  g_mutex_unlock (&priv->lock);
  return GST_BUFFER_POOL_CLASS (parent_class)->start (pool);
}

static gboolean
gst_vvas_master_pool_stop (GstBufferPool * pool)
{
  GstVvasMasterPool *master_pool = GST_VVAS_MASTER_POOL_CAST (pool);
  GstVvasMasterPoolPrivate *priv = master_pool->priv;
  GstBuffer **buffers;
  gboolean outstanding_storage = FALSE;
  gboolean result;
  guint i;

  buffers = g_new0 (GstBuffer *, priv->max_slots);
  g_mutex_lock (&priv->lock);
  priv->stopping = TRUE;
  g_cond_broadcast (&priv->condition);
  for (i = 0; i < priv->max_slots; i++) {
    if (priv->reserved_valid[i] || priv->transferred_valid[i]) {
      GST_ERROR_OBJECT (pool, "refusing stop with slot=%u still reserved", i);
      outstanding_storage = TRUE;
    }
  }
  if (outstanding_storage) {
    priv->stopping = FALSE;
    g_mutex_unlock (&priv->lock);
    g_free (buffers);
    return FALSE;
  }
  for (i = 0; i < priv->max_slots; i++) {
    buffers[i] = g_steal_pointer (&priv->available[i]);
    if (!buffers[i])
      buffers[i] = g_steal_pointer (&priv->quarantined[i]);
  }
  g_mutex_unlock (&priv->lock);

  for (i = 0; i < priv->max_slots; i++) {
    if (buffers[i])
      GST_BUFFER_POOL_CLASS (parent_class)->release_buffer (pool, buffers[i]);
  }
  g_free (buffers);
  result = GST_BUFFER_POOL_CLASS (parent_class)->stop (pool);
  return result;
}

static void
gst_vvas_master_pool_flush_start (GstBufferPool * pool)
{
  GstVvasMasterPool *master_pool = GST_VVAS_MASTER_POOL_CAST (pool);

  g_mutex_lock (&master_pool->priv->lock);
  g_cond_broadcast (&master_pool->priv->condition);
  g_mutex_unlock (&master_pool->priv->lock);
}

static void
gst_vvas_master_pool_flush_stop (GstBufferPool * pool)
{
  GstVvasMasterPool *master_pool = GST_VVAS_MASTER_POOL_CAST (pool);

  g_mutex_lock (&master_pool->priv->lock);
  g_cond_broadcast (&master_pool->priv->condition);
  g_mutex_unlock (&master_pool->priv->lock);
}

static void
gst_vvas_master_pool_dispose (GObject * object)
{
  GstVvasMasterPool *pool = GST_VVAS_MASTER_POOL (object);

  if (gst_buffer_pool_is_active (GST_BUFFER_POOL (pool)))
    gst_buffer_pool_set_active (GST_BUFFER_POOL (pool), FALSE);
  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_vvas_master_pool_finalize (GObject * object)
{
  GstVvasMasterPool *pool = GST_VVAS_MASTER_POOL (object);
  GstVvasMasterPoolPrivate *priv = pool->priv;
  guint i;

  for (i = 0; i < priv->max_slots; i++) {
    g_warn_if_fail (priv->reserved[i] == NULL);
    g_warn_if_fail (!priv->reserved_valid[i]);
    g_warn_if_fail (!priv->transferred_valid[i]);
  }

  gst_clear_object (&priv->allocator);
  if (priv->layout)
    gst_vvas_tile_composition_layout_unref (priv->layout);
  g_free (priv->available);
  g_free (priv->quarantined);
  g_free (priv->reserved);
  g_free (priv->reserved_identity);
  g_free (priv->transferred_identity);
  g_free (priv->reserved_valid);
  g_free (priv->transferred_valid);
  g_cond_clear (&priv->condition);
  g_mutex_clear (&priv->lock);
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_vvas_master_pool_class_init (GstVvasMasterPoolClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBufferPoolClass *pool_class = GST_BUFFER_POOL_CLASS (klass);

  gobject_class->dispose = gst_vvas_master_pool_dispose;
  gobject_class->finalize = gst_vvas_master_pool_finalize;
  pool_class->set_config = gst_vvas_master_pool_set_config;
  pool_class->start = gst_vvas_master_pool_start;
  pool_class->stop = gst_vvas_master_pool_stop;
  pool_class->alloc_buffer = gst_vvas_master_pool_alloc_buffer;
  pool_class->acquire_buffer = gst_vvas_master_pool_acquire_buffer;
  pool_class->release_buffer = gst_vvas_master_pool_release_buffer;
  pool_class->free_buffer = gst_vvas_master_pool_free_buffer;
  pool_class->flush_start = gst_vvas_master_pool_flush_start;
  pool_class->flush_stop = gst_vvas_master_pool_flush_stop;
}

static void
gst_vvas_master_pool_init (GstVvasMasterPool * pool)
{
  pool->priv = gst_vvas_master_pool_get_instance_private (pool);
  g_mutex_init (&pool->priv->lock);
  g_cond_init (&pool->priv->condition);
}

GstBufferPool *
gst_vvas_master_pool_new (GstVvasTileCompositionLayout * layout,
    guint max_slots)
{
  GstVvasMasterPool *pool;
  GstVvasMasterPoolPrivate *priv;

  g_return_val_if_fail (layout != NULL, NULL);
  g_return_val_if_fail (max_slots > 0, NULL);

  pool = g_object_new (GST_TYPE_VVAS_MASTER_POOL, NULL);
  gst_object_ref_sink (pool);
  priv = pool->priv;
  priv->layout = gst_vvas_tile_composition_layout_ref (layout);
  priv->max_slots = max_slots;
  priv->available = g_new0 (GstBuffer *, max_slots);
  priv->quarantined = g_new0 (GstBuffer *, max_slots);
  priv->reserved = g_new0 (GstBuffer *, max_slots);
  priv->reserved_identity = g_new0 (GstVvasTileCompositionIdentity, max_slots);
  priv->transferred_identity =
      g_new0 (GstVvasTileCompositionIdentity, max_slots);
  priv->reserved_valid = g_new0 (gboolean, max_slots);
  priv->transferred_valid = g_new0 (gboolean, max_slots);
  return GST_BUFFER_POOL_CAST (pool);
}

gboolean
gst_vvas_master_pool_reserve_slot (GstVvasMasterPool * pool,
    const GstVvasTileCompositionIdentity * identity)
{
  GstVvasMasterPoolPrivate *priv;
  GstBufferPoolAcquireParams params;
  GstBuffer *buffer = NULL;
  GstFlowReturn flow;

  g_return_val_if_fail (GST_IS_VVAS_MASTER_POOL (pool), FALSE);
  g_return_val_if_fail (identity != NULL, FALSE);
  priv = pool->priv;
  if (identity->slot_id >= priv->max_slots)
    return FALSE;

  memset (&params, 0, sizeof (params));
  params.flags = GST_VVAS_MASTER_POOL_ACQUIRE_SLOT;
  params.start = identity->slot_id;
  flow = gst_buffer_pool_acquire_buffer (GST_BUFFER_POOL (pool), &buffer,
      &params);
  if (flow != GST_FLOW_OK)
    return FALSE;

  g_mutex_lock (&priv->lock);
  if (priv->reserved_valid[identity->slot_id] ||
      priv->transferred_valid[identity->slot_id]) {
    g_mutex_unlock (&priv->lock);
    gst_buffer_unref (buffer);
    GST_ERROR_OBJECT (pool, "slot=%u already has storage", identity->slot_id);
    return FALSE;
  }
  priv->reserved[identity->slot_id] = buffer;
  priv->reserved_identity[identity->slot_id] = *identity;
  priv->reserved_valid[identity->slot_id] = TRUE;
  g_mutex_unlock (&priv->lock);
  GST_DEBUG_OBJECT (pool, "reserved slot=%u epoch=%" G_GUINT64_FORMAT,
      identity->slot_id, identity->epoch);
  return TRUE;
}

void
gst_vvas_master_pool_unreserve_slot (GstVvasMasterPool * pool,
    const GstVvasTileCompositionIdentity * identity)
{
  GstVvasMasterPoolPrivate *priv;
  GstBuffer *buffer = NULL;

  g_return_if_fail (GST_IS_VVAS_MASTER_POOL (pool));
  g_return_if_fail (identity != NULL);
  priv = pool->priv;
  if (identity->slot_id >= priv->max_slots)
    return;

  g_mutex_lock (&priv->lock);
  if (priv->reserved_valid[identity->slot_id] &&
      gst_vvas_tile_composition_storage_identity_equal (&priv->reserved_identity
          [identity->slot_id], identity)) {
    buffer = g_steal_pointer (&priv->reserved[identity->slot_id]);
    priv->reserved_valid[identity->slot_id] = FALSE;
  } else if (priv->transferred_valid[identity->slot_id] &&
      gst_vvas_tile_composition_storage_identity_equal
      (&priv->transferred_identity[identity->slot_id], identity)) {
    priv->transferred_valid[identity->slot_id] = FALSE;
  } else {
    GST_WARNING_OBJECT (pool, "stale unreserve slot=%u epoch=%"
        G_GUINT64_FORMAT, identity->slot_id, identity->epoch);
  }
  g_mutex_unlock (&priv->lock);

  if (buffer)
    gst_buffer_unref (buffer);
}

GstBuffer *
gst_vvas_master_pool_ref_slot_buffer (GstVvasMasterPool * pool,
    const GstVvasTileCompositionIdentity * identity)
{
  GstVvasMasterPoolPrivate *priv;
  GstBuffer *buffer = NULL;

  g_return_val_if_fail (GST_IS_VVAS_MASTER_POOL (pool), NULL);
  g_return_val_if_fail (identity != NULL, NULL);
  priv = pool->priv;
  if (identity->slot_id >= priv->max_slots)
    return NULL;

  g_mutex_lock (&priv->lock);
  if (priv->reserved_valid[identity->slot_id] &&
      gst_vvas_tile_composition_storage_identity_equal (&priv->reserved_identity
          [identity->slot_id], identity))
    buffer = gst_buffer_ref (priv->reserved[identity->slot_id]);
  g_mutex_unlock (&priv->lock);
  return buffer;
}

GstBuffer *
gst_vvas_master_pool_take_slot_buffer (GstVvasMasterPool * pool,
    const GstVvasTileCompositionIdentity * identity)
{
  GstVvasMasterPoolPrivate *priv;
  GstBuffer *buffer = NULL;

  g_return_val_if_fail (GST_IS_VVAS_MASTER_POOL (pool), NULL);
  g_return_val_if_fail (identity != NULL, NULL);
  priv = pool->priv;
  if (identity->slot_id >= priv->max_slots)
    return NULL;

  g_mutex_lock (&priv->lock);
  if (priv->reserved_valid[identity->slot_id] &&
      gst_vvas_tile_composition_storage_identity_equal (&priv->reserved_identity
          [identity->slot_id], identity)) {
    buffer = g_steal_pointer (&priv->reserved[identity->slot_id]);
    priv->reserved_valid[identity->slot_id] = FALSE;
    priv->transferred_identity[identity->slot_id] = *identity;
    priv->transferred_valid[identity->slot_id] = TRUE;
  }
  g_mutex_unlock (&priv->lock);
  return buffer;
}

GstVvasTileCompositionLayout *
gst_vvas_master_pool_ref_layout (GstVvasMasterPool * pool)
{
  g_return_val_if_fail (GST_IS_VVAS_MASTER_POOL (pool), NULL);
  return gst_vvas_tile_composition_layout_ref (pool->priv->layout);
}

gboolean
gst_vvas_master_pool_get_slot_snapshot (GstVvasMasterPool * pool,
    guint slot_id, GstVvasMasterSlotSnapshot * snapshot)
{
  g_return_val_if_fail (GST_IS_VVAS_MASTER_POOL (pool), FALSE);
  g_return_val_if_fail (snapshot != NULL, FALSE);
  if (slot_id >= pool->priv->max_slots)
    return FALSE;

  g_mutex_lock (&pool->priv->lock);
  snapshot->available = pool->priv->available[slot_id] != NULL;
  snapshot->quarantined = pool->priv->quarantined[slot_id] != NULL;
  snapshot->reserved = pool->priv->reserved_valid[slot_id];
  snapshot->transferred = pool->priv->transferred_valid[slot_id];
  g_mutex_unlock (&pool->priv->lock);
  return TRUE;
}
