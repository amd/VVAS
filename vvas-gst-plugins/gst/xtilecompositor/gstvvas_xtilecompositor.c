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

#include <gst/video/gstvideopool.h>
#include "gstvvas_xtilecompositor.h"
#include "gstvvas_xtilecompositorfronts.h"
#include <gst/vvas/gstvvasallocator.h>
#include <gst/vvas/gstvvasmasterpool.h>
#include <gst/vvas/gstvvastilecompositionleasemeta.h>
#include <gst/vvas/gstvvasoverlaymeta.h>
#include <gst/vvas/gstvvasslavepool.h>

GST_DEBUG_CATEGORY_STATIC (vvas_xtilecompositor_debug);
#define GST_CAT_DEFAULT vvas_xtilecompositor_debug

#define DEFAULT_DEV_INDEX 0
#define DEFAULT_MEM_BANK 0
#define DEFAULT_NEED_DMA TRUE
#define DEFAULT_MAX_POOL_SIZE 16
#define ALIGN_VALUE(value,align) \
  (((value) + (align) - 1) & ~((align) - 1))

enum
{
  PROP_0,
  PROP_STRIDE_ALIGN,
  PROP_ELEVATION_ALIGN,
  PROP_DEV_IDX,
  PROP_XCLBIN_LOCATION,
  PROP_MEM_BANK,
  PROP_NEED_DMA,
  PROP_PROBE_DOWNSTREAM_LAYOUT,
  PROP_MAX_POOL_SIZE,
};

static GstStaticPadTemplate src_templ = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ BGR, RGB }")));

static GstStaticPadTemplate sink_templ = GST_STATIC_PAD_TEMPLATE ("sink_%u",
    GST_PAD_SINK, GST_PAD_REQUEST,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ BGR, RGB }")));

struct _GstVvas_XTileCompositorPrivate
{
  GstVvasMasterPool *mpool;
  GstVvasTileCompositionLayout *layout;
  GstVvasTileCompositionCoordinator *coordinator;
  guint64 next_layout_version;
  GstClockTime last_output_pts;
  guint stride_align;
  guint elevation_align;
  guint dev_idx;
  gchar *xclbin_path;
  guint mem_bank;
  gboolean need_dma;
  gboolean probe_downstream_layout;
  guint max_pool_size;
  GMutex pool_lock;
  GCond pool_cond;
  gboolean pool_initializing;
};

G_DEFINE_TYPE (GstVvas_XTileCompositorPad, gst_vvas_xtilecompositor_pad,
    GST_TYPE_AGGREGATOR_PAD);

#define gst_vvas_xtilecompositor_parent_class parent_class
G_DEFINE_TYPE_WITH_PRIVATE (GstVvas_XTileCompositor, gst_vvas_xtilecompositor,
    GST_TYPE_AGGREGATOR);
#define GST_VVAS_XTILECOMPOSITOR_PRIVATE(self) \
  ((GstVvas_XTileCompositorPrivate *) \
      gst_vvas_xtilecompositor_get_instance_private (self))

static gboolean
gst_vvas_xtilecompositor_reserve_slot (const GstVvasTileCompositionIdentity *
    identity, gpointer user_data)
{
  return gst_vvas_master_pool_reserve_slot (GST_VVAS_MASTER_POOL (user_data),
      identity);
}

static void
gst_vvas_xtilecompositor_unreserve_slot (const GstVvasTileCompositionIdentity *
    identity, gpointer user_data)
{
  gst_vvas_master_pool_unreserve_slot (GST_VVAS_MASTER_POOL (user_data),
      identity);
}

static gboolean
gst_vvas_xtilecompositor_get_pad_index (GstPad * pad, guint * index)
{
  gchar *end = NULL;
  guint64 parsed;

  if (!g_str_has_prefix (GST_PAD_NAME (pad), "sink_"))
    return FALSE;
  parsed = g_ascii_strtoull (GST_PAD_NAME (pad) + 5, &end, 10);
  if (!end || *end || parsed >= GST_VVAS_TILE_COMPOSITION_TILE_COUNT)
    return FALSE;
  *index = (guint) parsed;
  return TRUE;
}

static void
gst_vvas_xtilecompositor_pad_class_init (GstVvas_XTileCompositorPadClass *
    klass)
{
  (void) klass;
}

static void
gst_vvas_xtilecompositor_pad_init (GstVvas_XTileCompositorPad * pad)
{
  pad->spool = NULL;
}

static gboolean
gst_vvas_xtilecompositor_start (GstAggregator * aggregator)
{
  GstVvas_XTileCompositor *self = GST_VVAS_XTILECOMPOSITOR (aggregator);

  if (!self->priv->need_dma) {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("need-dma=false is not supported by vvas_xtilecompositor"),
        ("DMA-BUF producer memory is required"));
    return FALSE;
  }
  self->priv->last_output_pts = GST_CLOCK_TIME_NONE;
  if (GST_AGGREGATOR_CLASS (parent_class)->start)
    return GST_AGGREGATOR_CLASS (parent_class)->start (aggregator);
  return TRUE;
}

static gboolean
gst_vvas_xtilecompositor_stop (GstAggregator * aggregator)
{
  GstVvas_XTileCompositor *self = GST_VVAS_XTILECOMPOSITOR (aggregator);
  GstVvasTileCompositionCoordinator *coordinator = NULL;
  GstVvasMasterPool *mpool = NULL;
  GstVvasTileCompositionLayout *layout = NULL;
  GList *item;
  gboolean parent_result = TRUE;

  g_mutex_lock (&self->priv->pool_lock);
  if (self->priv->coordinator)
    coordinator =
        gst_vvas_tile_composition_coordinator_ref (self->priv->coordinator);
  if (self->priv->mpool)
    mpool = gst_object_ref (self->priv->mpool);
  if (self->priv->layout)
    layout = gst_vvas_tile_composition_layout_ref (self->priv->layout);
  for (item = GST_ELEMENT_CAST (self)->sinkpads; item; item = item->next) {
    GstVvas_XTileCompositorPad *pad = GST_VVAS_XTILECOMPOSITOR_PAD (item->data);

    gst_clear_object (&pad->spool);
  }
  gst_clear_object (&self->priv->mpool);
  if (self->priv->coordinator) {
    gst_vvas_tile_composition_coordinator_unref (self->priv->coordinator);
    self->priv->coordinator = NULL;
  }
  if (self->priv->layout) {
    gst_vvas_tile_composition_layout_unref (self->priv->layout);
    self->priv->layout = NULL;
  }
  g_mutex_unlock (&self->priv->pool_lock);

  if (coordinator)
    gst_vvas_tile_composition_coordinator_set_stopping (coordinator);
  if (mpool && gst_buffer_pool_is_active (GST_BUFFER_POOL (mpool)) &&
      !gst_buffer_pool_set_active (GST_BUFFER_POOL (mpool), FALSE))
    GST_WARNING_OBJECT (self, "master pool still has outstanding buffers");

  if (GST_AGGREGATOR_CLASS (parent_class)->stop)
    parent_result = GST_AGGREGATOR_CLASS (parent_class)->stop (aggregator);
  if (coordinator)
    gst_vvas_tile_composition_coordinator_unref (coordinator);
  if (mpool)
    gst_object_unref (mpool);
  if (layout)
    gst_vvas_tile_composition_layout_unref (layout);
  return parent_result;
}

static gboolean
gst_vvas_xtilecompositor_create_master_pool (GstVvas_XTileCompositor * self)
{
  GstCaps *downstream_caps = NULL;
  GstCaps *template_caps = NULL;
  GstBufferPool *master_pool = NULL;
  GstVvasTileCompositionCoordinator *coordinator = NULL;
  GstVvasTileCompositionLayout *layout = NULL;
  GstVideoInfo master_info;
  GstVideoAlignment align;
  GstAllocationParams params;
  GstAllocator *allocator = NULL;
  GstBufferPool *queried_pool = NULL;
  GstStructure *queried_pool_config = NULL;
  GstStructure *config = NULL;
  GstBuffer *probe = NULL;
  gboolean queried_pool_active = FALSE;
  gboolean result = FALSE;
  GError *error = NULL;
  guint width;
  guint height;

  gst_video_info_init (&master_info);
  gst_video_alignment_reset (&align);
  gst_allocation_params_init (&params);
  template_caps = gst_pad_get_pad_template_caps (self->srcpad);
  downstream_caps =
      gst_pad_peer_query_caps (GST_AGGREGATOR_CAST (self)->srcpad,
      template_caps);
  gst_clear_caps (&template_caps);
  if (!downstream_caps || !gst_caps_is_fixed (downstream_caps) ||
      !gst_video_info_from_caps (&master_info, downstream_caps)) {
    GST_ERROR_OBJECT (self, "downstream must provide fixed BGR/RGB caps");
    goto done;
  }
  if (GST_VIDEO_INFO_FORMAT (&master_info) != GST_VIDEO_FORMAT_BGR &&
      GST_VIDEO_INFO_FORMAT (&master_info) != GST_VIDEO_FORMAT_RGB) {
    GST_ERROR_OBJECT (self, "unsupported master format %s",
        gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&master_info)));
    goto done;
  }

  width = GST_VIDEO_INFO_WIDTH (&master_info);
  height = GST_VIDEO_INFO_HEIGHT (&master_info);
  if (self->priv->probe_downstream_layout) {
    GstQuery *query = gst_query_new_allocation (downstream_caps, TRUE);
    guint queried_size = 0;

    if (!gst_pad_peer_query (GST_AGGREGATOR_CAST (self)->srcpad, query) ||
        gst_query_get_n_allocation_pools (query) == 0) {
      gst_query_unref (query);
      GST_ERROR_OBJECT (self, "downstream layout probe needs a buffer pool");
      goto done;
    }
    gst_query_parse_nth_allocation_pool (query, 0, &queried_pool,
        &queried_size, NULL, NULL);
    gst_query_unref (query);
    if (!queried_pool)
      goto done;

    config = gst_buffer_pool_get_config (queried_pool);
    queried_pool_config = gst_structure_copy (config);
    gst_buffer_pool_config_set_params (config, downstream_caps,
        MAX (queried_size, master_info.size), 1, 0);
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
    gst_buffer_pool_config_add_option (config,
        "GstBufferPoolOptionKMSPrimeExport");
    if (!gst_buffer_pool_set_config (queried_pool, config)) {
      config = NULL;
      goto done;
    }
    config = NULL;
    if (!gst_buffer_pool_set_active (queried_pool, TRUE))
      goto done;
    queried_pool_active = TRUE;
    if (gst_buffer_pool_acquire_buffer (queried_pool, &probe, NULL) !=
        GST_FLOW_OK)
      goto done;
    {
      GstVideoMeta *meta = gst_buffer_get_video_meta (probe);
      gsize row_bytes = (gsize) width * 3;
      gsize probe_size = gst_buffer_get_size (probe);

      if (!meta ||
          meta->format != GST_VIDEO_INFO_FORMAT (&master_info) ||
          meta->width != width || meta->height != height ||
          meta->n_planes != 1 || meta->offset[0] != 0 ||
          meta->stride[0] <= 0 || (gsize) meta->stride[0] < row_bytes ||
          probe_size < (gsize) meta->stride[0] * height) {
        GST_ERROR_OBJECT (self, "invalid downstream layout probe");
        goto done;
      }
      GST_VIDEO_INFO_PLANE_OFFSET (&master_info, 0) = 0;
      GST_VIDEO_INFO_PLANE_STRIDE (&master_info, 0) = meta->stride[0];
      GST_VIDEO_INFO_SIZE (&master_info) = probe_size;
    }
    gst_clear_buffer (&probe);
    if (!gst_buffer_pool_set_active (queried_pool, FALSE))
      goto done;
    queried_pool_active = FALSE;
    config = g_steal_pointer (&queried_pool_config);
    if (!gst_buffer_pool_set_config (queried_pool, config)) {
      config = NULL;
      goto done;
    }
    config = NULL;
  } else {
    align.padding_right = ALIGN_VALUE (width, self->priv->stride_align) - width;
    align.padding_bottom =
        ALIGN_VALUE (height, self->priv->elevation_align) - height;
    if (!gst_video_info_align (&master_info, &align))
      goto done;
  }

  layout = gst_vvas_tile_composition_layout_new (&master_info,
      self->priv->next_layout_version++, &error);
  if (!layout) {
    GST_ERROR_OBJECT (self, "failed to construct tile composition layout: %s",
        error ? error->message : "unknown error");
    g_clear_error (&error);
    goto done;
  }
  if (GST_VIDEO_INFO_PLANE_STRIDE
      (gst_vvas_tile_composition_layout_get_master_info (layout),
          0) != GST_VIDEO_INFO_PLANE_STRIDE (&master_info, 0)
      || gst_vvas_tile_composition_layout_get_allocation_size (layout) !=
      GST_VIDEO_INFO_SIZE (&master_info)) {
    GST_ERROR_OBJECT (self, "constructed layout differs from probed geometry");
    goto done;
  }

  master_pool = gst_vvas_master_pool_new (layout, self->priv->max_pool_size);
  if (!master_pool)
    goto done;
  allocator = gst_vvas_allocator_new (self->priv->dev_idx,
      self->priv->xclbin_path, TRUE, self->priv->mem_bank);
  if (!allocator)
    goto done;

  config = gst_buffer_pool_get_config (master_pool);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  gst_buffer_pool_config_set_params (config, downstream_caps,
      gst_vvas_tile_composition_layout_get_allocation_size (layout),
      self->priv->max_pool_size, self->priv->max_pool_size);
  gst_buffer_pool_config_set_allocator (config, allocator, &params);
  if (!gst_buffer_pool_set_config (master_pool, config)) {
    config = NULL;
    goto done;
  }
  config = NULL;
  if (!gst_buffer_pool_set_active (master_pool, TRUE))
    goto done;

  coordinator = gst_vvas_tile_composition_coordinator_new_full (layout,
      self->priv->max_pool_size,
      (1U << GST_VVAS_TILE_COMPOSITION_TILE_COUNT) - 1,
      gst_vvas_xtilecompositor_reserve_slot,
      gst_vvas_xtilecompositor_unreserve_slot, gst_object_ref (master_pool),
      (GDestroyNotify) gst_object_unref, &error);
  if (!coordinator) {
    GST_ERROR_OBJECT (self,
        "failed to construct tile composition coordinator: %s",
        error ? error->message : "unknown error");
    g_clear_error (&error);
    goto done;
  }

  self->priv->mpool = GST_VVAS_MASTER_POOL (master_pool);
  master_pool = NULL;
  self->priv->layout = layout;
  layout = NULL;
  self->priv->coordinator = coordinator;
  coordinator = NULL;
  result = TRUE;

done:
  if (probe)
    gst_buffer_unref (probe);
  if (queried_pool_active && queried_pool &&
      gst_buffer_pool_set_active (queried_pool, FALSE))
    queried_pool_active = FALSE;
  if (queried_pool_config && queried_pool && !queried_pool_active) {
    GstStructure *restore = g_steal_pointer (&queried_pool_config);

    if (!gst_buffer_pool_set_config (queried_pool, restore))
      GST_WARNING_OBJECT (self, "failed to restore probed downstream pool");
  }
  if (queried_pool_config)
    gst_structure_free (queried_pool_config);
  if (config)
    gst_structure_free (config);
  if (coordinator)
    gst_vvas_tile_composition_coordinator_unref (coordinator);
  if (master_pool) {
    if (gst_buffer_pool_is_active (master_pool))
      gst_buffer_pool_set_active (master_pool, FALSE);
    gst_object_unref (master_pool);
  }
  if (layout)
    gst_vvas_tile_composition_layout_unref (layout);
  gst_clear_object (&allocator);
  gst_clear_object (&queried_pool);
  gst_clear_caps (&downstream_caps);
  gst_clear_caps (&template_caps);
  return result;
}

static gboolean
gst_vvas_xtilecompositor_propose_allocation (GstAggregator * aggregator,
    GstAggregatorPad * aggregator_pad, GstQuery * decide_query,
    GstQuery * query)
{
  GstVvas_XTileCompositor *self = GST_VVAS_XTILECOMPOSITOR (aggregator);
  GstVvas_XTileCompositorPad *pad =
      GST_VVAS_XTILECOMPOSITOR_PAD (aggregator_pad);
  GstCaps *caps = NULL;
  GstStructure *config = NULL;
  guint tile_id;
  guint pool_size;
  gboolean need_pool;
  gboolean created_pool = FALSE;
  gboolean result = FALSE;

  (void) decide_query;
  gst_query_parse_allocation (query, &caps, &need_pool);
  if (!caps
      || !gst_vvas_xtilecompositor_get_pad_index (GST_PAD (pad), &tile_id))
    return FALSE;

  g_mutex_lock (&self->priv->pool_lock);
  while (self->priv->pool_initializing)
    g_cond_wait (&self->priv->pool_cond, &self->priv->pool_lock);
  if (!self->priv->mpool) {
    self->priv->pool_initializing = TRUE;
    g_mutex_unlock (&self->priv->pool_lock);
    result = gst_vvas_xtilecompositor_create_master_pool (self);
    g_mutex_lock (&self->priv->pool_lock);
    self->priv->pool_initializing = FALSE;
    g_cond_broadcast (&self->priv->pool_cond);
    if (!result)
      goto done;
  }

  if (!pad->spool) {
    pad->spool = gst_vvas_slave_pool_new (self->priv->coordinator,
        self->priv->mpool, self->priv->layout, tile_id);
    if (!pad->spool)
      goto done;
    created_pool = TRUE;
    config = gst_buffer_pool_get_config (pad->spool);
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
    gst_buffer_pool_config_set_params (config, caps,
        gst_vvas_tile_composition_layout_get_allocation_size (self->
            priv->layout), 0, self->priv->max_pool_size);
    if (!gst_buffer_pool_set_config (pad->spool, config)) {
      config = NULL;
      goto done;
    }
    config = NULL;
  }

  pool_size =
      gst_vvas_tile_composition_layout_get_allocation_size (self->priv->layout);
  gst_query_add_allocation_pool (query, pad->spool, pool_size, 0,
      self->priv->max_pool_size);
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
  GST_INFO_OBJECT (pad, "proposed coordinator pool tile=%u need-pool=%d",
      tile_id, need_pool);
  result = TRUE;

done:
  if (!result && created_pool)
    gst_clear_object (&pad->spool);
  if (config)
    gst_structure_free (config);
  g_mutex_unlock (&self->priv->pool_lock);
  return result;
}

static gboolean
gst_vvas_xtilecompositor_sink_query (GstAggregator * aggregator,
    GstAggregatorPad * pad, GstQuery * query)
{
  GstCaps *caps = NULL;

  if (GST_QUERY_TYPE (query) == GST_QUERY_ALLOCATION) {
    gst_query_parse_allocation (query, &caps, NULL);
    if (caps)
      return gst_vvas_xtilecompositor_propose_allocation (aggregator, pad, NULL,
          query);
  }
  return GST_AGGREGATOR_CLASS (parent_class)->sink_query (aggregator, pad,
      query);
}

static gboolean
gst_vvas_xtilecompositor_sink_event (GstAggregator * aggregator,
    GstAggregatorPad * pad, GstEvent * event)
{
  GstVvas_XTileCompositor *self = GST_VVAS_XTILECOMPOSITOR (aggregator);
  GstVvasTileCompositionCoordinator *coordinator = NULL;
  guint tile_id;
  gboolean have_tile_id;

  g_mutex_lock (&self->priv->pool_lock);
  if (self->priv->coordinator)
    coordinator =
        gst_vvas_tile_composition_coordinator_ref (self->priv->coordinator);
  have_tile_id =
      gst_vvas_xtilecompositor_get_pad_index (GST_PAD (pad), &tile_id);
  g_mutex_unlock (&self->priv->pool_lock);
  if (coordinator && have_tile_id) {
    if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_START)
      gst_vvas_tile_composition_coordinator_set_tile_flushing (coordinator,
          tile_id, TRUE);
    else if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_STOP)
      gst_vvas_tile_composition_coordinator_set_tile_flushing (coordinator,
          tile_id, FALSE);
    gst_vvas_tile_composition_coordinator_unref (coordinator);
  } else if (coordinator) {
    gst_vvas_tile_composition_coordinator_unref (coordinator);
  }
  return GST_AGGREGATOR_CLASS (parent_class)->sink_event (aggregator, pad,
      event);
}

typedef struct
{
  GstVvas_XTileCompositor *self;
  GstAggregatorPad *pads[GST_VVAS_TILE_COMPOSITION_TILE_COUNT];
  GstFlowReturn flow;
  gboolean saw_eos_without_buffer;
} GstVvasXTileCompositorFrontContext;

static gboolean
gst_vvas_xtilecompositor_peek_front (gpointer user_data, guint tile_id,
    GstVvasTileCompositionIdentity * identity)
{
  GstVvasXTileCompositorFrontContext *context = user_data;
  GstAggregatorPad *pad = context->pads[tile_id];
  GstVvasTileCompositionLeaseMeta *meta;
  GstBuffer *buffer;

  if (!gst_aggregator_pad_has_buffer (pad)) {
    if (gst_aggregator_pad_is_eos (pad))
      context->saw_eos_without_buffer = TRUE;
    return FALSE;
  }

  buffer = gst_aggregator_pad_peek_buffer (pad);
  if (!buffer) {
    if (GST_PAD_IS_FLUSHING (GST_PAD (pad)))
      context->flow = GST_FLOW_FLUSHING;
    return FALSE;
  }
  if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_GAP)) {
    GstBuffer *popped;

    gst_buffer_unref (buffer);
    popped = gst_aggregator_pad_pop_buffer (pad);
    gst_clear_buffer (&popped);
    return FALSE;
  }

  meta = gst_buffer_get_vvas_tile_composition_lease_meta (buffer);
  if (!meta ||
      gst_vvas_tile_composition_lease_get_role (meta->lease) !=
      GST_VVAS_TILE_COMPOSITION_LEASE_TILE ||
      gst_vvas_tile_composition_lease_get_tile_id (meta->lease) != tile_id) {
    GST_ERROR_OBJECT (pad, "missing or invalid tile lease meta");
    context->flow = GST_FLOW_ERROR;
    gst_buffer_unref (buffer);
    return FALSE;
  }
  gst_vvas_tile_composition_lease_get_identity (meta->lease, identity);
  gst_buffer_unref (buffer);
  return TRUE;
}

static gboolean
gst_vvas_xtilecompositor_pop_front (gpointer user_data, guint tile_id)
{
  GstVvasXTileCompositorFrontContext *context = user_data;
  GstBuffer *buffer;

  buffer = gst_aggregator_pad_pop_buffer (context->pads[tile_id]);
  if (!buffer) {
    if (GST_PAD_IS_FLUSHING (GST_PAD (context->pads[tile_id])))
      context->flow = GST_FLOW_FLUSHING;
    return FALSE;
  }
  gst_buffer_unref (buffer);
  return TRUE;
}

static GstVvasTileCompositionResult
gst_vvas_xtilecompositor_abort_front (gpointer user_data,
    const GstVvasTileCompositionIdentity * identity)
{
  GstVvasXTileCompositorFrontContext *context = user_data;
  GstVvasTileCompositionResult result;

  if (identity->slot_id >= context->self->priv->max_pool_size) {
    GST_ERROR_OBJECT (context->self,
        "invalid front slot=%u epoch=%" G_GUINT64_FORMAT " max-slots=%u",
        identity->slot_id, identity->epoch, context->self->priv->max_pool_size);
    context->flow = GST_FLOW_ERROR;
    return GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;
  }
  result =
      gst_vvas_tile_composition_coordinator_abort_epoch (context->self->
      priv->coordinator, identity, GST_VVAS_TILE_COMPOSITION_ABORT_INVALID_SET);
  if (result == GST_VVAS_TILE_COMPOSITION_RESULT_INVALID) {
    GST_WARNING_OBJECT (context->self,
        "dropping queued front for retired slot=%u epoch=%" G_GUINT64_FORMAT,
        identity->slot_id, identity->epoch);
    return GST_VVAS_TILE_COMPOSITION_RESULT_STALE;
  }
  if (result != GST_VVAS_TILE_COMPOSITION_RESULT_OK &&
      result != GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE &&
      result != GST_VVAS_TILE_COMPOSITION_RESULT_STALE)
    context->flow = GST_FLOW_ERROR;
  return result;
}

static void
gst_vvas_xtilecompositor_report_mismatch (gpointer user_data,
    const GstVvasTileCompositionIdentity
    identities[GST_VVAS_TILE_COMPOSITION_TILE_COUNT])
{
  GstVvasXTileCompositorFrontContext *context = user_data;

  GST_WARNING_OBJECT (context->self,
      "front identities tile0=%u/%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT " "
      "tile1=%u/%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT " "
      "tile2=%u/%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT " "
      "tile3=%u/%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT,
      identities[0].slot_id, identities[0].epoch,
      identities[0].layout_version,
      identities[1].slot_id, identities[1].epoch,
      identities[1].layout_version,
      identities[2].slot_id, identities[2].epoch,
      identities[2].layout_version,
      identities[3].slot_id, identities[3].epoch, identities[3].layout_version);
}

static void
gst_vvas_xtilecompositor_drop_fronts (GstVvas_XTileCompositor * self,
    GstAggregatorPad ** pads, GstVvasTileCompositionIdentity * identities,
    guint count, GstVvasTileCompositionAbortReason reason)
{
  guint64 oldest_epoch = G_MAXUINT64;
  guint i;
  guint j;

  for (i = 0; i < count; i++)
    oldest_epoch = MIN (oldest_epoch, identities[i].epoch);

  for (i = 0; i < count; i++) {
    gboolean duplicate = FALSE;

    if (identities[i].epoch != oldest_epoch)
      continue;
    for (j = 0; j < i; j++) {
      if (identities[i].slot_id == identities[j].slot_id &&
          identities[i].epoch == identities[j].epoch &&
          identities[i].layout_version == identities[j].layout_version) {
        duplicate = TRUE;
        break;
      }
    }
    if (!duplicate)
      gst_vvas_tile_composition_coordinator_abort_epoch (self->
          priv->coordinator, &identities[i], reason);
  }
  for (i = 0; i < count; i++) {
    GstBuffer *buffer;

    if (identities[i].epoch != oldest_epoch)
      continue;
    buffer = gst_aggregator_pad_pop_buffer (pads[i]);
    if (buffer)
      gst_buffer_unref (buffer);
  }
}

static GstFlowReturn
gst_vvas_xtilecompositor_drain_eos_fronts (GstVvas_XTileCompositor * self,
    GstAggregatorPad ** pads)
{
  gboolean all_eos = TRUE;
  guint tile;

  for (tile = 0; tile < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; tile++) {
    GstBuffer *buffer;

    while ((buffer = gst_aggregator_pad_pop_buffer (pads[tile]))) {
      GstVvasTileCompositionLeaseMeta *meta =
          gst_buffer_get_vvas_tile_composition_lease_meta (buffer);

      if (meta &&
          gst_vvas_tile_composition_lease_get_role (meta->lease) ==
          GST_VVAS_TILE_COMPOSITION_LEASE_TILE) {
        GstVvasTileCompositionIdentity identity;
        GstVvasTileCompositionResult result;

        gst_vvas_tile_composition_lease_get_identity (meta->lease, &identity);
        result =
            gst_vvas_tile_composition_coordinator_abort_epoch (self->
            priv->coordinator, &identity,
            GST_VVAS_TILE_COMPOSITION_ABORT_EXPLICIT);
        if (result != GST_VVAS_TILE_COMPOSITION_RESULT_OK
            && result != GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE
            && result != GST_VVAS_TILE_COMPOSITION_RESULT_STALE
            && result != GST_VVAS_TILE_COMPOSITION_RESULT_INVALID) {
          gst_buffer_unref (buffer);
          return GST_FLOW_ERROR;
        }
      }
      gst_buffer_unref (buffer);
    }
    if (!gst_aggregator_pad_is_eos (pads[tile]))
      all_eos = FALSE;
  }

  return all_eos ? GST_FLOW_EOS : GST_FLOW_OK;
}

static GstFlowReturn
gst_vvas_xtilecompositor_aggregate (GstAggregator * aggregator,
    gboolean timeout)
{
  GstVvas_XTileCompositor *self = GST_VVAS_XTILECOMPOSITOR (aggregator);
  GstAggregatorPad *pads[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] = { NULL };
  GstBuffer *peeked[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] = { NULL };
  GstBuffer *popped[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] = { NULL };
  GstVvasTileCompositionLease *leases[GST_VVAS_TILE_COMPOSITION_TILE_COUNT] =
      { NULL };
  GstVvasTileCompositionIdentity
      identities[GST_VVAS_TILE_COMPOSITION_TILE_COUNT];
  GstVvasTileCompositionLease *output_lease = NULL;
  GstVvasTileCompositionIdentity output_identity;
  GstBuffer *master = NULL;
  GstBuffer *metadata_buffer = NULL;
  GstVvasOverlayMeta *merged_meta = NULL;
  GstVvasTileCompositionResult composition_result;
  GstClockTime output_pts = GST_CLOCK_TIME_NONE;
  GstClockTime output_duration = GST_CLOCK_TIME_NONE;
  GList *item;
  guint seen_mask = 0;
  guint count = 0;
  guint i;
  GstFlowReturn flow = GST_FLOW_OK;
  GstVvasXTileCompositorFrontContext front_context = { 0 };
  GstVvasXTileCompositorFrontResult front_result;

  (void) timeout;
  if (!self->priv->coordinator || !self->priv->layout || !self->priv->mpool)
    return GST_FLOW_NOT_NEGOTIATED;
  if (g_list_length (GST_ELEMENT_CAST (self)->sinkpads) !=
      GST_VVAS_TILE_COMPOSITION_TILE_COUNT)
    return GST_FLOW_NOT_NEGOTIATED;

  for (item = GST_ELEMENT_CAST (self)->sinkpads; item; item = item->next) {
    GstVvas_XTileCompositorPad *pad = GST_VVAS_XTILECOMPOSITOR_PAD (item->data);
    guint tile_id;

    if (!gst_vvas_xtilecompositor_get_pad_index (GST_PAD (pad), &tile_id) ||
        (seen_mask & (1U << tile_id))) {
      flow = GST_FLOW_ERROR;
      goto done;
    }
    pads[tile_id] = GST_AGGREGATOR_PAD (pad);
    seen_mask |= 1U << tile_id;
    count++;
  }

  if (seen_mask != ((1U << GST_VVAS_TILE_COMPOSITION_TILE_COUNT) - 1))
    goto done;

retry_fronts:
  front_context.self = self;
  front_context.flow = GST_FLOW_OK;
  front_context.saw_eos_without_buffer = FALSE;
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++)
    front_context.pads[i] = pads[i];
  front_result =
      gst_vvas_xtilecompositor_align_fronts
      (gst_vvas_xtilecompositor_peek_front, gst_vvas_xtilecompositor_pop_front,
      gst_vvas_xtilecompositor_abort_front,
      gst_vvas_xtilecompositor_report_mismatch, &front_context, identities);
  if (front_result == GST_VVAS_XTILECOMPOSITOR_FRONTS_ERROR) {
    flow = front_context.flow == GST_FLOW_OK ?
        GST_FLOW_ERROR : front_context.flow;
    goto done;
  }
  if (front_result == GST_VVAS_XTILECOMPOSITOR_FRONTS_NEED_DATA) {
    if (front_context.saw_eos_without_buffer &&
        front_context.flow == GST_FLOW_OK)
      flow = gst_vvas_xtilecompositor_drain_eos_fronts (self, pads);
    else
      flow = front_context.flow;
    goto done;
  }

  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    GstVvasTileCompositionLeaseMeta *lease_meta;

    peeked[i] = gst_aggregator_pad_peek_buffer (pads[i]);
    if (!peeked[i]) {
      flow = GST_PAD_IS_FLUSHING (GST_PAD (pads[i])) ?
          GST_FLOW_FLUSHING : GST_FLOW_ERROR;
      goto done;
    }
    lease_meta = gst_buffer_get_vvas_tile_composition_lease_meta (peeked[i]);
    if (!lease_meta) {
      flow = GST_FLOW_ERROR;
      goto done;
    }
    leases[i] = lease_meta->lease;
    gst_vvas_tile_composition_lease_get_identity (leases[i], &identities[i]);
  }

  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    composition_result =
        gst_vvas_tile_composition_coordinator_mark_tile_ready (leases[i],
        GST_BUFFER_PTS (peeked[i]), GST_BUFFER_DURATION (peeked[i]));
    if (composition_result != GST_VVAS_TILE_COMPOSITION_RESULT_OK
        && composition_result != GST_VVAS_TILE_COMPOSITION_RESULT_DUPLICATE) {
      if (composition_result == GST_VVAS_TILE_COMPOSITION_RESULT_FLUSHING) {
        flow = GST_FLOW_FLUSHING;
        goto done;
      }
      gst_vvas_xtilecompositor_drop_fronts (self, pads, identities, count,
          GST_VVAS_TILE_COMPOSITION_ABORT_INVALID_SET);
      for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++)
        gst_clear_buffer (&peeked[i]);
      goto retry_fronts;
    }
  }

retry_begin_processing:
  composition_result =
      gst_vvas_tile_composition_coordinator_begin_processing (self->
      priv->coordinator, leases, GST_VVAS_TILE_COMPOSITION_TILE_COUNT,
      &output_lease);
  if (composition_result == GST_VVAS_TILE_COMPOSITION_RESULT_WAITING) {
    composition_result =
        gst_vvas_tile_composition_coordinator_wait_until_publishable (leases[0],
        50 * GST_MSECOND);
    if (composition_result == GST_VVAS_TILE_COMPOSITION_RESULT_OK)
      goto retry_begin_processing;
    if (composition_result == GST_VVAS_TILE_COMPOSITION_RESULT_WAITING)
      goto done;
    if (composition_result == GST_VVAS_TILE_COMPOSITION_RESULT_FLUSHING) {
      flow = GST_FLOW_FLUSHING;
      goto done;
    }
  }
  if (composition_result != GST_VVAS_TILE_COMPOSITION_RESULT_OK) {
    gst_vvas_xtilecompositor_drop_fronts (self, pads, identities, count,
        GST_VVAS_TILE_COMPOSITION_ABORT_INVALID_SET);
    goto done;
  }
  gst_vvas_tile_composition_lease_get_identity (output_lease, &output_identity);

  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    popped[i] = gst_aggregator_pad_pop_buffer (pads[i]);
    if (!popped[i]) {
      flow = GST_PAD_IS_FLUSHING (GST_PAD (pads[i])) ?
          GST_FLOW_FLUSHING : GST_FLOW_ERROR;
      goto done;
    }
  }
  master = gst_vvas_master_pool_take_slot_buffer (self->priv->mpool,
      &output_identity);
  if (!master) {
    GST_ERROR_OBJECT (self, "failed to take master slot=%u epoch=%"
        G_GUINT64_FORMAT, output_identity.slot_id, output_identity.epoch);
    flow = GST_FLOW_ERROR;
    goto done;
  }

  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    GstVvasOverlayMeta *source_meta =
        gst_buffer_get_vvas_overlay_meta (popped[i]);
    const GstVvasTileCompositionTile *tile =
        gst_vvas_tile_composition_layout_get_tile (self->priv->layout, i);
    const GstVideoInfo *master_info =
        gst_vvas_tile_composition_layout_get_master_info (self->priv->layout);

    if (GST_CLOCK_TIME_IS_VALID (GST_BUFFER_PTS (popped[i])) &&
        (!GST_CLOCK_TIME_IS_VALID (output_pts) ||
            GST_BUFFER_PTS (popped[i]) > output_pts))
      output_pts = GST_BUFFER_PTS (popped[i]);
    if (GST_CLOCK_TIME_IS_VALID (GST_BUFFER_DURATION (popped[i])) &&
        (!GST_CLOCK_TIME_IS_VALID (output_duration) ||
            GST_BUFFER_DURATION (popped[i]) < output_duration))
      output_duration = GST_BUFFER_DURATION (popped[i]);

    if (source_meta) {
      if (!metadata_buffer)
        metadata_buffer = gst_buffer_new ();
      if (!merged_meta)
        merged_meta = gst_buffer_add_vvas_overlay_meta (metadata_buffer);
      if (!merged_meta || !tile ||
          !gst_vvas_overlay_meta_append_translated (merged_meta, source_meta,
              tile->x, tile->y, tile->width, tile->height,
              GST_VIDEO_INFO_WIDTH (master_info),
              GST_VIDEO_INFO_HEIGHT (master_info))) {
        GST_ERROR_OBJECT (self, "failed to merge overlay meta for tile=%u", i);
        flow = GST_FLOW_ERROR;
        goto done;
      }
    }
  }

  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    gst_clear_buffer (&peeked[i]);
    gst_clear_buffer (&popped[i]);
  }
  /* Coordinator leases prevent slot reuse until all tile wrappers retire. */
  if (metadata_buffer &&
      !gst_buffer_copy_into (master, metadata_buffer, GST_BUFFER_COPY_META,
          0, -1)) {
    GST_ERROR_OBJECT (self, "failed to attach merged overlay metadata");
    flow = GST_FLOW_ERROR;
    goto done;
  }

  if (GST_CLOCK_TIME_IS_VALID (self->priv->last_output_pts) &&
      (!GST_CLOCK_TIME_IS_VALID (output_pts) ||
          output_pts <= self->priv->last_output_pts)) {
    output_pts = self->priv->last_output_pts == G_MAXUINT64 ?
        self->priv->last_output_pts : self->priv->last_output_pts + 1;
  }
  if (GST_CLOCK_TIME_IS_VALID (output_pts))
    self->priv->last_output_pts = output_pts;
  GST_BUFFER_PTS (master) = output_pts;
  GST_BUFFER_DTS (master) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION (master) = output_duration;

  if (!gst_buffer_add_vvas_tile_composition_lease_meta (master, output_lease)) {
    GST_ERROR_OBJECT (self, "failed to attach output lease meta");
    flow = GST_FLOW_ERROR;
    goto done;
  }
  gst_vvas_tile_composition_lease_unref (output_lease);
  output_lease = NULL;
  flow = gst_aggregator_finish_buffer (aggregator, master);
  master = NULL;

done:
  for (i = 0; i < GST_VVAS_TILE_COMPOSITION_TILE_COUNT; i++) {
    gst_clear_buffer (&peeked[i]);
    gst_clear_buffer (&popped[i]);
  }
  if (metadata_buffer)
    gst_buffer_unref (metadata_buffer);
  if (output_lease)
    gst_vvas_tile_composition_lease_unref (output_lease);
  if (master)
    gst_buffer_unref (master);
  return flow;
}

static void
gst_vvas_xtilecompositor_release_pad (GstElement * element, GstPad * pad)
{
  GstVvas_XTileCompositorPad *compositor_pad =
      GST_VVAS_XTILECOMPOSITOR_PAD (pad);

  gst_clear_object (&compositor_pad->spool);
  gst_pad_set_active (pad, FALSE);
  gst_element_remove_pad (element, pad);
}

static void
gst_vvas_xtilecompositor_finalize (GObject * object)
{
  GstVvas_XTileCompositor *self = GST_VVAS_XTILECOMPOSITOR (object);

  gst_clear_object (&self->priv->mpool);
  if (self->priv->coordinator)
    gst_vvas_tile_composition_coordinator_unref (self->priv->coordinator);
  if (self->priv->layout)
    gst_vvas_tile_composition_layout_unref (self->priv->layout);
  g_free (self->priv->xclbin_path);
  g_cond_clear (&self->priv->pool_cond);
  g_mutex_clear (&self->priv->pool_lock);
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_vvas_xtilecompositor_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVvas_XTileCompositor *self = GST_VVAS_XTILECOMPOSITOR (object);

  switch (property_id) {
    case PROP_STRIDE_ALIGN:
      self->priv->stride_align = g_value_get_uint (value);
      break;
    case PROP_ELEVATION_ALIGN:
      self->priv->elevation_align = g_value_get_uint (value);
      break;
    case PROP_DEV_IDX:
      self->priv->dev_idx = g_value_get_uint (value);
      break;
    case PROP_XCLBIN_LOCATION:
      g_free (self->priv->xclbin_path);
      self->priv->xclbin_path = g_value_dup_string (value);
      break;
    case PROP_MEM_BANK:
      self->priv->mem_bank = g_value_get_uint (value);
      break;
    case PROP_NEED_DMA:
      self->priv->need_dma = g_value_get_boolean (value);
      break;
    case PROP_PROBE_DOWNSTREAM_LAYOUT:
      self->priv->probe_downstream_layout = g_value_get_boolean (value);
      break;
    case PROP_MAX_POOL_SIZE:
      self->priv->max_pool_size = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_vvas_xtilecompositor_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstVvas_XTileCompositor *self = GST_VVAS_XTILECOMPOSITOR (object);

  switch (property_id) {
    case PROP_STRIDE_ALIGN:
      g_value_set_uint (value, self->priv->stride_align);
      break;
    case PROP_ELEVATION_ALIGN:
      g_value_set_uint (value, self->priv->elevation_align);
      break;
    case PROP_DEV_IDX:
      g_value_set_uint (value, self->priv->dev_idx);
      break;
    case PROP_XCLBIN_LOCATION:
      g_value_set_string (value, self->priv->xclbin_path);
      break;
    case PROP_MEM_BANK:
      g_value_set_uint (value, self->priv->mem_bank);
      break;
    case PROP_NEED_DMA:
      g_value_set_boolean (value, self->priv->need_dma);
      break;
    case PROP_PROBE_DOWNSTREAM_LAYOUT:
      g_value_set_boolean (value, self->priv->probe_downstream_layout);
      break;
    case PROP_MAX_POOL_SIZE:
      g_value_set_uint (value, self->priv->max_pool_size);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_vvas_xtilecompositor_class_init (GstVvas_XTileCompositorClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstAggregatorClass *aggregator_class = GST_AGGREGATOR_CLASS (klass);

  gobject_class->set_property = gst_vvas_xtilecompositor_set_property;
  gobject_class->get_property = gst_vvas_xtilecompositor_get_property;
  gobject_class->finalize = gst_vvas_xtilecompositor_finalize;
  element_class->release_pad =
      GST_DEBUG_FUNCPTR (gst_vvas_xtilecompositor_release_pad);
  aggregator_class->start = GST_DEBUG_FUNCPTR (gst_vvas_xtilecompositor_start);
  aggregator_class->stop = GST_DEBUG_FUNCPTR (gst_vvas_xtilecompositor_stop);
  aggregator_class->aggregate =
      GST_DEBUG_FUNCPTR (gst_vvas_xtilecompositor_aggregate);
  aggregator_class->sink_query =
      GST_DEBUG_FUNCPTR (gst_vvas_xtilecompositor_sink_query);
  aggregator_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_vvas_xtilecompositor_sink_event);
  aggregator_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_vvas_xtilecompositor_propose_allocation);

  gst_element_class_add_static_pad_template_with_gtype (element_class,
      &sink_templ, GST_TYPE_VVAS_XTILECOMPOSITOR_PAD);
  gst_element_class_add_static_pad_template_with_gtype (element_class,
      &src_templ, GST_TYPE_AGGREGATOR_PAD);

  g_object_class_install_property (gobject_class, PROP_STRIDE_ALIGN,
      g_param_spec_uint ("stride-align", "Stride alignment",
          "Master stride alignment when downstream probing is disabled",
          1, G_MAXUINT, 1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_ELEVATION_ALIGN,
      g_param_spec_uint ("elevation-align", "Elevation alignment",
          "Master elevation alignment when downstream probing is disabled",
          1, G_MAXUINT, 1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_DEV_IDX,
      g_param_spec_uint ("dev-idx", "Device index",
          "FPGA device index for XRT master buffers", 0, 31,
          DEFAULT_DEV_INDEX, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_XCLBIN_LOCATION,
      g_param_spec_string ("xclbin-location", "xclbin location",
          "Optional xclbin used to open the VVAS allocator context", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_MEM_BANK,
      g_param_spec_uint ("mem-bank", "Memory bank",
          "Memory bank for XRT master buffers", 0, G_MAXUINT,
          DEFAULT_MEM_BANK, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_NEED_DMA,
      g_param_spec_boolean ("need-dma", "Need DMA-BUF",
          "Expose DMA-BUF-backed tile views to producer pools",
          DEFAULT_NEED_DMA, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
      PROP_PROBE_DOWNSTREAM_LAYOUT,
      g_param_spec_boolean ("probe-downstream-layout",
          "Probe downstream layout",
          "Sample downstream VideoMeta stride and allocation size",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_MAX_POOL_SIZE,
      g_param_spec_uint ("max-pool-size", "Maximum tile composition slots",
          "Number of fixed XRT master slots", 1, G_MAXUINT,
          DEFAULT_MAX_POOL_SIZE,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
          G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (element_class,
      "VVAS Tile Compositor", "Video/Muxer",
      "Coordinates DMA-BUF tiles by slot and epoch",
      "Advanced Micro Devices, Inc");
  GST_DEBUG_CATEGORY_INIT (vvas_xtilecompositor_debug, "vvas_xtilecompositor",
      0, "VVAS Tile Compositor");
}

static void
gst_vvas_xtilecompositor_init (GstVvas_XTileCompositor * self)
{
  self->priv = GST_VVAS_XTILECOMPOSITOR_PRIVATE (self);
  self->srcpad = GST_AGGREGATOR_CAST (self)->srcpad;
  self->priv->stride_align = 1;
  self->priv->elevation_align = 1;
  self->priv->dev_idx = DEFAULT_DEV_INDEX;
  self->priv->mem_bank = DEFAULT_MEM_BANK;
  self->priv->need_dma = DEFAULT_NEED_DMA;
  self->priv->probe_downstream_layout = FALSE;
  self->priv->max_pool_size = DEFAULT_MAX_POOL_SIZE;
  self->priv->next_layout_version = 1;
  self->priv->last_output_pts = GST_CLOCK_TIME_NONE;
  g_mutex_init (&self->priv->pool_lock);
  g_cond_init (&self->priv->pool_cond);
}

#ifndef PACKAGE
#define PACKAGE "vvas_xtilecompositor"
#endif

static gboolean
vvas_xtilecompositor_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "vvas_xtilecompositor", GST_RANK_NONE,
      GST_TYPE_VVAS_XTILECOMPOSITOR);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    vvas_xtilecompositor,
    "VVAS Tile Compositor plugin",
    vvas_xtilecompositor_init,
    VVAS_API_VERSION, "MIT/X11", "AMD VVAS SDK", "https://www.amd.com/")
