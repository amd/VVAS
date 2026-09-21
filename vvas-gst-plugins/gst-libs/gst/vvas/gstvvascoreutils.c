/*
 * Copyright (C) 2020 - 2022 Xilinx, Inc.
 * Copyright (C) 2022 - 2026 Advanced Micro Devices, Inc.
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

#include <gst/vvas/gstvvascoreutils.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>


typedef struct
{
  GstBuffer *buf;
  GstMapInfo map_info;
  GstVideoFrame *vframe;
} VvasGstUserData;

gboolean
gst_vvas_buffer_make_dmabuf_memory_writable (GstBuffer **buf_p)
{
  GstAllocator *allocator = NULL;
  GstBuffer *buf;
  GstBuffer *wrapper = NULL;
  GstMemory *mem;
  GstMemory *alias = NULL;
  gsize size;
  gsize offset;
  gsize maxsize;
  gint source_fd;
  gint alias_fd = -1;
  guint i, n;

  g_return_val_if_fail (buf_p != NULL, FALSE);
  g_return_val_if_fail (GST_IS_BUFFER (*buf_p), FALSE);

  buf = *buf_p;

  if (!gst_buffer_is_writable (buf)) {
    wrapper = gst_buffer_new ();
    if (!wrapper)
      return FALSE;
    if (!gst_buffer_copy_into (wrapper, buf, GST_BUFFER_COPY_METADATA, 0, -1)) {
      gst_buffer_unref (wrapper);
      return FALSE;
    }
    n = gst_buffer_n_memory (buf);
    for (i = 0; i < n; i++) {
      mem = gst_buffer_peek_memory (buf, i);
      gst_buffer_append_memory (wrapper, gst_memory_ref (mem));
    }
    gst_buffer_unref (buf);
    *buf_p = wrapper;
    buf = wrapper;
  }

  if (gst_buffer_is_all_memory_writable (buf))
    return TRUE;

  if (gst_buffer_n_memory (buf) != 1) {
    GST_ERROR ("buffer %p must contain exactly one DMA-BUF memory", buf);
    return FALSE;
  }

  mem = gst_buffer_peek_memory (buf, 0);
  if (!gst_is_dmabuf_memory (mem)) {
    GST_ERROR ("buffer %p does not contain DMA-BUF memory", buf);
    return FALSE;
  }

  size = gst_memory_get_sizes (mem, &offset, &maxsize);
  if (maxsize == 0 || offset > maxsize || size > maxsize - offset) {
    GST_ERROR ("invalid DMA-BUF layout size=%zu offset=%zu maxsize=%zu",
        size, offset, maxsize);
    return FALSE;
  }

  source_fd = gst_dmabuf_memory_get_fd (mem);
  if (source_fd < 0) {
    GST_ERROR ("failed to get DMA-BUF fd from memory %p", mem);
    return FALSE;
  }
  alias_fd = fcntl (source_fd, F_DUPFD_CLOEXEC, 0);
  if (alias_fd < 0) {
    GST_ERROR ("failed to duplicate DMA-BUF fd %d: %s", source_fd,
        g_strerror (errno));
    return FALSE;
  }
  allocator = gst_dmabuf_allocator_new ();
  alias = gst_dmabuf_allocator_alloc (allocator, alias_fd, maxsize);
  gst_object_unref (allocator);
  if (!alias) {
    GST_ERROR ("failed to wrap duplicated DMA-BUF fd %d", alias_fd);
    close (alias_fd);
    return FALSE;
  }

  gst_memory_resize (alias, offset, size);
  gst_buffer_replace_memory (buf, 0, alias);

  if (!gst_buffer_is_all_memory_writable (buf)) {
    GST_ERROR ("DMA-BUF alias on buffer %p is not writable", buf);
    return FALSE;
  }

  GST_DEBUG
      ("created writable DMA-BUF alias for buffer %p maxsize=%zu size=%zu", buf,
      maxsize, size);
  return TRUE;
}


VvasLogLevel
vvas_get_core_log_level (GstDebugLevel gst_level)
{
  switch (gst_level) {
    case GST_LEVEL_NONE:
      return VVAS_LOG_LEVEL_NONE;
    case GST_LEVEL_ERROR:
      return VVAS_LOG_LEVEL_ERROR;
    case GST_LEVEL_WARNING:
      return VVAS_LOG_LEVEL_WARNING;
    case GST_LEVEL_FIXME:
      return VVAS_LOG_LEVEL_FIXME;
    case GST_LEVEL_INFO:
      return VVAS_LOG_LEVEL_INFO;
    default:
      return VVAS_LOG_LEVEL_DEBUG;
  }
}

static void
copy_tensors_list (void *data, void *udata)
{
  TensorBuf *tb = (TensorBuf *) data;
  TensorBuf *dst_tb = NULL;
  VvasList **dst_tensors = (VvasList **) udata;

  tb->copy ((void **) &tb, (void **) &dst_tb);
  *dst_tensors = vvas_list_append (*dst_tensors, dst_tb);
}

static bool
prediction_node_assign (const VvasTreeNode *node, void *data)
{
  VvasInferPrediction *dmeta = NULL;
  if (NULL == node) {
    return false;
  }

  dmeta = (VvasInferPrediction *) (node->data);
  if (dmeta->node) {
    /* Destroy node pointing to itself */
    vvas_treenode_destroy (dmeta->node);
  }
  /* Assign the node from deep_copy */
  dmeta->node = (VvasTreeNode *) node;

  return false;
}

static void *
prediction_node_copy (const void *infer, void *data)
{
  VvasInferPrediction *dmeta = NULL;
  GstInferencePrediction *gst_infer = (GstInferencePrediction *) infer;
  VvasInferPrediction *smeta = NULL;

  if (NULL != gst_infer) {
    smeta = &(gst_infer->prediction);
    dmeta = vvas_inferprediction_new ();
  }

  if ((NULL != dmeta) && (NULL != smeta)) {
    dmeta->prediction_id = smeta->prediction_id;
    dmeta->enabled = smeta->enabled;

    if (smeta->model_path)
      dmeta->model_path = strdup (smeta->model_path);

    if (smeta->infer_result && smeta->infer_result->create &&
        smeta->infer_result->data) {
      dmeta->infer_result = smeta->infer_result->create ();
      smeta->infer_result->copy (smeta->infer_result->data,
          &(dmeta->infer_result->data));
    }

    dmeta->priv.copy = smeta->priv.copy;
    dmeta->priv.free = smeta->priv.free;
    if (smeta->priv.data && smeta->priv.copy) {
      smeta->priv.copy (smeta->priv.data, &(dmeta->priv.data));
    }

    if (smeta->tb_ref) {
      dmeta->tb_ref = tensor_buf_ref (smeta->tb_ref);
    }

    if (smeta->tensors) {
      vvas_list_foreach (smeta->tensors, copy_tensors_list, &dmeta->tensors);
    }

  }
  return (void *) dmeta;
}

VvasInferPrediction *
vvas_infer_from_gstinfer (GstInferencePrediction *pred)
{
  VvasTreeNode *node = NULL;

  if (NULL != pred) {

    node =
        vvas_treenode_copy_deep (pred->prediction.node, prediction_node_copy,
        NULL);

    vvas_treenode_traverse (node, G_IN_ORDER,
        G_TRAVERSE_ALL, -1, prediction_node_assign, NULL);

    return node->data;
  }

  return NULL;
}

static bool
gst_prediction_node_assign (const VvasTreeNode *node, void *data)
{
  GstInferencePrediction *dmeta = NULL;
  if (NULL == node) {
    return false;
  }

  dmeta = (GstInferencePrediction *) (node->data);
  if (dmeta->prediction.node) {
    /* Destroy node pointing to itself */
    vvas_treenode_destroy (dmeta->prediction.node);
  }
  /* Assign the node from deep_copy */
  dmeta->prediction.node = (VvasTreeNode *) node;

  return false;
}

static void *
vvas_prediction_node_to_gst (const void *pred, void *data)
{
  GstInferencePrediction *self = NULL;
  VvasInferPrediction *vinfer = (VvasInferPrediction *) pred;

  if (vinfer) {
    self = gst_inference_prediction_new ();

    if (NULL == self) {
      return NULL;
    }

    self->prediction.prediction_id = vinfer->prediction_id;

    if (vinfer->model_path)
      self->prediction.model_path = g_strdup (vinfer->model_path);
    self->prediction.scaled_to_root = vinfer->scaled_to_root;
    self->prediction.enabled = vinfer->enabled;
    self->prediction.width = vinfer->width;
    self->prediction.height = vinfer->height;

    if (vinfer->infer_result && vinfer->infer_result->create &&
        vinfer->infer_result->data) {
      self->prediction.infer_result = vinfer->infer_result->create ();
      vinfer->infer_result->copy (vinfer->infer_result->data,
          &(self->prediction.infer_result->data));
    }

    self->prediction.priv.copy = vinfer->priv.copy;
    self->prediction.priv.free = vinfer->priv.free;
    if (vinfer->priv.data && vinfer->priv.copy) {
      self->prediction.priv.copy (vinfer->priv.data,
          &(self->prediction.priv.data));
    }

    if (vinfer->tb_ref != NULL) {
      self->prediction.tb_ref = tensor_buf_ref (vinfer->tb_ref);
    }

    if (vinfer->tensors) {
      vvas_list_foreach (vinfer->tensors, copy_tensors_list,
          &self->prediction.tensors);
    }
  }

  return (void *) self;
}

GstInferencePrediction *
gstinfer_from_vvas_infer (VvasInferPrediction *pred)
{
  VvasTreeNode *node = NULL;

  if (!pred) {
    return NULL;
  }

  node =
      vvas_treenode_copy_deep (pred->node, vvas_prediction_node_to_gst, NULL);
  vvas_treenode_traverse (node, G_IN_ORDER, G_TRAVERSE_ALL, -1,
      gst_prediction_node_assign, NULL);
  return node->data;
}

GstInferencePrediction *
gst_infer_node_from_vvas_infer (VvasInferPrediction *vinfer)
{
  GstInferencePrediction *self = NULL;

  if (vinfer) {
    self = gst_inference_prediction_new ();

    if (NULL == self) {
      return NULL;
    }

    self->prediction.prediction_id = vinfer->prediction_id;

    if (vinfer->model_path)
      self->prediction.model_path = g_strdup (vinfer->model_path);
    self->prediction.scaled_to_root = vinfer->scaled_to_root;
    self->prediction.enabled = vinfer->enabled;
    self->prediction.width = vinfer->width;
    self->prediction.height = vinfer->height;

    if (vinfer->infer_result && vinfer->infer_result->create &&
        vinfer->infer_result->data) {
      self->prediction.infer_result = vinfer->infer_result->create ();
      vinfer->infer_result->copy (vinfer->infer_result->data,
          &(self->prediction.infer_result->data));
    }

    self->prediction.priv.copy = vinfer->priv.copy;
    self->prediction.priv.free = vinfer->priv.free;
    if (vinfer->priv.data && vinfer->priv.copy) {
      vinfer->priv.copy (vinfer->priv.data, &(self->prediction.priv.data));
    }

    if (vinfer->tb_ref != NULL) {
      self->prediction.tb_ref = tensor_buf_ref (vinfer->tb_ref);
    }

    if (vinfer->tensors) {
      vvas_list_foreach (vinfer->tensors, copy_tensors_list,
          &self->prediction.tensors);
    }

  }

  return self;
}

static void
vvas_inferprediction_get_childnodes (VvasTreeNode *node, void *data)
{
  VvasList **children = (VvasList **) data;
  VvasInferPrediction *prediction;

  if ((NULL == node) || (NULL == data)) {
    return;
  }

  prediction = (VvasInferPrediction *) node->data;

  *children = vvas_list_append (*children, prediction);
}

VvasList *
vvas_inferprediction_get_nodes (VvasInferPrediction *self)
{
  VvasList *children = NULL;

  if (NULL == self) {
    return NULL;
  }

  if (self->node) {
    vvas_treenode_traverse_child (self->node, G_TRAVERSE_ALL,
        vvas_inferprediction_get_childnodes, &children);
  }

  return children;
}

static void
free_gst_buffer (void *data, void *user_data)
{
  VvasGstUserData *gst_data = (VvasGstUserData *) user_data;
  gst_buffer_unmap (gst_data->buf, &gst_data->map_info);
  gst_buffer_unref (gst_data->buf);
  free (gst_data);
}

static void
free_gst_buffer_from_vvas_frame (void *data[], void *user_data)
{
  VvasGstUserData *gst_data = (VvasGstUserData *) user_data;
  gst_video_frame_unmap (gst_data->vframe);
  g_free (gst_data->vframe);
  free (gst_data);
}

VvasVideoFormat
get_vvas_fmt_from_gst (GstVideoFormat format)
{
  switch (format) {
    case GST_VIDEO_FORMAT_NV12:
      return VVAS_VIDEO_FORMAT_Y_UV8_420;
    case GST_VIDEO_FORMAT_I420:
      return VVAS_VIDEO_FORMAT_I420;
    case GST_VIDEO_FORMAT_BGR: /* BGR 8-bit */
      return VVAS_VIDEO_FORMAT_BGR;
    case GST_VIDEO_FORMAT_BGRA:        /* BGR 32-bit */
      return VVAS_VIDEO_FORMAT_BGRA;
    case GST_VIDEO_FORMAT_RGBA:        /* RGBA 32-bit */
      return VVAS_VIDEO_FORMAT_RGBA;
    case GST_VIDEO_FORMAT_RGB: /* RGB 8-bit */
      return VVAS_VIDEO_FORMAT_RGB;
    case GST_VIDEO_FORMAT_YUY2:        /* YUYV */
      return VVAS_VIDEO_FORMAT_YUY2;
    case GST_VIDEO_FORMAT_r210:
      return VVAS_VIDEO_FORMAT_r210;
    case GST_VIDEO_FORMAT_v308:
      return VVAS_VIDEO_FORMAT_v308;
    case GST_VIDEO_FORMAT_NV12_10LE32:
      return VVAS_VIDEO_FORMAT_NV12_10LE32;
    case GST_VIDEO_FORMAT_I422_10LE:
      return VVAS_VIDEO_FORMAT_I422_10LE;
    case GST_VIDEO_FORMAT_GRAY8:
      return VVAS_VIDEO_FORMAT_GRAY8;
    case GST_VIDEO_FORMAT_GRAY10_LE32:
      return VVAS_VIDEO_FORMAT_GRAY10_LE32;
    case GST_VIDEO_FORMAT_BGRx:
      return VVAS_VIDEO_FORMAT_BGRx;
    case GST_VIDEO_FORMAT_RGBx:
      return VVAS_VIDEO_FORMAT_RGBx;
    case GST_VIDEO_FORMAT_RGBP:
      return VVAS_VIDEO_FORMAT_RGBP;
    case GST_VIDEO_FORMAT_BGRP:
      return VVAS_VIDEO_FORMAT_BGRP;
    default:
    {
      const gchar *fmt_str = gst_video_format_to_string (format);
      VvasVideoFormat vfmt = vvas_format_from_gst_core_name (fmt_str);
      if (vfmt != VVAS_VIDEO_FORMAT_UNKNOWN) {
        return vfmt;
      }
    }
      GST_ERROR ("Not supporting %s yet", gst_video_format_to_string (format));
      return VVAS_VIDEO_FORMAT_UNKNOWN;
  }
}


GstVideoFormat
gst_coreutils_get_gst_fmt_from_vvas (VvasVideoFormat format)
{
  switch (format) {
    case VVAS_VIDEO_FORMAT_Y_UV8_420:
      return GST_VIDEO_FORMAT_NV12;
    case VVAS_VIDEO_FORMAT_I420:
      return GST_VIDEO_FORMAT_I420;
    case VVAS_VIDEO_FORMAT_BGR:        /* BGR 8-bit */
      return GST_VIDEO_FORMAT_BGR;
    case VVAS_VIDEO_FORMAT_RGB:        /* RGB 8-bit */
      return GST_VIDEO_FORMAT_RGB;
    case VVAS_VIDEO_FORMAT_YUY2:       /* YUYV */
      return GST_VIDEO_FORMAT_YUY2;
    case VVAS_VIDEO_FORMAT_r210:
      return GST_VIDEO_FORMAT_r210;
    case VVAS_VIDEO_FORMAT_v308:
      return GST_VIDEO_FORMAT_v308;
    case VVAS_VIDEO_FORMAT_NV12_10LE32:
      return GST_VIDEO_FORMAT_NV12_10LE32;
    case VVAS_VIDEO_FORMAT_I422_10LE:
      return GST_VIDEO_FORMAT_I422_10LE;
    case VVAS_VIDEO_FORMAT_GRAY8:
      return GST_VIDEO_FORMAT_GRAY8;
    case VVAS_VIDEO_FORMAT_GRAY10_LE32:
      return GST_VIDEO_FORMAT_GRAY10_LE32;
    case VVAS_VIDEO_FORMAT_RGBx:
      return GST_VIDEO_FORMAT_RGBx;
    case VVAS_VIDEO_FORMAT_BGRx:
      return GST_VIDEO_FORMAT_BGRx;
    case VVAS_VIDEO_FORMAT_BGRA:
      return GST_VIDEO_FORMAT_BGRA;
    case VVAS_VIDEO_FORMAT_RGBA:
      return GST_VIDEO_FORMAT_RGBA;
    case VVAS_VIDEO_FORMAT_NV16:
      return GST_VIDEO_FORMAT_NV16;
    case VVAS_VIDEO_FORMAT_Y410:
      return GST_VIDEO_FORMAT_Y410;
    case VVAS_VIDEO_FORMAT_RGBP:
      return GST_VIDEO_FORMAT_RGBP;
    case VVAS_VIDEO_FORMAT_BGRP:
      return GST_VIDEO_FORMAT_BGRP;
    default:
    {
      const gchar *core_name = vvas_format_gst_core_name_from_vvas (format);
      if (core_name) {
        GstVideoFormat gst_fmt = gst_video_format_from_string (core_name);
        if (gst_fmt != GST_VIDEO_FORMAT_UNKNOWN) {
          return gst_fmt;
        }
        GST_WARNING ("GStreamer core does not recognise format \"%s\"",
            core_name);
      }
    }
      GST_DEBUG ("Not supporting %d yet", format);
      return GST_VIDEO_FORMAT_UNKNOWN;
  }
}

VvasMemory *
vvas_memory_from_gstbuffer (VvasContext *vvas_ctx, uint8_t mbank_idx,
    GstBuffer *buf)
{
  VvasMemoryPrivate *priv = NULL;
  GstMemory *mem = NULL;
  VvasAllocationType alloc_type = VVAS_ALLOC_TYPE_UNKNOWN;
  gboolean bret = FALSE;
  GstMapInfo ginfo;

  if (!vvas_ctx || !buf) {
    GST_ERROR ("Invalid arguments");
    return NULL;
  }

  mem = gst_buffer_get_memory (buf, 0);
  if (mem == NULL) {
    GST_ERROR ("failed to get memory from  buffer");
    goto error;
  }

  if (vvas_ctx->dev_handle && gst_is_vvas_memory (mem)) {

    /* validate whether VvasContext's device index and GstMemory's device index is same or not */
    if (gst_vvas_memory_can_avoid_copy (mem, vvas_ctx->dev_idx, mbank_idx)) {
#ifdef XLNX_PCIe_PLATFORM
      VvasSyncFlags gst_syncflag;
#endif
      GST_DEBUG ("buffer and vvascontext are same device and memory bank");

      priv = (VvasMemoryPrivate *) calloc (1, sizeof (VvasMemoryPrivate));
      if (priv == NULL) {
        GST_ERROR ("failed to allocate memory for VvasMemory");
        goto error;
      }

      priv->boh = gst_vvas_allocator_get_bo (mem);
      alloc_type = VVAS_ALLOC_TYPE_CMA;
      priv->size = gst_buffer_get_sizes (buf, NULL, NULL);
      priv->free_cb = NULL;
      priv->ctx = vvas_ctx;

      priv->mem_info.alloc_type = alloc_type;
      priv->mem_info.alloc_flags = VVAS_ALLOC_FLAG_NONE;        /* currently gstreamer allocator supports both device and host memory allocation */
      priv->mem_info.mbank_idx = gst_vvas_memory_get_mem_bank (mem);
      if (priv->mem_info.mbank_idx == (guint) - 1) {
        GST_ERROR ("failed to get memory bank from GstVvasMemory");
        goto error;
      }
#ifdef XLNX_PCIe_PLATFORM
      gst_syncflag = gst_vvas_memory_get_sync_flag (mem);
      priv->mem_info.sync_flags = VVAS_DATA_SYNC_NONE;

      if (gst_syncflag & VVAS_SYNC_TO_DEVICE)
        priv->mem_info.sync_flags |= VVAS_DATA_SYNC_TO_DEVICE;

      if (gst_syncflag & VVAS_SYNC_FROM_DEVICE)
        priv->mem_info.sync_flags |= VVAS_DATA_SYNC_FROM_DEVICE;
#endif
      priv->mem_info.map_flags = VVAS_DATA_MAP_NONE;
    } else {
      gsize gst_buf_size = gst_buffer_get_sizes (buf, NULL, NULL);
      VvasReturnType vret;
      VvasMemoryMapInfo vinfo;

      priv =
          vvas_memory_alloc (vvas_ctx, VVAS_ALLOC_TYPE_CMA,
          VVAS_ALLOC_FLAG_NONE, mbank_idx, gst_buf_size, NULL);
      if (!priv) {
        GST_ERROR ("failed to allocate CMA memory on bank index %d", mbank_idx);
        goto error;
      }

      /*get vaddr of vvas memory to copy data from GstBuffer */
      vret = vvas_memory_map (priv, VVAS_DATA_MAP_WRITE, &vinfo);
      if (VVAS_IS_ERROR (vret)) {
        GST_ERROR ("failed to map vvas memory in write mode");
        goto error;
      }

      /* map GstBuffer to read data */
      bret = gst_buffer_map (buf, &ginfo, GST_MAP_READ);
      if (!bret) {
        GST_ERROR ("failed to map buffer in read mode");
        goto error;
      }

      /* copy data */
      memcpy (vinfo.data, ginfo.data, gst_buf_size);

      gst_buffer_unmap (buf, &ginfo);

      vret = vvas_memory_unmap (priv, &vinfo);
      if (VVAS_IS_ERROR (vret)) {
        GST_ERROR ("failed to map vvas memory in write mode");
        goto error;
      }
    }
  } else {
    VvasGstUserData *user_data = calloc (1, sizeof (VvasGstUserData));

    /* Memory is not allocated from GstVvasAllocator object */
    bret = gst_buffer_map (buf, &user_data->map_info, GST_MAP_READ);
    if (!bret) {
      GST_ERROR ("failed to map buffer in read mode");
      goto error;
    }
    user_data->buf = gst_buffer_ref (buf);

    /* allocate memory mapped virtual pointer */
    priv =
        vvas_memory_alloc_from_data (vvas_ctx, user_data->map_info.data,
        user_data->map_info.size, free_gst_buffer, user_data, NULL);
    if (!priv) {
      GST_ERROR ("failed to allocate non-CMA memory from GstBuffer");
      goto error;
    }
  }

  gst_memory_unref (mem);
  return (VvasMemory *) priv;

error:
  if (priv)
    free (priv);

  if (mem)
    gst_memory_unref (mem);

  return NULL;
}

VvasVideoFrame *
vvas_videoframe_from_gstbuffer (VvasContext *vvas_ctx, int8_t mbank_idx,
    GstBuffer *buf, GstVideoInfo *gst_vinfo, GstMapFlags flags)
{
  VvasVideoFramePriv *priv = NULL;
  GstMemory *mem = NULL;
  VvasVideoInfo vinfo = { 0, };
  VvasReturnType vret;
  uint8_t pidx;
  void *usr_meta;
  gboolean free_mem = TRUE;
  GstVideoMeta *vmeta;

  if (!vvas_ctx || !buf || !gst_vinfo) {
    GST_ERROR ("Invalid arguments");
    return NULL;
  }

  mem = gst_buffer_get_memory (buf, 0);
  if (mem == NULL) {
    GST_ERROR ("failed to get memory from  buffer");
    goto error;
  }

  vmeta = gst_buffer_get_video_meta (buf);
  if (vmeta) {
    vinfo.alignment.padding_bottom = vmeta->alignment.padding_bottom;
    vinfo.alignment.padding_top = vmeta->alignment.padding_top;
    vinfo.alignment.padding_left = vmeta->alignment.padding_left;
    vinfo.alignment.padding_right = vmeta->alignment.padding_right;
  }

  vinfo.width = GST_VIDEO_INFO_WIDTH (gst_vinfo);
  vinfo.height = GST_VIDEO_INFO_HEIGHT (gst_vinfo);
  vinfo.fmt = get_vvas_fmt_from_gst (GST_VIDEO_INFO_FORMAT (gst_vinfo));
  vinfo.n_planes = GST_VIDEO_INFO_N_PLANES (gst_vinfo);
  if (vmeta) {
    for (uint32_t idx = 0; idx < vinfo.n_planes; idx++) {
      vinfo.alignment.stride_align[idx] = vmeta->alignment.stride_align[idx];
    }
  }

  if (vvas_ctx->dev_handle && mbank_idx >= 0) {

    /* from inbuf get the pool handle */
    /* height & stride alignment  using Gobect get */

#ifdef XLNX_PCIe_PLATFORM
    VvasSyncFlags gst_syncflag;
#endif

    priv = (VvasVideoFramePriv *) calloc (1, sizeof (VvasVideoFramePriv));
    if (priv == NULL) {
      GST_ERROR ("failed to allocate memory for  VvasVideoFrame");
      goto error;
    }

    priv->log_level = vvas_ctx->log_level;
    priv->num_planes = vinfo.n_planes;
    priv->width = vinfo.width;
    priv->height = vinfo.height;
    priv->fmt = vinfo.fmt;
    priv->ctx = vvas_ctx;
    priv->mbank_idx = mbank_idx;
    priv->alignment = vinfo.alignment;

    vvas_video_get_aligned_video_info (vvas_ctx, &vinfo);

    if (vvas_fill_planes (&vinfo, priv) < 0) {
      GST_ERROR ("failed to do prepare plane info");
      goto error;
    }

    if (gst_is_vvas_memory (mem)) {
      guint actual_mbank = gst_vvas_memory_get_mem_bank (mem);

      if (actual_mbank == (guint) - 1) {
        GST_ERROR ("failed to get memory bank from GstVvasMemory");
        goto error;
      }
      priv->mbank_idx = actual_mbank;
      priv->boh =
          vvas_xrt_create_sub_bo (gst_vvas_allocator_get_bo (mem), priv->size,
          0);
    }
#ifndef XLNX_PCIe_PLATFORM
    else if (gst_is_dmabuf_memory (mem)) {
      gint dma_fd = -1;
      dma_fd = gst_dmabuf_memory_get_fd (mem);
      if (dma_fd < 0) {
        GST_ERROR ("failed to get DMABUF FD");
        goto error;
      }
      priv->boh = vvas_xrt_import_bo (vvas_ctx->dev_handle, dma_fd);
    }
#endif
    if (!priv->boh) {
      GST_ERROR ("failed to update bo handle");
      goto error;
    }

    for (pidx = 0; pidx < priv->num_planes; pidx++) {
      priv->planes[pidx].boh =
          vvas_xrt_create_sub_bo (priv->boh, priv->planes[pidx].size,
          priv->planes[pidx].offset);
      if (priv->planes[pidx].boh == NULL) {
        GST_ERROR ("failed to allocate sub BO with size %zu and offset %zu",
            priv->planes[pidx].size, priv->planes[pidx].offset);
        goto error;
      }
    }

    priv->mem_info.alloc_type = VVAS_ALLOC_TYPE_CMA;
    /* currently gstreamer allocator supports both device and host memory allocation */
    priv->mem_info.alloc_flags = VVAS_ALLOC_FLAG_NONE;
    priv->mem_info.mbank_idx = priv->mbank_idx;
    priv->mem_info.sync_flags = VVAS_DATA_SYNC_NONE;
#ifdef XLNX_PCIe_PLATFORM

    /* In case of PCIe platform, its application's responsibility
     * to copy dma buffer into vvas buffer before calling this function.
     * When control comes here, it will always be VVAS buffer.
     */

    gst_syncflag = gst_vvas_memory_get_sync_flag (mem);

    if (gst_syncflag & VVAS_SYNC_TO_DEVICE)
      priv->mem_info.sync_flags |= VVAS_DATA_SYNC_TO_DEVICE;
    if (gst_syncflag & VVAS_SYNC_FROM_DEVICE)
      priv->mem_info.sync_flags |= VVAS_DATA_SYNC_FROM_DEVICE;
#endif
    priv->mem_info.map_flags = VVAS_DATA_MAP_NONE;
  } else {                      /* This is a software buffer */
    VvasGstUserData *user_data = calloc (1, sizeof (VvasGstUserData));
    void *data[VVAS_VIDEO_MAX_PLANES];
    GstVideoFrame *vframe = NULL;

    if (NULL == user_data) {
      GST_ERROR ("failed to allocate memory -user_data");
      goto error;
    }

    vframe = g_malloc0 (sizeof (GstVideoFrame));
    if (NULL == vframe) {
      free (user_data);
      GST_ERROR ("failed to allocate memory -vframe");
      goto error;
    }

    /* map input buffer in read mode */
    if (!gst_video_frame_map (vframe, gst_vinfo, buf, flags)) {
      GST_ERROR ("failed to map input buffer");
      g_free (vframe);
      free (user_data);
      goto error;
    }

    /* This vframe info will be unmapped and freed in callback */
    user_data->vframe = vframe;

    for (pidx = 0; pidx < GST_VIDEO_FRAME_N_PLANES (vframe); pidx++) {
      data[pidx] = GST_VIDEO_FRAME_PLANE_DATA (vframe, pidx);
    }

    priv = vvas_video_frame_alloc_from_data (vvas_ctx, &vinfo, data,
        free_gst_buffer_from_vvas_frame, user_data, &vret);
    if (!priv) {
      GST_ERROR ("Failed to allocate VVAS Video Frame from data");
      g_free (vframe);
      free (user_data);
      goto error;
    }
  }
  free_mem = FALSE;

  usr_meta = gst_buffer_get_vvas_usr_meta ((GstBuffer *) buf);
  if (usr_meta) {
    priv->usr_meta = (void *) ((GstVvasUsrMeta *) usr_meta)->usr_data;
  }

error:
  if (free_mem && priv) {
    free (priv);
    priv = NULL;
  }

  if (mem)
    gst_memory_unref (mem);

  return (VvasVideoFrame *) priv;
}

VvasVideoFrame *
vvas_videoframe_from_gstbuffer_with_vvas_video_format (VvasContext *vvas_ctx,
    int8_t mbank_idx, GstBuffer *buf, GstVideoInfo *gst_vinfo,
    VvasVideoFormat video_fmt, GstMapFlags flags)
{
  VvasVideoFramePriv *priv = NULL;
  GstMemory *mem = NULL;
  VvasVideoInfo vinfo = { 0, };
  VvasReturnType vret;
  uint8_t pidx;
  void *usr_meta;
  gboolean free_mem = TRUE;
  GstVideoMeta *vmeta;

  if (!vvas_ctx || !buf || !gst_vinfo) {
    GST_ERROR ("Invalid arguments");
    return NULL;
  }

  if (video_fmt == VVAS_VIDEO_FORMAT_UNKNOWN) {
    GST_ERROR ("Invalid custom format provided %d", video_fmt);
    return NULL;
  }

  mem = gst_buffer_get_memory (buf, 0);
  if (mem == NULL) {
    GST_ERROR ("failed to get memory from  buffer");
    goto error;
  }

  vmeta = gst_buffer_get_video_meta (buf);
  if (vmeta) {
    vinfo.alignment.padding_bottom = vmeta->alignment.padding_bottom;
    vinfo.alignment.padding_top = vmeta->alignment.padding_top;
    vinfo.alignment.padding_left = vmeta->alignment.padding_left;
    vinfo.alignment.padding_right = vmeta->alignment.padding_right;
  }

  vinfo.width = GST_VIDEO_INFO_WIDTH (gst_vinfo);
  vinfo.height = GST_VIDEO_INFO_HEIGHT (gst_vinfo);
  vinfo.fmt = video_fmt;
  vinfo.n_planes = GST_VIDEO_INFO_N_PLANES (gst_vinfo);
  if (vmeta) {
    for (uint32_t idx = 0; idx < vinfo.n_planes; idx++) {
      vinfo.alignment.stride_align[idx] = vmeta->alignment.stride_align[idx];
    }
  }

  if (vvas_ctx->dev_handle && mbank_idx >= 0) {

    /* from inbuf get the pool handle */
    /* height & stride alignment  using Gobect get */

#ifdef XLNX_PCIe_PLATFORM
    VvasSyncFlags gst_syncflag;
#endif

    priv = (VvasVideoFramePriv *) calloc (1, sizeof (VvasVideoFramePriv));
    if (priv == NULL) {
      GST_ERROR ("failed to allocate memory for  VvasVideoFrame");
      goto error;
    }

    priv->log_level = vvas_ctx->log_level;
    priv->num_planes = vinfo.n_planes;
    priv->width = vinfo.width;
    priv->height = vinfo.height;
    priv->fmt = vinfo.fmt;
    priv->ctx = vvas_ctx;
    priv->mbank_idx = mbank_idx;
    priv->alignment = vinfo.alignment;

    vvas_video_get_aligned_video_info (vvas_ctx, &vinfo);

    if (vvas_fill_planes (&vinfo, priv) < 0) {
      GST_ERROR ("failed to do prepare plane info");
      goto error;
    }

    if (gst_is_vvas_memory (mem)) {
      guint actual_mbank = gst_vvas_memory_get_mem_bank (mem);

      if (actual_mbank == (guint) - 1) {
        GST_ERROR ("failed to get memory bank from GstVvasMemory");
        goto error;
      }
      priv->mbank_idx = actual_mbank;
      priv->boh =
          vvas_xrt_create_sub_bo (gst_vvas_allocator_get_bo (mem), priv->size,
          0);
    }
#ifndef XLNX_PCIe_PLATFORM
    else if (gst_is_dmabuf_memory (mem)) {
      gint dma_fd = -1;
      dma_fd = gst_dmabuf_memory_get_fd (mem);
      if (dma_fd < 0) {
        GST_ERROR ("failed to get DMABUF FD");
        goto error;
      }
      priv->boh = vvas_xrt_import_bo (vvas_ctx->dev_handle, dma_fd);
    }
#endif
    if (!priv->boh) {
      GST_ERROR ("failed to update bo handle");
      goto error;
    }

    for (pidx = 0; pidx < priv->num_planes; pidx++) {
      priv->planes[pidx].boh =
          vvas_xrt_create_sub_bo (priv->boh, priv->planes[pidx].size,
          priv->planes[pidx].offset);
      if (priv->planes[pidx].boh == NULL) {
        GST_ERROR ("failed to allocate sub BO with size %zu and offset %zu",
            priv->planes[pidx].size, priv->planes[pidx].offset);
        goto error;
      }
    }

    priv->mem_info.alloc_type = VVAS_ALLOC_TYPE_CMA;
    /* currently gstreamer allocator supports both device and host memory allocation */
    priv->mem_info.alloc_flags = VVAS_ALLOC_FLAG_NONE;
    priv->mem_info.mbank_idx = priv->mbank_idx;
    priv->mem_info.sync_flags = VVAS_DATA_SYNC_NONE;
#ifdef XLNX_PCIe_PLATFORM

    /* In case of PCIe platform, its application's responsibility
     * to copy dma buffer into vvas buffer before calling this function.
     * When control comes here, it will always be VVAS buffer.
     */

    gst_syncflag = gst_vvas_memory_get_sync_flag (mem);

    if (gst_syncflag & VVAS_SYNC_TO_DEVICE)
      priv->mem_info.sync_flags |= VVAS_DATA_SYNC_TO_DEVICE;
    if (gst_syncflag & VVAS_SYNC_FROM_DEVICE)
      priv->mem_info.sync_flags |= VVAS_DATA_SYNC_FROM_DEVICE;
#endif
    priv->mem_info.map_flags = VVAS_DATA_MAP_NONE;
  } else {                      /* This is a software buffer */
    VvasGstUserData *user_data = calloc (1, sizeof (VvasGstUserData));
    void *data[VVAS_VIDEO_MAX_PLANES];
    GstVideoFrame *vframe = NULL;

    if (NULL == user_data) {
      GST_ERROR ("failed to allocate memory -user_data");
      goto error;
    }

    vframe = g_malloc0 (sizeof (GstVideoFrame));
    if (NULL == vframe) {
      free (user_data);
      GST_ERROR ("failed to allocate memory -vframe");
      goto error;
    }

    /* map input buffer in read mode */
    if (!gst_video_frame_map (vframe, gst_vinfo, buf, flags)) {
      GST_ERROR ("failed to map input buffer");
      g_free (vframe);
      free (user_data);
      goto error;
    }

    /* This vframe info will be unmapped and freed in callback */
    user_data->vframe = vframe;

    for (pidx = 0; pidx < GST_VIDEO_FRAME_N_PLANES (vframe); pidx++) {
      data[pidx] = GST_VIDEO_FRAME_PLANE_DATA (vframe, pidx);
    }

    priv = vvas_video_frame_alloc_from_data (vvas_ctx, &vinfo, data,
        free_gst_buffer_from_vvas_frame, user_data, &vret);
    if (!priv) {
      GST_ERROR ("Failed to allocate VVAS Video Frame from data");
      g_free (vframe);
      free (user_data);
      goto error;
    }
  }
  free_mem = FALSE;

  usr_meta = gst_buffer_get_vvas_usr_meta ((GstBuffer *) buf);
  if (usr_meta) {
    priv->usr_meta = (void *) ((GstVvasUsrMeta *) usr_meta)->usr_data;
  }

error:
  if (free_mem && priv) {
    free (priv);
    priv = NULL;
  }

  if (mem)
    gst_memory_unref (mem);

  return (VvasVideoFrame *) priv;
}

/**
 *  @fn static uint32_t vvas_image_process_align (uint32_t stride_in, uint16_t alignment)
 *  @param [in] stride_in   - Input stride
 *  @param [in] alignment   - alignment requirement
 *  @return aligned value
 *  @brief  This function aligns the stride_in value to the next aligned integer value
 */
static inline uint32_t
vvas_image_process_align (uint32_t stride_in, uint16_t alignment)
{
  uint32_t stride;
  /* Align the passed value (stride_in) by passed alignment value, this will
   * return the next aligned integer */
  stride = (((stride_in) + alignment - 1) / alignment) * alignment;
  return stride;
}

/**
 *  @fn gboolean vvas_image_process_align_rect_params (const VvasImageProcessAlignReq  *align_req,
 *                                              VvasVideoFormat video_format,
 *                                              VvasImageProcessFrameRect *rect)
 *  @param [in] align_req       - VvasImageProcessAlignReq
 *  @param [in] video_format    - VvasVideoFormat for which rect need to be aligned
 *  @param [in/out] rect        - VvasImageProcessFrameRect to be aligned
 *  @return True on Success, False on failure
 *  @brief  This function aligns the given VvasImageProcessFrameRect as per the \p align_req
 *          and \p video_format and updates the aligned values back to the user in
 *          \p rect. This function expands the given rect while aligning.
 *          If rect->frame is valid it checks aligned value against the frame boundary.
 */
gboolean
vvas_image_process_align_rect_params (const VvasImageProcessAlignReq *align_req,
    VvasVideoFormat video_format, VvasImageProcessFrameRect *rect)
{
  uint32_t x_aligned, y_aligned, width_aligned, height_aligned;

  if (!align_req || !rect) {
    GST_ERROR ("invalid argument");
    return false;
  }

  if (VVAS_VIDEO_FORMAT_UNKNOWN == video_format) {
    GST_ERROR ("unknown format");
    return false;
  }

  GST_DEBUG ("Rect params: (x, y) = (%hu, %hu), (width, height) = (%hu, %hu)",
      rect->x, rect->y, rect->width, rect->height);

  /* Align X by moving it to the left */
  x_aligned = (rect->x / (align_req->x)) * (align_req->x);

  /* Add extra pixels resulted due to x alignment into the width */
  width_aligned = rect->width + (rect->x - x_aligned);

  /* Align width by moving it to right */
  width_aligned = vvas_image_process_align (width_aligned, align_req->width);

  switch (video_format) {
      /* For below formats height of UV plane is height of Y plane / 2,
       * hence align height and y coordinate by 2
       */
    case VVAS_VIDEO_FORMAT_Y_UV8_420:
    case VVAS_VIDEO_FORMAT_I420:
    case VVAS_VIDEO_FORMAT_NV16:
    case VVAS_VIDEO_FORMAT_I422_10LE:
    case VVAS_VIDEO_FORMAT_NV12_10LE32:{
      /* Align y by moving it up */
      y_aligned = (rect->y / 2) * 2;
      /* Add extra pixels resulted due to y alignment into the height */
      height_aligned = rect->y + rect->height - y_aligned;
      /* Align height by moving it down */
      height_aligned = vvas_image_process_align (height_aligned, 2);
    }
      break;

    default:{
      y_aligned = rect->y;
      height_aligned = rect->height;
    }
      break;
  }

  GST_DEBUG ("Aligned Rect params: (x, y) = (%hu, %hu), (width, height) = "
      "(%hu, %hu)", x_aligned, y_aligned, width_aligned, height_aligned);

  if (rect->frame) {
    /* VideoFrame was already allocated, check boundary condition */
    VvasVideoInfo video_info = { 0 };

    vvas_video_frame_get_videoinfo (rect->frame, &video_info);

    if ((x_aligned + width_aligned) > video_info.width) {
      GST_ERROR ("x_aligned[%u] + width_aligned[%u] is beyond the frame "
          "width[%d]", x_aligned, width_aligned, video_info.width);
      return false;
    }

    if ((y_aligned + height_aligned) > video_info.height) {
      GST_ERROR ("y_aligned[%u] + height_aligned[%u] is beyond the frame "
          "height[%d]", y_aligned, height_aligned, video_info.height);
      return false;
    }
  }

  /* Update aligned values back to the user */
  rect->x = x_aligned;
  rect->y = y_aligned;
  rect->width = width_aligned;
  rect->height = height_aligned;

  return true;
}

typedef enum
{
  VVAS_IMG_PROC_CAPS_DIR_INPUT,
  VVAS_IMG_PROC_CAPS_DIR_OUTPUT,
} VvasImageProcessCapsDirection;

/**
 *  @fn static GstCaps * build_gstcaps (const VvasImageProcessCapabilities *caps,
 *                                      const VvasVideoFormat *fmts, uint8_t n_fmts)
 *  @param [in] caps    - VvasImageProcessCapabilities (for width/height ranges)
 *  @param [in] fmts    - Array of VvasVideoFormat to include in the caps
 *  @param [in] n_fmts  - Number of entries in \p fmts
 *  @return GstCaps populated with the given format list and the width/height
 *          ranges from \p caps. Caller must free with gst_caps_unref.
 *  @brief  Shared helper for vvas_image_process_get_gstcaps_input /_output.
 */
static GstCaps *
build_gstcaps (const VvasImageProcessCapabilities *caps,
    const VvasVideoFormat *fmts, uint8_t n_fmts)
{
  GstCaps *gstcaps;
  GstStructure *st;
  GValue format = G_VALUE_INIT;

  gstcaps = gst_caps_new_empty_simple ("video/x-raw");
  if (!gstcaps) {
    return NULL;
  }

  st = gst_caps_get_structure (gstcaps, 0);
  if (!st) {
    gst_caps_unref (gstcaps);
    return NULL;
  }

  gst_structure_set (st,
      "width", GST_TYPE_INT_RANGE, caps->min_width, caps->max_width,
      "height", GST_TYPE_INT_RANGE, caps->min_height, caps->max_height,
      "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);

  g_value_init (&format, GST_TYPE_LIST);

  for (uint8_t i = 0; i < n_fmts; i++) {
    GValue v = G_VALUE_INIT;
    GstVideoFormat v_fmt;
    const char *fvft_string;
    gboolean already_added = FALSE;

    for (int j = i - 1; j >= 0; j--) {
      if (fmts[j] == fmts[i]) {
        already_added = TRUE;
        break;
      }
    }

    if (already_added) {
      continue;
    }

    g_value_init (&v, G_TYPE_STRING);

    v_fmt = gst_coreutils_get_gst_fmt_from_vvas (fmts[i]);
    if (v_fmt == GST_VIDEO_FORMAT_UNKNOWN) {
      continue;
    }

    fvft_string = gst_video_format_to_string (v_fmt);
    g_value_set_static_string (&v, fvft_string);

    gst_value_list_append_and_take_value (&format, &v);
  }

  gst_structure_take_value (st, "format", &format);

  return gstcaps;
}

/**
 *  @fn static GstCaps * build_superset_caps (const VvasImageProcessLibraryCapabilities *libs_caps,
 *                                            VvasImageProcessCapabilities *lib_caps,
 *                                            VvasImageProcessCapsDirection dir)
 *  @brief  Shared helper for vvas_image_process_get_superset_caps_input /_output.
 *          Builds an aggregated format list across all libraries for the given
 *          direction, writes the corresponding n_*_fmts / supported_*_fmts
 *          fields (plus width/height ranges) into \p lib_caps if non-NULL, and
 *          returns the equivalent GstCaps.
 */
static GstCaps *
build_superset_caps (const VvasImageProcessLibraryCapabilities *libs_caps,
    VvasImageProcessCapabilities *lib_caps, VvasImageProcessCapsDirection dir)
{
  VvasImageProcessCapabilities caps = { 0 };
  GstCaps *superset_caps = NULL;
  GList *format_list = NULL;
  uint32_t min_width, min_height, max_width, max_height;
  const gchar *dir_str =
      (dir == VVAS_IMG_PROC_CAPS_DIR_INPUT) ? "input" : "output";

  if (!libs_caps) {
    return NULL;
  }

  GST_DEBUG ("Total Image Process Libraries: %hhu", libs_caps->num_libs);

  min_width = min_height = (uint32_t) - 1;
  max_width = max_height = 0;

  for (uint8_t i = 0; i < libs_caps->num_libs; i++) {

    const VvasImageProcessCapabilities *lcaps = libs_caps->lib_caps[i];
    uint8_t n;
    const VvasVideoFormat *fmts;

    if (dir == VVAS_IMG_PROC_CAPS_DIR_INPUT) {
      n = lcaps->n_input_fmts;
      fmts = lcaps->supported_input_fmts;
    } else {
      n = lcaps->n_output_fmts;
      fmts = lcaps->supported_output_fmts;
    }

    min_width = (lcaps->min_width < min_width) ? lcaps->min_width : min_width;
    min_height =
        (lcaps->min_height < min_height) ? lcaps->min_height : min_height;

    max_width = (lcaps->max_width > max_width) ? lcaps->max_width : max_width;
    max_height =
        (lcaps->max_height > max_height) ? lcaps->max_height : max_height;

    for (uint8_t idx = 0; idx < n; idx++) {
      if (!g_list_find (format_list, GINT_TO_POINTER (fmts[idx]))) {
        format_list = g_list_prepend (format_list, GINT_TO_POINTER (fmts[idx]));
      }
    }
  }

  if (!format_list) {
    GST_ERROR ("No %s format", dir_str);
    return NULL;
  }

  format_list = g_list_reverse (format_list);

  GST_DEBUG ("Consolidated Width: [%u %u], height: [%u %u]",
      min_width, max_width, min_height, max_height);

  caps.min_width = min_width;
  caps.max_width = max_width;
  caps.min_height = min_height;
  caps.max_height = max_height;

  {
    uint8_t n = (uint8_t) g_list_length (format_list);
    VvasVideoFormat *dst_fmts;

    if (dir == VVAS_IMG_PROC_CAPS_DIR_INPUT) {
      caps.n_input_fmts = n;
      dst_fmts = caps.supported_input_fmts;
    } else {
      caps.n_output_fmts = n;
      dst_fmts = caps.supported_output_fmts;
    }

    for (uint8_t i = 0; i < n; i++) {
      dst_fmts[i] = (VvasVideoFormat)
          GPOINTER_TO_INT (g_list_nth_data (format_list, i));
    }

    superset_caps = build_gstcaps (&caps, dst_fmts, n);
  }

  g_list_free (format_list);

  if (!superset_caps) {
    GST_ERROR ("Couldn't get superset %s caps", dir_str);
  } else if (lib_caps) {
    *lib_caps = caps;
  }

  return superset_caps;
}

/**
 *  @fn GstCaps * vvas_image_process_get_gstcaps_input (const VvasImageProcessCapabilities *caps)
 */
GstCaps *
vvas_image_process_get_gstcaps_input (const VvasImageProcessCapabilities *caps)
{
  if (!caps) {
    return NULL;
  }
  return build_gstcaps (caps, caps->supported_input_fmts, caps->n_input_fmts);
}

/**
 *  @fn GstCaps * vvas_image_process_get_gstcaps_output (const VvasImageProcessCapabilities *caps)
 */
GstCaps *
vvas_image_process_get_gstcaps_output (const VvasImageProcessCapabilities *caps)
{
  if (!caps) {
    return NULL;
  }
  return build_gstcaps (caps, caps->supported_output_fmts, caps->n_output_fmts);
}

/**
 *  @fn GstCaps * vvas_image_process_get_superset_caps_input (const VvasImageProcessLibraryCapabilities * libs_caps,
 *                                                            VvasImageProcessCapabilities * lib_caps)
 */
GstCaps *
vvas_image_process_get_superset_caps_input (const
    VvasImageProcessLibraryCapabilities *libs_caps,
    VvasImageProcessCapabilities *lib_caps)
{
  return build_superset_caps (libs_caps, lib_caps,
      VVAS_IMG_PROC_CAPS_DIR_INPUT);
}

/**
 *  @fn GstCaps * vvas_image_process_get_superset_caps_output (const VvasImageProcessLibraryCapabilities * libs_caps,
 *                                                             VvasImageProcessCapabilities * lib_caps)
 */
GstCaps *
vvas_image_process_get_superset_caps_output (const
    VvasImageProcessLibraryCapabilities *libs_caps,
    VvasImageProcessCapabilities *lib_caps)
{
  return build_superset_caps (libs_caps, lib_caps,
      VVAS_IMG_PROC_CAPS_DIR_OUTPUT);
}
