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

#ifndef __VVAS_GST_CORE_UTILS__
#define __VVAS_GST_CORE_UTILS__

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/vvas/gstvvasallocator.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/vvas/gstvvasusrmeta.h>
#include <vvas_core/vvas_memory.h>
#include <vvas_core/vvas_memory_priv.h>
#include <vvas_core/vvas_video.h>
#include <vvas_core/vvas_video_priv.h>
#include <vvas_core/vvas_infer_prediction.h>
#include <vvas_utils/vvas_infer_results.h>
#include <vvas_core/vvas_image_process.h>
#include "gstinferenceprediction.h"

#define DEFAULT_DEBUG_LOG_LEVEL VVAS_LOG_LEVEL_ERROR
#ifdef __cplusplus
extern "C"
{
#endif

typedef struct {
  guint64 sum_us;
  guint64 min_us;
  guint64 max_us;
  guint64 count;
  guint64 init_us;
  guint64 deinit_us;
  gboolean enabled;
} VvasProfilerStats;

#define VVAS_PROFILER_STATS_INIT(s) do {                          \
                                    (s)->sum_us=0;               \
                                    (s)->min_us=G_MAXUINT64;     \
                                    (s)->max_us=0; (s)->count=0; \
                                    (s)->init_us=0;              \
                                    (s)->deinit_us=0;            \
                                    (s)->enabled=FALSE;          \
                                 } while (0)

/**
 * @fn inline guint64 vvas_profiler_now_us (void)
 * @brief Get the current time in microseconds
 * @return Current time in microseconds
 */
inline guint64 vvas_profiler_now_us (void)
{
  /* g_get_monotonic_time -> microseconds */
  return (guint64) g_get_monotonic_time ();
}

/**
 * @fn inline void vvas_profiler_stats_update (VvasProfilerStats *s, guint64 dur_us)
 * @brief Update the profiling statistics for a specific stage
 * @param [in/out] s      - VvasProfilerStats structure to update
 * @param [in] num_frames - Number of frames to add to the stats
 * @param [in] dur_us     - Duration in microseconds to add to the stats
 */
inline void vvas_profiler_stats_update (VvasProfilerStats *s, guint64 num_frames, guint64 dur_us) {
  guint64 avg_us = dur_us / num_frames;
  s->sum_us += dur_us;
  s->count += num_frames;
  if (avg_us < s->min_us) s->min_us = avg_us;
  if (avg_us > s->max_us) s->max_us = avg_us;
}

GST_EXPORT
VvasLogLevel vvas_get_core_log_level (GstDebugLevel gst_level);

GST_EXPORT
VvasMemory * vvas_memory_from_gstbuffer (VvasContext * vvas_ctx, uint8_t mbank_idx, GstBuffer * buf);

GST_EXPORT
VvasVideoFrame * vvas_videoframe_from_gstbuffer_with_vvas_video_format (VvasContext * vvas_ctx,
                                                                        int8_t mbank_idx, GstBuffer * buf,
                                                                        GstVideoInfo * gst_vinfo,
                                                                        VvasVideoFormat video_fmt,
                                                                        GstMapFlags flags);

GST_EXPORT 
VvasVideoFrame *vvas_videoframe_from_gstbuffer (VvasContext *vvas_ctx,
                                                int8_t mbank_idx, GstBuffer * buf, GstVideoInfo * gst_vinfo,
                                                GstMapFlags flags);

/**
 *  @fn gboolean gst_vvas_buffer_make_dmabuf_memory_writable
 *      (GstBuffer **buf)
 *  @param [in/out] buf - GstBuffer to wrap with a writable DMA-BUF alias
 *  @return TRUE on success, FALSE on failure
 *  @brief  Replace non-writable DMA-BUF GstMemory with a writable alias of
 *          the same fd, preserving the original maxsize.
 *  @details If @buf is not writable, a new buffer wrapper is returned
 *           (metadata copied, pixels not copied).
 */
GST_EXPORT
gboolean gst_vvas_buffer_make_dmabuf_memory_writable (GstBuffer **buf);

GST_EXPORT
VvasInferPrediction * vvas_infer_from_gstinfer (GstInferencePrediction *pred);

GST_EXPORT
GstInferencePrediction * gstinfer_from_vvas_infer (VvasInferPrediction *pred);

GST_EXPORT
VvasList * vvas_inferprediction_get_nodes (VvasInferPrediction * self);

GST_EXPORT
GstInferencePrediction * gst_infer_node_from_vvas_infer (VvasInferPrediction * vinfer);

GST_EXPORT
GstVideoFormat gst_coreutils_get_gst_fmt_from_vvas (VvasVideoFormat format);

GST_EXPORT
VvasVideoFormat get_vvas_fmt_from_gst (GstVideoFormat format);

/**
 *  @fn gboolean vvas_image_process_align_rect_params (const VvasImageProcessAlignReq  *align_req,
 *                                              VvasVideoFormat video_format,
 *                                              VvasImageProcessFrameRect *rect)
 *  @param [in] align_req       - VvasImageProcessAlignReq
 *  @param [in] video_format    - VvasVideoFormat of the Rect
 *  @param [in/out] rect        - VvasImageProcessFrameRect to be aligned
 *  @return True on Success, False on failure
 *  @brief  This function aligns the given VvasImageProcessFrameRect as per the \p align_req
 *          and \p video_format and updates the aligned values back to the user in
 *          \p rect. This function expands the given rect while aligning.
 *          If rect->frame is valid it checks aligned value against the frame boundary.
 */
GST_EXPORT
gboolean vvas_image_process_align_rect_params (const VvasImageProcessAlignReq  *align_req,
    VvasVideoFormat video_format, VvasImageProcessFrameRect *rect);

/**
 *  @fn GstCaps * vvas_image_process_get_gstcaps_input (const VvasImageProcessCapabilities *caps)
 *  @param [in]  caps  - VvasImageProcessCapabilities for which input GstCaps is needed
 *  @return GstCaps containing input-accepted formats from \p caps
 *  @brief  This function creates a GstCaps from supported_input_fmts in \p caps.
 *          Caller must free this GstCaps using gst_caps_unref;
 */
GstCaps *
vvas_image_process_get_gstcaps_input (const VvasImageProcessCapabilities *caps);

/**
 *  @fn GstCaps * vvas_image_process_get_gstcaps_output (const VvasImageProcessCapabilities *caps)
 *  @param [in]  caps  - VvasImageProcessCapabilities for which output GstCaps is needed
 *  @return GstCaps containing output-produced formats from \p caps
 *  @brief  This function creates a GstCaps from supported_output_fmts in \p caps.
 *          Caller must free this GstCaps using gst_caps_unref;
 */
GstCaps *
vvas_image_process_get_gstcaps_output (const VvasImageProcessCapabilities *caps);

/**
 *  @fn GstCaps * vvas_image_process_get_superset_caps_input (const VvasImageProcessLibraryCapabilities * libs_caps,
                                                 VvasImageProcessCapabilities * lib_caps)
 *  @param [in]  libs_caps  - VvasImageProcessLibraryCapabilities for which superset input GstCaps is needed
 *  @param [out] lib_caps   - Superset VvasImageProcessCapabilities (input fields populated)
 *  @return Superset GstCaps containing input-accepted formats
 *  @brief  This function creates a superset GstCaps from the supported_input_fmts across
 *          all libraries in \p libs_caps and returns the aggregated VvasImageProcessCapabilities
 *          in \p lib_caps (with n_input_fmts / supported_input_fmts populated).
 *          Caller must free this GstCaps using gst_caps_unref;
 */
GstCaps *
vvas_image_process_get_superset_caps_input (const VvasImageProcessLibraryCapabilities *libs_caps,
                               VvasImageProcessCapabilities * lib_caps);

/**
 *  @fn GstCaps * vvas_image_process_get_superset_caps_output (const VvasImageProcessLibraryCapabilities * libs_caps,
                                                 VvasImageProcessCapabilities * lib_caps)
 *  @param [in]  libs_caps  - VvasImageProcessLibraryCapabilities for which superset output GstCaps is needed
 *  @param [out] lib_caps   - Superset VvasImageProcessCapabilities (output fields populated)
 *  @return Superset GstCaps containing output-produced formats
 *  @brief  This function creates a superset GstCaps from the supported_output_fmts across
 *          all libraries in \p libs_caps and returns the aggregated VvasImageProcessCapabilities
 *          in \p lib_caps (with n_output_fmts / supported_output_fmts populated).
 *          Caller must free this GstCaps using gst_caps_unref;
 */
GstCaps *
vvas_image_process_get_superset_caps_output (const VvasImageProcessLibraryCapabilities *libs_caps,
                               VvasImageProcessCapabilities * lib_caps);

/**
 * vvas_format_gst_core_name_from_vvas:
 * @fmt: VvasVideoFormat enum value
 *
 * Returns: the native GStreamer format string for the given VVAS format
 *          enum, or NULL if no mapping exists.
 */
static inline const gchar *
vvas_format_gst_core_name_from_vvas (VvasVideoFormat fmt)
{
  switch (fmt) {
    case VVAS_VIDEO_FORMAT_RGBx_BF16:
      return "RGBX_BF16_C4";
    case VVAS_VIDEO_FORMAT_BGRx_BF16:
      return "BGRX_BF16_C4";
    case VVAS_VIDEO_FORMAT_RGB_BF16:
      return "RGB_BF16";
    case VVAS_VIDEO_FORMAT_BGR_BF16:
      return "BGR_BF16";
    case VVAS_VIDEO_FORMAT_RGBP_BF16:
      return "RGB_BF16P";
    case VVAS_VIDEO_FORMAT_BGRP_BF16:
      return "BGR_BF16P";

    case VVAS_VIDEO_FORMAT_RGBx_FP16:
      return "RGBX_FP16_C4";
    case VVAS_VIDEO_FORMAT_BGRx_FP16:
      return "BGRX_FP16_C4";
    case VVAS_VIDEO_FORMAT_RGB_FP16:
      return "RGB_FP16";
    case VVAS_VIDEO_FORMAT_BGR_FP16:
      return "BGR_FP16";
    case VVAS_VIDEO_FORMAT_RGBP_FP16:
      return "RGB_FP16P";
    case VVAS_VIDEO_FORMAT_BGRP_FP16:
      return "BGR_FP16P";

    case VVAS_VIDEO_FORMAT_RGBx_C8:
      return "RGBX8_C8";
    case VVAS_VIDEO_FORMAT_RGBx_BF16_C8:
      return "RGBX_BF16_C8";
    case VVAS_VIDEO_FORMAT_RGBx_FP16_C8:
      return "RGBX_FP16_C8";

    case VVAS_VIDEO_FORMAT_RGBP_FLOAT:
      return "RGB_FLOATP";
    case VVAS_VIDEO_FORMAT_BGRP_FLOAT:
      return "BGR_FLOATP";
    case VVAS_VIDEO_FORMAT_RGB_FLOAT:
      return "RGB_FLOAT";
    case VVAS_VIDEO_FORMAT_BGR_FLOAT:
      return "BGR_FLOAT";

    case VVAS_VIDEO_FORMAT_GRAY_BF16:
      return "GRAY_BF16";
    case VVAS_VIDEO_FORMAT_GRAY_FP16:
      return "GRAY_FP16";
    case VVAS_VIDEO_FORMAT_GRAY32_FLOAT:
      return "GRAY_FLOAT";

    default:
      return NULL;
  }
}

static inline VvasVideoFormat
vvas_format_from_gst_core_name (const gchar *gst_name)
{
  if (!gst_name)
    return VVAS_VIDEO_FORMAT_UNKNOWN;
  if (g_str_equal (gst_name, "RGBX_BF16_C4"))
    return VVAS_VIDEO_FORMAT_RGBx_BF16;
  if (g_str_equal (gst_name, "BGRX_BF16_C4"))
    return VVAS_VIDEO_FORMAT_BGRx_BF16;
  if (g_str_equal (gst_name, "RGB_BF16"))
    return VVAS_VIDEO_FORMAT_RGB_BF16;
  if (g_str_equal (gst_name, "BGR_BF16"))
    return VVAS_VIDEO_FORMAT_BGR_BF16;
  if (g_str_equal (gst_name, "RGB_BF16P"))
    return VVAS_VIDEO_FORMAT_RGBP_BF16;
  if (g_str_equal (gst_name, "BGR_BF16P"))
    return VVAS_VIDEO_FORMAT_BGRP_BF16;

  if (g_str_equal (gst_name, "RGBX_FP16_C4"))
    return VVAS_VIDEO_FORMAT_RGBx_FP16;
  if (g_str_equal (gst_name, "BGRX_FP16_C4"))
    return VVAS_VIDEO_FORMAT_BGRx_FP16;
  if (g_str_equal (gst_name, "RGB_FP16"))
    return VVAS_VIDEO_FORMAT_RGB_FP16;
  if (g_str_equal (gst_name, "BGR_FP16"))
    return VVAS_VIDEO_FORMAT_BGR_FP16;
  if (g_str_equal (gst_name, "RGB_FP16P"))
    return VVAS_VIDEO_FORMAT_RGBP_FP16;
  if (g_str_equal (gst_name, "BGR_FP16P"))
    return VVAS_VIDEO_FORMAT_BGRP_FP16;

  if (g_str_equal (gst_name, "RGBX8_C8"))
    return VVAS_VIDEO_FORMAT_RGBx_C8;
  if (g_str_equal (gst_name, "RGBX_BF16_C8"))
    return VVAS_VIDEO_FORMAT_RGBx_BF16_C8;
  if (g_str_equal (gst_name, "RGBX_FP16_C8"))
    return VVAS_VIDEO_FORMAT_RGBx_FP16_C8;

  if (g_str_equal (gst_name, "RGB_FLOATP"))
    return VVAS_VIDEO_FORMAT_RGBP_FLOAT;
  if (g_str_equal (gst_name, "BGR_FLOATP"))
    return VVAS_VIDEO_FORMAT_BGRP_FLOAT;
  if (g_str_equal (gst_name, "RGB_FLOAT"))
    return VVAS_VIDEO_FORMAT_RGB_FLOAT;
  if (g_str_equal (gst_name, "BGR_FLOAT"))
    return VVAS_VIDEO_FORMAT_BGR_FLOAT;

  if (g_str_equal (gst_name, "GRAY_BF16"))
    return VVAS_VIDEO_FORMAT_GRAY_BF16;
  if (g_str_equal (gst_name, "GRAY_FP16"))
    return VVAS_VIDEO_FORMAT_GRAY_FP16;
  if (g_str_equal (gst_name, "GRAY_FLOAT"))
    return VVAS_VIDEO_FORMAT_GRAY32_FLOAT;
  return VVAS_VIDEO_FORMAT_UNKNOWN;
}

#ifdef __cplusplus
}
#endif
#endif
