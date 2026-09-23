/*
* Copyright (C) 2020 - 2022 Xilinx, Inc.  All rights reserved.
* Copyright (C) 2022 - 2026 Advanced Micro Devices, Inc.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software
* is furnished to do so, subject to the following conditions:
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
* KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO
* EVENT SHALL XILINX BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
* OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE. Except as contained in this notice, the name of the Xilinx shall
* not be used in advertising or otherwise to promote the sale, use or other
* dealings in this Software without prior written authorization from Xilinx.
*/

#include <stdio.h>
#include <string.h>             /* for strcmp */
#include <vvas/vvas_kernel.h>
#include <vvas_core/vvas_log.h>
#include <gst/gst.h>
#include <gst/video/gstvideometa.h>

/* Height and Width to test internal allocations */
#define USER_HEIGHT 720
#define USER_WIDTH 1280

#define STUB_DEFAULT_MEM_BANK 0

typedef enum
{
  VVAS_KERNEL_LIB_TYPE_UNKNOWN,
  VVAS_KERNEL_LIB_TYPE_HARDKERNEL,
  VVAS_KERNEL_LIB_TYPE_PSKERNEL,
  VVAS_KERNEL_LIB_TYPE_SOFTLIB,
} Vvas_XKernelMode;

typedef enum
{
  VVAS_ELEMENT_MODE_NOT_SUPPORTED,
  VVAS_ELEMENT_MODE_PASSTHROUGH,        /* does not alter input buffer */
  VVAS_ELEMENT_MODE_IN_PLACE,   /* going to change input buffer content */
  VVAS_ELEMENT_MODE_TRANSFORM,  /* input and output buffers are different */
} Vvas_XElementMode;

typedef struct _vvas_xstub
{
  int log_level;
  Vvas_XKernelMode kernel_type;
  Vvas_XElementMode element_mode;
} VVASXStub;



VvasReturnType xlnx_kernel_init (VVASKernel * handle);
VvasReturnType xlnx_kernel_start (VVASKernel * handle, int start,
    VVASFrame * input[MAX_NUM_OBJECT], VVASFrame * output[MAX_NUM_OBJECT]);
VvasReturnType xlnx_kernel_done (VVASKernel * handle);
VvasReturnType xlnx_kernel_deinit (VVASKernel * handle);

int32_t validate_vvas_frame (VVASKernel * handle, VVASFrame * frame);
void modify_input_frame_inplace (VVASKernel * handle, VVASFrame * frame);

static Vvas_XKernelMode
get_kernel_lib_type (const char *mode)
{
  if (!strcmp ("hard-kernel", mode))
    return VVAS_KERNEL_LIB_TYPE_HARDKERNEL;
  else if (!strcmp ("ps-kernel", mode))
    return VVAS_KERNEL_LIB_TYPE_PSKERNEL;
  else if (!strcmp ("soft-lib", mode))
    return VVAS_KERNEL_LIB_TYPE_SOFTLIB;
  else
    return VVAS_KERNEL_LIB_TYPE_UNKNOWN;
}

static Vvas_XElementMode
get_element_mode (const gchar *mode)
{
  if (!g_strcmp0 ("passthrough", mode))
    return VVAS_ELEMENT_MODE_PASSTHROUGH;
  else if (!g_strcmp0 ("inplace", mode))
    return VVAS_ELEMENT_MODE_IN_PLACE;
  else if (!g_strcmp0 ("transform", mode))
    return VVAS_ELEMENT_MODE_TRANSFORM;
  else
    return VVAS_ELEMENT_MODE_NOT_SUPPORTED;
}

VvasReturnType
xlnx_kernel_init (VVASKernel *handle)
{
  VVASXStub *kpriv;
  json_t *val;
  char *static_cfg;

  /* kernel specification info is required to validate kernel library */
  if (handle->kernel_config == NULL) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, VVAS_LOG_LEVEL_WARNING,
        "json object for static kernel configuration is NOT available");
    return VVAS_RET_ERROR;
  }

  kpriv = (VVASXStub *) calloc (1, sizeof (VVASXStub));
  if (!kpriv) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, VVAS_LOG_LEVEL_WARNING,
        "failed to allocate private handle\n");
    return VVAS_RET_ERROR;
  }

  val = json_object_get (handle->kernel_config, "debug-level");
  if (!val || !json_is_integer (val))
    kpriv->log_level = VVAS_LOG_LEVEL_WARNING;
  else
    kpriv->log_level = json_integer_value (val);

  static_cfg = json_dumps (handle->kernel_config, JSON_INDENT (2));
  VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
      "static kernel json config :\n%s", static_cfg);
  free (static_cfg);

  handle->kernel_priv = (void *) kpriv;

  val = json_object_get (handle->kernel_config, "in-mem-bank");
  if (!json_is_integer (val)) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
        "in-mem-bank is not set, setting it to %d", STUB_DEFAULT_MEM_BANK);
    handle->in_mem_bank = STUB_DEFAULT_MEM_BANK;
  } else {
    handle->in_mem_bank = json_integer_value (val);
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
        "in-mem-bank is set to %lld", json_integer_value (val));
  }

  val = json_object_get (handle->kernel_config, "out-mem-bank");
  if (!json_is_integer (val)) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
        "out-mem-bank is not set, setting it to %d", STUB_DEFAULT_MEM_BANK);
    handle->out_mem_bank = STUB_DEFAULT_MEM_BANK;
  } else {
    handle->out_mem_bank = json_integer_value (val);
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
        "out-mem-bank is set to %lld", json_integer_value (val));
  }

  /* getting kernel-lib-type for validations of VVAS handles */
  val = json_object_get (handle->kernel_config, "kernel-lib-type");
  if (!json_is_string (val)) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, kpriv->log_level,
        "failed to get kernel-lib-type\n");
    return VVAS_RET_ERROR;
  }

  kpriv->kernel_type = get_kernel_lib_type (json_string_value (val));
  if (kpriv->kernel_type == VVAS_KERNEL_LIB_TYPE_UNKNOWN) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, kpriv->log_level,
        "unknown kernel lib type : %s\n", json_string_value (val));
    return VVAS_RET_ERROR;
  }
  VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
      "kernel library type : %s\n", json_string_value (val));

  /* Log dev_handle to verify XRT device context availability */
  VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
      "Device handle: %p (NULL indicates software-only mode)",
      handle->dev_handle);

  if (kpriv->kernel_type == VVAS_KERNEL_LIB_TYPE_SOFTLIB) {
    /* kernel library is for software library */
    if (handle->dev_handle) {
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_WARNING, kpriv->log_level,
          "XRT device context available for soft-lib (xclbin-location set)");
    }

    if (handle->alloc_func) {
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_WARNING, kpriv->log_level,
          "Buffer allocation callback registered by GStreamer plugin");
    }

    if (handle->free_func) {
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_WARNING, kpriv->log_level,
          "Buffer free callback registered by GStreamer plugin");
    }

    if (handle->cb_user_data == NULL) {
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_WARNING, kpriv->log_level,
          "Callback user data not set (may be intentional)");
    }

    val = json_object_get (handle->kernel_config, "element-mode");
    if (!json_is_string (val)) {
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, kpriv->log_level,
          "failed to get element-mode in kernel config\n");
      return VVAS_RET_ERROR;
    }

    kpriv->element_mode = get_element_mode (json_string_value (val));
    if (kpriv->element_mode == VVAS_ELEMENT_MODE_NOT_SUPPORTED) {
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, kpriv->log_level,
          "Unsupported element mode : %s\n", json_string_value (val));
      return VVAS_RET_ERROR;
    }
  } else if (kpriv->kernel_type == VVAS_KERNEL_LIB_TYPE_HARDKERNEL
      || kpriv->kernel_type == VVAS_KERNEL_LIB_TYPE_PSKERNEL) {
    if (handle->dev_handle == NULL) {
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, kpriv->log_level,
          "dev_handle is not allocated by gstplugin");
      return VVAS_RET_ERROR;
    }
#ifdef XLNX_PCIe_PLATFORM
    if (kpriv->kernel_type == VVAS_KERNEL_LIB_TYPE_PSKERNEL
        && !handle->is_ps_kernel) {
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, kpriv->log_level,
          "is_ps_kernel is false when library is PSKERNEL type");
      return VVAS_RET_ERROR;
    }
#endif
  }

  val = json_object_get (handle->kernel_config, "src-stride-align");
  if (val && json_is_integer (val)) {
    vvas_caps_set_src_stride_align (handle, json_integer_value (val));
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
        "Setting the src stride align value as %lld", json_integer_value (val));
  }

  val = json_object_get (handle->kernel_config, "src-height-align");
  if (val && json_is_integer (val)) {
    vvas_caps_set_src_height_align (handle, json_integer_value (val));
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
        "Setting the src height align value as %lld", json_integer_value (val));
  }

  val = json_object_get (handle->kernel_config, "sink-stride-align");
  if (val && json_is_integer (val)) {
    vvas_caps_set_sink_stride_align (handle, json_integer_value (val));
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
        "Setting the sink stride align value as %lld",
        json_integer_value (val));
  }

  val = json_object_get (handle->kernel_config, "sink-height-align");
  if (val && json_is_integer (val)) {
    vvas_caps_set_sink_height_align (handle, json_integer_value (val));
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
        "Setting the sink height align value as %lld",
        json_integer_value (val));
  }

  VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
      "STUB Init Successfull");
  return VVAS_RET_SUCCESS;
}

VvasReturnType
xlnx_kernel_deinit (VVASKernel *handle)
{
  VVASXStub *kpriv = (VVASXStub *) handle->kernel_priv;

  if (kpriv) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level, "enter");
    free (kpriv);
  }

  return VVAS_RET_SUCCESS;
}

/**
 * @brief Draw a centered white rectangle on the frame to demonstrate inplace modification
 * 
 * The rectangle is drawn at the center of the image with size (width/2 x height/2).
 * This visually shows that inplace modification occurred while preserving most of
 * the original image content.
 */
void
modify_input_frame_inplace (VVASKernel *handle, VVASFrame *frame)
{
  VVASXStub *kpriv = (VVASXStub *) handle->kernel_priv;
  uint32_t width, height, stride;
  uint32_t rect_x, rect_y, rect_w, rect_h;

  if (kpriv->kernel_type != VVAS_KERNEL_LIB_TYPE_SOFTLIB) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
        "Inplace modification only supported for soft-lib mode");
    return;
  }

  if (!frame->vaddr[0]) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_WARNING, kpriv->log_level,
        "No virtual address available for inplace modification");
    return;
  }

  width = frame->props.width;
  height = frame->props.height;
  stride = frame->props.stride;

  /* Calculate centered rectangle bounds */
  rect_x = width / 4;
  rect_y = height / 4;
  rect_w = width / 2;
  rect_h = height / 2;

  VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
      "Drawing white rectangle at (%u,%u) size %ux%u on frame %ux%u fmt=%u stride=%u",
      rect_x, rect_y, rect_w, rect_h, width, height, frame->props.fmt, stride);

  switch (frame->props.fmt) {
    case VVAS_VFMT_RGB8:
    case VVAS_VFMT_BGR8:{
      /* RGB/BGR: 3 bytes per pixel, fill rectangle with white (255,255,255) */
      uint8_t *base = (uint8_t *) frame->vaddr[0];
      uint32_t bytes_per_pixel = 3;
      uint32_t y;

      for (y = rect_y; y < rect_y + rect_h && y < height; y++) {
        uint8_t *row = base + (y * stride) + (rect_x * bytes_per_pixel);
        memset (row, 255, rect_w * bytes_per_pixel);
      }
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
          "RGB/BGR rectangle drawn");
      break;
    }

    case VVAS_VFMT_Y_UV8_420:{
      /* NV12: Y plane + interleaved UV plane */
      /* White in YUV: Y=235 (or 255), U=128, V=128 */
      uint8_t *y_base = (uint8_t *) frame->vaddr[0];
      uint32_t y, x;

      /* Fill Y plane with white (235 for broadcast safe, 255 for full range) */
      for (y = rect_y; y < rect_y + rect_h && y < height; y++) {
        uint8_t *row = y_base + (y * stride) + rect_x;
        memset (row, 235, rect_w);
      }

      /* Fill UV plane (interleaved, half resolution) */
      if (frame->n_planes >= 2 && frame->vaddr[1]) {
        uint8_t *uv_base = (uint8_t *) frame->vaddr[1];
        uint32_t uv_rect_x = rect_x / 2;
        uint32_t uv_rect_y = rect_y / 2;
        uint32_t uv_rect_w = rect_w / 2;
        uint32_t uv_rect_h = rect_h / 2;

        for (y = uv_rect_y; y < uv_rect_y + uv_rect_h && y < height / 2; y++) {
          uint8_t *row = uv_base + (y * stride) + (uv_rect_x * 2);
          /* Fill with U=128, V=128 (interleaved) */
          for (x = 0; x < uv_rect_w; x++) {
            row[x * 2] = 128;   /* U */
            row[x * 2 + 1] = 128;       /* V */
          }
        }
      }
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
          "NV12 rectangle drawn");
      break;
    }

    case VVAS_VFMT_Y8:{
      /* Grayscale/Y-only: 1 byte per pixel */
      uint8_t *base = (uint8_t *) frame->vaddr[0];
      uint32_t y;

      for (y = rect_y; y < rect_y + rect_h && y < height; y++) {
        uint8_t *row = base + (y * stride) + rect_x;
        memset (row, 255, rect_w);
      }
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
          "Y8/GRAY8 rectangle drawn");
      break;
    }

    case VVAS_VFMT_I420:{
      /* I420/YV12: Separate Y, U, V planes */
      uint8_t *y_base = (uint8_t *) frame->vaddr[0];
      uint32_t uv_stride, uv_rect_x, uv_rect_y, uv_rect_w, uv_rect_h;
      uint32_t y;

      /* Fill Y plane */
      for (y = rect_y; y < rect_y + rect_h && y < height; y++) {
        uint8_t *row = y_base + (y * stride) + rect_x;
        memset (row, 235, rect_w);
      }

      /* Fill U and V planes (half resolution each) */
      uv_stride = stride / 2;
      uv_rect_x = rect_x / 2;
      uv_rect_y = rect_y / 2;
      uv_rect_w = rect_w / 2;
      uv_rect_h = rect_h / 2;

      if (frame->n_planes >= 2 && frame->vaddr[1]) {
        uint8_t *u_base = (uint8_t *) frame->vaddr[1];
        for (y = uv_rect_y; y < uv_rect_y + uv_rect_h; y++) {
          memset (u_base + (y * uv_stride) + uv_rect_x, 128, uv_rect_w);
        }
      }
      if (frame->n_planes >= 3 && frame->vaddr[2]) {
        uint8_t *v_base = (uint8_t *) frame->vaddr[2];
        for (y = uv_rect_y; y < uv_rect_y + uv_rect_h; y++) {
          memset (v_base + (y * uv_stride) + uv_rect_x, 128, uv_rect_w);
        }
      }
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
          "I420 rectangle drawn");
      break;
    }

    case VVAS_VFMT_RGBX8:
    case VVAS_VFMT_BGRX8:
    case VVAS_VFMT_ARGB8:
    case VVAS_VFMT_ABGR8:{
      /* RGBA/BGRA variants: 4 bytes per pixel */
      uint8_t *base = (uint8_t *) frame->vaddr[0];
      uint32_t bytes_per_pixel = 4;
      uint32_t y, x;

      for (y = rect_y; y < rect_y + rect_h && y < height; y++) {
        uint8_t *row = base + (y * stride) + (rect_x * bytes_per_pixel);
        for (x = 0; x < rect_w; x++) {
          row[x * 4] = 255;     /* R or B or A */
          row[x * 4 + 1] = 255; /* G or R */
          row[x * 4 + 2] = 255; /* B or G */
          row[x * 4 + 3] = 255; /* A or B */
        }
      }
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
          "RGBX/ARGB rectangle drawn");
      break;
    }

    default:
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_WARNING, kpriv->log_level,
          "Inplace rectangle not implemented for format %u", frame->props.fmt);
      break;
  }
}

int32_t
validate_vvas_frame (VVASKernel *handle, VVASFrame *frame)
{
  int p;
  VVASXStub *kpriv = (VVASXStub *) handle->kernel_priv;

  if (frame->n_planes > VIDEO_MAX_PLANES) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, kpriv->log_level,
        "wrong number of planes in frame %u", frame->n_planes);
    return -1;
  }
  for (p = 0; p < frame->n_planes; p++) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
        "plane %d : bo %p vaddr %p paddr %p size %u", p, frame->bo[p],
        frame->vaddr[p], (void *) frame->paddr[p], frame->size[p]);

    if (kpriv->kernel_type == VVAS_KERNEL_LIB_TYPE_HARDKERNEL
        || kpriv->kernel_type == VVAS_KERNEL_LIB_TYPE_PSKERNEL) {
      if (frame->paddr[p] == 0 || frame->paddr[p] == (uint64_t) - 1) {
        VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, kpriv->log_level,
            "not valid physical address %lu", frame->paddr[p]);
        return -1;
      }

      /* vaddr not required in case hardkernel and ps_kernel
       * but, if gstplugin assigns this it might have resulted DMA copy in PCIe platforms
       */
      if (frame->vaddr[p]) {
        VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_WARNING, kpriv->log_level,
            "vaddr %p available..possibility of DMA copy and hence performance drop",
            frame->vaddr[p]);
      }
    } else {
      if (frame->vaddr[p] == 0 || (uint64_t) frame->vaddr[p] == (uint64_t) - 1) {
        VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, kpriv->log_level,
            "not valid virtual address %p", (void *) frame->paddr[p]);
        return -1;
      }

      if (frame->paddr[p]) {
        VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_WARNING, kpriv->log_level,
            "paddr %p availble.. but not required here as kernel type is softlib",
            frame->vaddr[p]);
      }
    }
  }
  VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
      "frame props : width %u, height %u, stride %u, fmt %u",
      frame->props.width, frame->props.height, frame->props.stride,
      frame->props.fmt);
  VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
      "meta_data %p, app_priv %p, mem_type %u, n_planes %u",
      frame->meta_data, frame->app_priv, frame->mem_type, frame->n_planes);
  return 0;
}

VvasReturnType
xlnx_kernel_start (VVASKernel *handle, int start,
    VVASFrame *input[MAX_NUM_OBJECT], VVASFrame *output[MAX_NUM_OBJECT])
{
  VVASXStub *kpriv = (VVASXStub *) handle->kernel_priv;
  int idx = 0;
  int ret = 0;
  char *dynamic_cfg;
  uint user_height = USER_HEIGHT;
  uint user_width = USER_WIDTH;
  VVASFrame *user_frame;
  VVASFrameProps user_frame_props = { 0 };
  VVASMemoryType user_mem_type;
  uint16_t user_mem_bank;
  uint32_t user_buf_size;

  VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level, "enter");

  /**************************************************
   *********** Input frames validation **************
   **************************************************/
  while (idx < MAX_NUM_OBJECT) {
    if (input[idx]) {
      VVASFrame *inframe = input[idx];

      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
          "----- Input frame index %d -----", idx);
      ret = validate_vvas_frame (handle, inframe);
      if (ret < 0) {
        return VVAS_RET_ERROR;
      }
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
          "----------------------------");

      idx++;
    } else
      break;
  }
  VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
      "number of input frames received : %d", idx);

  /**************************************************
   *********** Output frames validation *************
   **************************************************/
  idx = 0;

  if (output[idx]) {

    while (idx < MAX_NUM_OBJECT) {
      if (output[idx]) {
        VVASFrame *outframe = output[idx];

        VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
            "----- Output frame index %d -----", idx);
        ret = validate_vvas_frame (handle, outframe);
        if (ret < 0) {
          return VVAS_RET_ERROR;
        }
        VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
            "----------------------------");
        idx++;
      } else
        break;
    }
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
        "number of output frames received : %d", idx);

    /* In transform mode, copy input data to output buffer */
    if (kpriv->element_mode == VVAS_ELEMENT_MODE_TRANSFORM) {
      int frame_idx = 0;
      while (frame_idx < MAX_NUM_OBJECT && input[frame_idx]
          && output[frame_idx]) {
        VVASFrame *inframe = input[frame_idx];
        VVASFrame *outframe = output[frame_idx];
        int p;

        VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
            "Transform mode: copying input frame %d to output", frame_idx);

        /* Copy each plane from input to output */
        for (p = 0; p < inframe->n_planes && p < outframe->n_planes; p++) {
          size_t in_size = inframe->size[p];
          size_t out_size = outframe->size[p];

          /* If size is not set, compute per-plane using format-aware logic.
           * Chroma planes are smaller than luma for subsampled formats. */
          if (in_size == 0 && inframe->props.stride > 0
              && inframe->props.height > 0) {
            size_t plane_stride = inframe->props.stride;
            size_t plane_height = inframe->props.height;
            if (p > 0) {
              switch (inframe->props.fmt) {
                case VVAS_VFMT_Y_UV8_420:
                  plane_height = inframe->props.height / 2;
                  break;
                case VVAS_VFMT_I420:
                  plane_stride = inframe->props.stride / 2;
                  plane_height = inframe->props.height / 2;
                  break;
                default:
                  break;
              }
            }
            in_size = plane_stride * plane_height;
          }
          if (out_size == 0 && outframe->props.stride > 0
              && outframe->props.height > 0) {
            size_t plane_stride = outframe->props.stride;
            size_t plane_height = outframe->props.height;
            if (p > 0) {
              switch (outframe->props.fmt) {
                case VVAS_VFMT_Y_UV8_420:
                  plane_height = outframe->props.height / 2;
                  break;
                case VVAS_VFMT_I420:
                  plane_stride = outframe->props.stride / 2;
                  plane_height = outframe->props.height / 2;
                  break;
                default:
                  break;
              }
            }
            out_size = plane_stride * plane_height;
          }

          if (inframe->vaddr[p] && outframe->vaddr[p] && in_size > 0) {
            size_t copy_size = (in_size < out_size) ? in_size : out_size;
            memcpy (outframe->vaddr[p], inframe->vaddr[p], copy_size);
            VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
                "Copied plane %d: %zu bytes from %p to %p",
                p, copy_size, inframe->vaddr[p], outframe->vaddr[p]);
          } else {
            VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_WARNING, kpriv->log_level,
                "Plane %d: vaddr not available for copy (in=%p, out=%p) or size is 0",
                p, inframe->vaddr[p], outframe->vaddr[p]);
          }
        }
        frame_idx++;
      }
    }
  } else {
    /* no output buffers in case of passthrough/inplace mode */
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
        "number of output frames received : %d", idx);
    idx = 0;
    while (idx < MAX_NUM_OBJECT) {
      if (input[idx]) {
        if (kpriv->element_mode == VVAS_ELEMENT_MODE_IN_PLACE) {
          VVASFrame *inframe = input[idx];

          VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
              "Modifying the input %d in inplace mode", idx);
          modify_input_frame_inplace (handle, inframe);


        } else if (kpriv->element_mode == VVAS_ELEMENT_MODE_PASSTHROUGH) {
          VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
              "input %d is passthrough", idx);
        }

        idx++;
      } else
        break;
    }

  }

  /**************************************************
   *********** Internal frames allocation ***********
   **************************************************/

  /* VVAS_FRAME_MEMORY validation - works for all kernel types
   * Uses GStreamer buffer pool allocation */
  /* Use actual input frame dimensions and format to match the pool */
  if (input[0]) {
    user_height = input[0]->props.height;
    user_width = input[0]->props.width;
    user_frame_props.fmt = input[0]->props.fmt;
  } else {
    user_frame_props.fmt = VVAS_VFMT_Y_UV8_420;
  }

  user_frame_props.height = user_height;
  user_frame_props.width = user_width;
  user_mem_type = VVAS_FRAME_MEMORY;
  user_mem_bank = STUB_DEFAULT_MEM_BANK;
  /* If memtype is a VVAS_FRAME_MEMORY size is calculated by gstreamer plugin, so setting it to 0 */
  user_buf_size = 0;
  user_frame =
      vvas_alloc_buffer (handle, user_buf_size, user_mem_type, user_mem_bank,
      &user_frame_props);

  if (user_frame == NULL) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, kpriv->log_level,
        "Couldn't allocate vvas frame");
    return VVAS_RET_ERROR;
  }

  if (user_frame->app_priv) {
    GstVideoMeta *vmeta;
    GstBuffer *buffer = (GstBuffer *) user_frame->app_priv;
    int p;
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
        "Allocated vvas frame from gst memory pool %d", user_frame->n_planes);

    vmeta = gst_buffer_get_video_meta (buffer);
    if (vmeta == NULL) {
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, kpriv->log_level,
          "Could not find vmeta in gst buffer");

      return VVAS_RET_ERROR;
    }


    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
        "Gst Buffer height %d width %d Planes %d", vmeta->height,
        vmeta->width, vmeta->n_planes);


    for (p = 0; p < vmeta->n_planes; p++) {
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
          "stride of plane_%d is %d ", p, vmeta->stride[p]);
    }

    for (p = 0; p < user_frame->n_planes; p++) {
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
          "plane %d : bo %p vaddr %p paddr %p size %u", p,
          user_frame->bo[p], user_frame->vaddr[p],
          (void *) user_frame->paddr[p], user_frame->size[p]);

    }
  }

  vvas_free_buffer (handle, user_frame);


  /* VVAS_INTERNAL_MEMORY validation - allocates device memory via XRT
   * Works for all kernel types when dev_handle is available */
  if (handle->dev_handle) {
    user_mem_type = VVAS_INTERNAL_MEMORY;

    user_buf_size = user_height * user_width * 1.5;

    /* No need to pass VVASFrameProps while using VVAS_INTERNAL_MEMORY */
    user_frame =
        vvas_alloc_buffer (handle, user_buf_size, user_mem_type, user_mem_bank,
        NULL);

    if (user_frame == NULL) {
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, kpriv->log_level,
          "Couldn't allocate vvas frame from internal memory");
      return VVAS_RET_ERROR;
    }

    if (user_frame->bo[0]) {
      int p = 0;
      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
          "Allocated vvas frame from internal memory (dev_handle=%p)",
          handle->dev_handle);

      VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_INFO, kpriv->log_level,
          "plane %d : bo %p vaddr %p paddr %p size %u", p, user_frame->bo[0],
          user_frame->vaddr[0], (void *) user_frame->paddr[0],
          user_frame->size[0]);

    }

    vvas_free_buffer (handle, user_frame);
  } else {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
        "Skipping VVAS_INTERNAL_MEMORY test (no dev_handle available)");
  }

  dynamic_cfg = json_dumps (handle->kernel_dyn_config, JSON_INDENT (2));
  VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level,
      "dynamic kernel json config :\n%s", dynamic_cfg);

  return VVAS_RET_SUCCESS;
}

VvasReturnType
xlnx_kernel_done (VVASKernel *handle)
{
  VVASXStub *kpriv = (VVASXStub *) handle->kernel_priv;

  VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_DEBUG, kpriv->log_level, "Kernel Done\n");
  return VVAS_RET_SUCCESS;
}
