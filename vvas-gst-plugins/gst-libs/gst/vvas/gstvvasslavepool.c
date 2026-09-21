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

#include <gst/allocators/gstdmabuf.h>
#include <gst/vvas/gstvvastilecompositionleasemeta.h>
#include "gstvvasslavepool.h"

GST_DEBUG_CATEGORY_STATIC (gst_vvas_slave_pool_debug);
#define GST_CAT_DEFAULT gst_vvas_slave_pool_debug

struct _GstVvasSlavePoolPrivate
{
  GstVvasTileCompositionCoordinator *coordinator;
  GstVvasMasterPool *master_pool;
  GstVvasTileCompositionLayout *layout;
  GstAllocator *dmabuf_allocator;
  GstVideoInfo tile_info;
  guint tile_id;
  gboolean configured;
};

#define parent_class gst_vvas_slave_pool_parent_class
G_DEFINE_TYPE_WITH_CODE (GstVvasSlavePool, gst_vvas_slave_pool,
    GST_TYPE_VIDEO_BUFFER_POOL, G_ADD_PRIVATE (GstVvasSlavePool);
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "vvasslavepool", 0,
        "VVAS tile composition producer buffer pool"));

static gboolean
gst_vvas_slave_pool_set_config (GstBufferPool * pool, GstStructure * config)
{
  GstVvasSlavePool *producer_pool = GST_VVAS_SLAVE_POOL_CAST (pool);
  GstVvasSlavePoolPrivate *priv = producer_pool->priv;
  GstVideoInfo input_info;
  GstCaps *caps = NULL;
  GError *error = NULL;
  guint size;
  guint min_buffers;
  guint max_buffers;

  if (!gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers,
          &max_buffers) || !caps)
    return FALSE;
  if (!gst_video_info_from_caps (&input_info, caps))
    return FALSE;
  if (!gst_buffer_pool_config_has_option (config,
          GST_BUFFER_POOL_OPTION_VIDEO_META)) {
    GST_ERROR_OBJECT (pool, "producer pool requires GstVideoMeta");
    return FALSE;
  }
  if (!gst_vvas_tile_composition_layout_prepare_tile_info (priv->layout,
          priv->tile_id, &input_info, &priv->tile_info, &error)) {
    GST_ERROR_OBJECT (pool, "invalid sink_%u tile layout: %s", priv->tile_id,
        error ? error->message : "unknown error");
    g_clear_error (&error);
    return FALSE;
  }

  gst_buffer_pool_config_set_params (config, caps,
      gst_vvas_tile_composition_layout_get_allocation_size (priv->layout),
      min_buffers, max_buffers);
  priv->configured = TRUE;
  GST_INFO_OBJECT (pool, "configured sink_%u ephemeral wrappers size=%zu "
      "offset=%zu stride=%d requested=%u/%u", priv->tile_id,
      gst_vvas_tile_composition_layout_get_allocation_size (priv->layout),
      GST_VIDEO_INFO_PLANE_OFFSET (&priv->tile_info, 0),
      GST_VIDEO_INFO_PLANE_STRIDE (&priv->tile_info, 0),
      min_buffers, max_buffers);
  return GST_BUFFER_POOL_CLASS (parent_class)->set_config (pool, config);
}

static gboolean
gst_vvas_slave_pool_start (GstBufferPool * pool)
{
  GstVvasSlavePool *producer_pool = GST_VVAS_SLAVE_POOL_CAST (pool);

  return producer_pool->priv->configured;
}

static gboolean
gst_vvas_slave_pool_stop (GstBufferPool * pool)
{
  (void) pool;
  return TRUE;
}

static GstFlowReturn
gst_vvas_slave_pool_acquire_buffer (GstBufferPool * pool,
    GstBuffer ** buffer, GstBufferPoolAcquireParams * params)
{
  GstVvasSlavePool *producer_pool = GST_VVAS_SLAVE_POOL_CAST (pool);
  GstVvasSlavePoolPrivate *priv = producer_pool->priv;
  GstVvasTileCompositionLeaseMeta *lease_meta;
  GstVvasTileCompositionLease *lease = NULL;
  GstVvasTileCompositionIdentity identity;
  GstVvasTileCompositionResult result;
  GstBuffer *master = NULL;
  GstMemory *master_memory = NULL;
  GstMemory *wrapper_memory = NULL;
  gsize memory_offset;
  gsize memory_maxsize;
  gsize memory_size;
  gboolean dont_wait = params &&
      (params->flags & GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT);
  gint fd;

  *buffer = NULL;
  if (GST_BUFFER_POOL_IS_FLUSHING (pool))
    return GST_FLOW_FLUSHING;
  result =
      gst_vvas_tile_composition_coordinator_acquire_tile (priv->coordinator,
      priv->tile_id, dont_wait, &lease);
  if (result == GST_VVAS_TILE_COMPOSITION_RESULT_FLUSHING)
    return GST_FLOW_FLUSHING;
  if (result == GST_VVAS_TILE_COMPOSITION_RESULT_NO_SLOT)
    return GST_FLOW_EOS;
  if (result != GST_VVAS_TILE_COMPOSITION_RESULT_OK) {
    GST_ERROR_OBJECT (pool, "failed to acquire sink_%u lease: %s",
        priv->tile_id, gst_vvas_tile_composition_result_name (result));
    return GST_FLOW_ERROR;
  }

  gst_vvas_tile_composition_lease_get_identity (lease, &identity);
  master = gst_vvas_master_pool_ref_slot_buffer (priv->master_pool, &identity);
  if (!master) {
    GST_ERROR_OBJECT (pool, "missing master storage slot=%u epoch=%"
        G_GUINT64_FORMAT, identity.slot_id, identity.epoch);
    goto error;
  }

  master_memory = gst_buffer_peek_memory (master, 0);
  if (!master_memory || !gst_is_dmabuf_memory (master_memory)) {
    GST_ERROR_OBJECT (pool, "slot=%u is not DMA-BUF backed", identity.slot_id);
    goto error;
  }
  memory_size = gst_memory_get_sizes (master_memory, &memory_offset,
      &memory_maxsize);
  if (memory_offset > memory_maxsize ||
      memory_size > memory_maxsize - memory_offset ||
      memory_size <
      gst_vvas_tile_composition_layout_get_allocation_size (priv->layout)) {
    GST_ERROR_OBJECT (pool, "invalid master memory geometry slot=%u",
        identity.slot_id);
    goto error;
  }

  fd = gst_dmabuf_memory_get_fd (master_memory);
  if (fd < 0)
    goto error;
  wrapper_memory =
      gst_dmabuf_allocator_alloc_with_flags (priv->dmabuf_allocator, fd,
      gst_vvas_tile_composition_layout_get_allocation_size (priv->layout),
      GST_FD_MEMORY_FLAG_DONT_CLOSE);
  if (!wrapper_memory)
    goto error;

  *buffer = gst_buffer_new ();
  gst_buffer_append_memory (*buffer, wrapper_memory);
  wrapper_memory = NULL;
  if (!gst_buffer_add_video_meta_full (*buffer, GST_VIDEO_FRAME_FLAG_NONE,
          GST_VIDEO_INFO_FORMAT (&priv->tile_info),
          GST_VIDEO_INFO_WIDTH (&priv->tile_info),
          GST_VIDEO_INFO_HEIGHT (&priv->tile_info),
          GST_VIDEO_INFO_N_PLANES (&priv->tile_info),
          priv->tile_info.offset, priv->tile_info.stride))
    goto error;
  lease_meta = gst_buffer_add_vvas_tile_composition_lease_meta (*buffer, lease);
  if (!lease_meta)
    goto error;

  GST_LOG_OBJECT (pool, "issued wrapper=%p slot=%u epoch=%"
      G_GUINT64_FORMAT " tile=%u fd=%d", *buffer, identity.slot_id,
      identity.epoch, priv->tile_id, fd);
  gst_buffer_unref (master);
  gst_vvas_tile_composition_lease_unref (lease);
  return GST_FLOW_OK;

error:
  if (wrapper_memory)
    gst_memory_unref (wrapper_memory);
  if (master)
    gst_buffer_unref (master);
  if (*buffer) {
    gst_buffer_unref (*buffer);
    *buffer = NULL;
  }
  if (lease)
    gst_vvas_tile_composition_lease_unref (lease);
  return GST_FLOW_ERROR;
}

static void
gst_vvas_slave_pool_release_buffer (GstBufferPool * pool, GstBuffer * buffer)
{
  /* gst_buffer_pool_acquire_buffer() owns outstanding accounting and assigns
   * buffer->pool after this class returns GST_FLOW_OK. The public release path
   * clears buffer->pool and decrements outstanding after this vfunc, so the
   * dynamically-created wrapper needs only the default free operation here. */
  GST_LOG_OBJECT (pool, "freeing ephemeral wrapper=%p", buffer);
  gst_buffer_unref (buffer);
}

/* A producer importing DMA-BUFs sets its downstream pool flushing on every
 * unlock(), including the routine unlock performed while delivering EOS with
 * the pipeline still playing. Pool flushing therefore means only "stop issuing
 * tiles for this producer and wake its waiters"; routing it to the pipeline
 * flush path would abort every in-flight epoch on each such unlock, and
 * nothing re-primes them afterwards. Pipeline flushes reach the coordinator
 * from the element's FLUSH_START and FLUSH_STOP handling. */
static void
gst_vvas_slave_pool_flush_start (GstBufferPool * pool)
{
  GstVvasSlavePool *producer_pool = GST_VVAS_SLAVE_POOL_CAST (pool);

  gst_vvas_tile_composition_coordinator_set_pool_flushing (producer_pool->
      priv->coordinator, producer_pool->priv->tile_id, TRUE);
}

static void
gst_vvas_slave_pool_flush_stop (GstBufferPool * pool)
{
  GstVvasSlavePool *producer_pool = GST_VVAS_SLAVE_POOL_CAST (pool);

  gst_vvas_tile_composition_coordinator_set_pool_flushing (producer_pool->
      priv->coordinator, producer_pool->priv->tile_id, FALSE);
}

static void
gst_vvas_slave_pool_finalize (GObject * object)
{
  GstVvasSlavePool *pool = GST_VVAS_SLAVE_POOL (object);
  GstVvasSlavePoolPrivate *priv = pool->priv;

  gst_clear_object (&priv->dmabuf_allocator);
  gst_clear_object (&priv->master_pool);
  if (priv->coordinator)
    gst_vvas_tile_composition_coordinator_unref (priv->coordinator);
  if (priv->layout)
    gst_vvas_tile_composition_layout_unref (priv->layout);
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_vvas_slave_pool_class_init (GstVvasSlavePoolClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBufferPoolClass *pool_class = GST_BUFFER_POOL_CLASS (klass);

  gobject_class->finalize = gst_vvas_slave_pool_finalize;
  pool_class->set_config = gst_vvas_slave_pool_set_config;
  pool_class->start = gst_vvas_slave_pool_start;
  pool_class->stop = gst_vvas_slave_pool_stop;
  pool_class->acquire_buffer = gst_vvas_slave_pool_acquire_buffer;
  pool_class->release_buffer = gst_vvas_slave_pool_release_buffer;
  pool_class->flush_start = gst_vvas_slave_pool_flush_start;
  pool_class->flush_stop = gst_vvas_slave_pool_flush_stop;
}

static void
gst_vvas_slave_pool_init (GstVvasSlavePool * pool)
{
  pool->priv = gst_vvas_slave_pool_get_instance_private (pool);
  pool->priv->dmabuf_allocator = gst_dmabuf_allocator_new ();
}

GstBufferPool *
gst_vvas_slave_pool_new (GstVvasTileCompositionCoordinator * coordinator,
    GstVvasMasterPool * master_pool, GstVvasTileCompositionLayout * layout,
    guint tile_id)
{
  GstVvasSlavePool *pool;

  g_return_val_if_fail (coordinator != NULL, NULL);
  g_return_val_if_fail (GST_IS_VVAS_MASTER_POOL (master_pool), NULL);
  g_return_val_if_fail (layout != NULL, NULL);
  g_return_val_if_fail (tile_id < GST_VVAS_TILE_COMPOSITION_TILE_COUNT, NULL);

  pool = g_object_new (GST_TYPE_VVAS_SLAVE_POOL, NULL);
  gst_object_ref_sink (pool);
  pool->priv->coordinator =
      gst_vvas_tile_composition_coordinator_ref (coordinator);
  pool->priv->master_pool = gst_object_ref (master_pool);
  pool->priv->layout = gst_vvas_tile_composition_layout_ref (layout);
  pool->priv->tile_id = tile_id;
  return GST_BUFFER_POOL_CAST (pool);
}
