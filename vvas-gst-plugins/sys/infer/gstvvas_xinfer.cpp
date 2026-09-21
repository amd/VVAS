/*
 * Copyright (C) 2020 - 2022 Xilinx, Inc.  All rights reserved.
 * Copyright (C) 2022 - 2025 Advanced Micro Devices, Inc.
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

/*
 * gstvvas_xinfer.cpp: This file contains the implementation of the GstVvas_XInfer plugin.
 * This plugin is used to perform inference on the input video frames using the VART::Runner, and
 * VVAS acceleration library.
 * The plugin supports pre-processing, inference, and post-processing of the input video frames in
 * different threads.
 * The plugin supports multiple inference levels, where the output of one inference level is used as
 * input to the next inference level.
 */

#include <vart/vart_runner_factory.hpp>
#ifdef HAVE_CONFIG_H
#include "config.h"
#else
#define VVAS_API_VERSION "1.0.0"
#endif

#include <iostream>
#include <sstream>
#include <atomic>
#include <optional>
#include <numeric>
#include <dlfcn.h>              /* for dlXXX APIs */
#include <sys/mman.h>           /* for munmap */
#include <jansson.h>
#include <math.h>

#ifdef XLNX_PCIe_PLATFORM
#include <experimental/xrt-next.h>
#else
#include <xrt/experimental/xrt-next.h>
#endif

#include "gstvvas_xinfer_helper.h"
#include <vvas_core/vvas_video_priv.h>
#include <vvas_core/vvas_memory_priv.h>
#include <gst/vvas/gstvvaslogbridge.h>

#include "gstvvas_xinfer.h"


/**
 *  @brief Defines a GstDebugCategory global variable "gst_vvas_xinfer_debug"
 */
GST_DEBUG_CATEGORY (gst_vvas_xinfer_debug);

/** @def GST_CAT_DEFAULT
 *  @brief Setting gst_vvas_xinfer_debug as default debug category for logging
 */
#define GST_CAT_DEFAULT gst_vvas_xinfer_debug

/**
 *  @brief Defines a static GstDebugCategory global variable with name
 *  GST_CAT_PERFORMANCE for performance logging purpose
 */
GST_DEBUG_CATEGORY_STATIC (GST_CAT_PERFORMANCE);

/** @def PRINT_METADATA_TREE
 *  @brief Enable prints inference metadata
 */
#define PRINT_METADATA_TREE

/** @def DUMP_INFER_INPUT
 *  @brief Dump raw data in file which is prepared for inference
 */
#undef DUMP_INFER_INPUT
/*#define DUMP_INFER_INPUT*/

/** @def PPE_STRIDE_ALIGN
 *  @brief Stride alignment requirement for PPE
 */
#define PPE_STRIDE_ALIGN  (self->priv->pre_proc->caps->alignment_req.stride)

/** @def PPE_STRIDE_ALIGN
 *  @brief Alignment for height for PPE.
 */
#define PPE_HEIGHT_ALIGN  2

/** @def DEFAULT_ATTACH_EMPTY_METADATA
 * @brief Default flag to enable or disable attach metadata
 */
#define DEFAULT_ATTACH_EMPTY_METADATA  TRUE

#ifdef XLNX_PCIe_PLATFORM
/* In PCIe platforms only we will have multiple devices.
 * In Embedded platforms, we will have single device with dev-idx = 0 */
#define DEFAULT_VVAS_LIB_PATH "/opt/xilinx/vvas/lib/"
#define PPE_DEVICE_IDX -1
#define XDNA_DEVICE_IDX 0
#define USE_DMABUF 0
#define USE_DMABUF_EXPORT 0
#else
#define DEFAULT_VVAS_LIB_PATH "/usr/lib/"
#define PPE_DEVICE_IDX 1
#define XDNA_DEVICE_IDX 0
#define USE_DMABUF 0
#define USE_DMABUF_EXPORT 1
#endif

/** @def DEFAULT_BATCH_SUBMIT_TIMEOUT
 *  *  @brief Time to wait in milliseconds before pushing current batch
 *   */
#define DEFAULT_BATCH_SUBMIT_TIMEOUT  2000

/** @def TENSOR_POOL_SIZE_MULTIPLIER
 *  @brief VvasTensorPool size multiplication factor
 */
#define TENSOR_POOL_SIZE_MULTIPLIER 3

/** @def DEFAULT_QT_FCTR
 *  @brief Default quantization factor
 */
#define DEFAULT_QT_FCTR 1.0

GQuark _scale_quark;
GQuark _copy_quark;

typedef struct _GstVvas_XInferPrivate GstVvas_XInferPrivate;

enum
{
  /** Signal to be emitted when acceleration library processed a frame */
  SIGNAL_VVAS,

  /* add more signal above this */
  SIGNAL_LAST
};

static guint vvas_signals[SIGNAL_LAST] = { 0 };

enum
{
  PROP_0,
  /** Property ID of the config-file property */
  PROP_CONFIG_LOCATION,
  /** Property ID to indicate attach empty metadata or not */
  PROP_ATTACH_EMPTY_METADATA,
  /** Property ID for timeout to submit batch */
  PROP_BATCH_SUBMIT_TIMEOUT,
  /** Property ID to enable or disable profiling */
  PROP_ENABLE_PROFILER,
  /** Property ID for profiler output file */
  PROP_PROFILER_FILE,
  /** Property ID to set profiling log interval (in seconds) */
  PROP_PROFILER_LOG_INTERVAL
};

typedef enum
{
  VVAS_XINFER_ML_API_ONNX,
  VVAS_XINFER_ML_API_VART,
  VVAS_XINFER_ML_API_MAX
} VvasXinferMlApi;

/** @struct Vvas_XInferNodeInfo
 *  @brief  Contains video information of particular node
 */
typedef struct _vvas_xinfer_nodeinfo
{
  /** Handle to private members */
  GstVvas_XInfer *self;
  /** Video info of parent node */
  GstVideoInfo *parent_vinfo;
  /** Video info of node */
  GstVideoInfo *child_vinfo;
  /** vvas input frame roi data*/
  vvas_ms_roi input_roi;
  /** vvas output frame roi data*/
  vvas_ms_roi output_roi;
  /** flag to use roi data for update metadata*/
  gboolean use_roi_data;
} Vvas_XInferNodeInfo;

/** @struct Vvas_XInferNumSubs
 *  @brief  Contains sub buffer information
 */
typedef struct _vvas_xinfer_numsubs
{
  /** Handle to private members */
  GstVvas_XInfer *self;
  /** No. of available Sub buffers */
  guint available_buffer;
  /** No. of required Sub buffers */
  guint required_buffer;
} Vvas_XInferNumSubs;


/** @struct _GstVvas_XInferPrivate
 *  @brief  Contains private member of xinfer
 */
struct _GstVvas_XInferPrivate
{
  /*common members */
  /** Helps to return from Query, before Initialization of kernels */
  gboolean do_init;
  /** Holds Input video configuration */
  GstVideoInfo *in_vinfo;
  /** internal input buffer pool */
  GstBufferPool *input_pool;
  /** Stop triggered on EOS or ctrl^c */
    atomic < gboolean > stop;
  /** Holds last status of pad_push, to be returned in generate_output */
    atomic < GstFlowReturn > last_fret;
  /** Sets on GST_EVENT_EOS */
    atomic < gboolean > is_eos;
  /** Sets on CUSTOM_PAD_EOS */
    atomic < gboolean > is_pad_eos;
  /** Holds status on error conditions */
    atomic < gboolean > is_error;
  /** Instance name of the element */
  gchar *instance_name;

    std::unique_ptr < PreProcessInfo > pre_proc;
    std::unique_ptr < InferInfo > infer;
    std::unique_ptr < PostProcessInfo > post_proc;

  /** Inference core Log level */
  VvasLogLevel core_log_level;

  /** Profiling information for inference operations */
  VvasInferProfiler infer_profiler;

#ifdef DUMP_INFER_INPUT
  /** pointer to output FILE used for dumping all input frame to infer */
  FILE *fp;
#endif

  /** whether to attach tensors or not */
  gboolean attach_tensors;
};

/**
 *  @var GstStaticPadTemplate sink_template
 *  @brief Contains capabilities associated with xinfer's sink pad
 */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE
        ("{NV12, BGR, RGB, BGRA, RGBA, RGBx, BGRx, "
            "RGBX_BF16_C4, BGRX_BF16_C4, RGBX_BF16_C8, "
            "RGB_BF16, BGR_BF16, "
            "RGB_BF16P, BGR_BF16P, "
            "RGBX_FP16_C4, BGRX_FP16_C4, RGBX_FP16_C8, RGBX8_C8, "
            "RGB_FP16, BGR_FP16, "
            "RGB_FP16P, BGR_FP16P, "
            "RGB_FLOAT, BGR_FLOAT, RGB_FLOATP, BGR_FLOATP, "
            "GRAY_BF16, GRAY_FP16, GRAY_FLOAT}")));

/**
 *  @var GstStaticPadTemplate src_template
 *  @brief Contains capabilities associated with xinfer's src pad
 */
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE
        ("{NV12, BGR, RGB, BGRA, RGBA, RGBx, BGRx, "
            "RGBX_BF16_C4, BGRX_BF16_C4, RGBX_BF16_C8, "
            "RGB_BF16, BGR_BF16, "
            "RGB_BF16P, BGR_BF16P, "
            "RGBX_FP16_C4, BGRX_FP16_C4, RGBX_FP16_C8, RGBX8_C8, "
            "RGB_FP16, BGR_FP16, "
            "RGB_FP16P, BGR_FP16P, "
            "RGB_FLOAT, BGR_FLOAT, RGB_FLOATP, BGR_FLOATP, "
            "GRAY_BF16, GRAY_FP16, GRAY_FLOAT}")));

#define gst_vvas_xinfer_parent_class parent_class

/** @brief  Glib's convenience macro for GstVvas_XInfer type implementation.
 *  @details This macro does below tasks:\n
 *		- Declares a class initialization function with prefix gst_vvas_xinfer
 *		- Declares an instance initialization function
 *		- A static variable named gst_vvas_xinfer_parent_class pointing to the parent class
 *		- Defines a gst_vvas_xinfer_get_type() function with below tasks
 *			- Initializes GTypeInfo function pointers
 *			- Registers created GTypeInfo with GType system as declaring parent type as
 *			  GST_TYPE_BASE_TRANSFORM
 *			- Registers GstVvas_XInferPrivate as private structure to GstVvas_XInfer type
 */
G_DEFINE_TYPE_WITH_PRIVATE (GstVvas_XInfer, gst_vvas_xinfer,
    GST_TYPE_BASE_TRANSFORM);

#define GST_VVAS_XINFER_PRIVATE(self) (GstVvas_XInferPrivate *) (gst_vvas_xinfer_get_instance_private (self))

static gboolean
vvas_xinfer_prepare_ppe_output_frame (GstVvas_XInfer * self, GstBuffer * outbuf,
    GstVideoInfo * out_vinfo, VvasVideoFrame ** vvas_frame);
static void gst_vvas_xinfer_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_vvas_xinfer_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_vvas_xinfer_finalize (GObject * obj);

static inline const gchar *
vvas_format_to_caps_str (VvasVideoFormat fmt)
{
  return gst_video_format_to_string (gst_coreutils_get_gst_fmt_from_vvas (fmt));
}

/**
 * @fn static gboolean is_class_allowed (GstVvas_XInferPrivate * priv, const gchar * class)
 * @param [in] priv - Handle to Infer Private Members
 * @param [in] class - pointer to the class string
 * @return TRUE if class string entry is found in input_class_filters list.
 *         FALSE if class string is not found in input_class_filters list.
 *
 * @brief This function will check if class string entry is of present in input_class_filters.
 */
static gboolean
is_class_allowed (GstVvas_XInferPrivate *priv, const gchar *class_str)
{
  if (!priv->infer->input_class_filters)
    return TRUE;

  if (g_list_find_custom (priv->infer->input_class_filters,
          (gconstpointer) class_str, (GCompareFunc) g_strcmp0)) {
    return TRUE;
  }

  return FALSE;
}

/**
 * @fn static gboolean check_filter_label_at_node (GNode * node, gpointer data)
 * @param [in] node - node in a tree
 * @param [in] data - pointer to Vvas_XInferNumSubs
 * @return TRUE if class is allowed.
 *         FALSE if class is not filter out.
 *
 * @brief This function will check if node is of allowed class or not.
 */
static gboolean
check_filter_label_at_node (GNode *node, gpointer data)
{
  gboolean ret = FALSE;
  GstVvas_XInfer *self = (GstVvas_XInfer *) data;
  GstVvas_XInferPrivate *priv = self->priv;
  GstInferencePrediction *prediction = (GstInferencePrediction *) node->data;
  VvasInferClassification *classification;
  GList *classes = NULL;

  if (prediction->prediction.infer_result->infer_result_type ==
      VVAS_INFER_RESULT_CLASSIFICATION) {
    classes = (GList *) prediction->prediction.infer_result->data;
    for (; classes; classes = g_list_next (classes)) {
      classification = (VvasInferClassification *) classes->data;
      if (is_class_allowed (priv, (gchar *) classification->label)) {
        ret = TRUE;
        break;
      }
    }
  } else if (prediction->prediction.infer_result->infer_result_type ==
      VVAS_INFER_RESULT_DETECTION) {
    VvasInferDetection *d =
        (VvasInferDetection *) prediction->prediction.infer_result->data;
    if (is_class_allowed (priv, (gchar *) d->label)) {
      ret = TRUE;
    }
  }

  return ret;
}

/**
 * @fn static gboolean check_bbox_buffers_availability (GNode * node, gpointer data)
 * @param [in] node - node in a tree
 * @param [in] data - pointer to Vvas_XInferNumSubs
 * @return TRUE or FALSE
 *         The traversal can be halted by returning TRUE when suitable
 *         sub-buffer found
 *
 * @brief This function return either the sub-buffer can be used for
 *        current level of prediction
 */
static gboolean
check_bbox_buffers_availability (GNode *node, gpointer data)
{
  Vvas_XInferNumSubs *pNumSubs = (Vvas_XInferNumSubs *) data;
  GstVvas_XInfer *self = pNumSubs->self;
  GstInferencePrediction *prediction = NULL;
  VvasInferDetection *detection = NULL;

  /* ignore node which is not at same level of current inference */
  if (g_node_depth ((GNode *) node) != self->priv->infer->level) {
    GST_LOG_OBJECT (self, "ignoring node %p at level %d", node,
        g_node_depth ((GNode *) node));
    return FALSE;
  }

  if (self->priv->infer->num_input_class_filters
      && !check_filter_label_at_node (node, (gpointer) self)) {
    GST_DEBUG_OBJECT (self,
        "Skipping inference on this node as it's class is filtered out");
    return FALSE;
  }

  prediction = (GstInferencePrediction *) node->data;
  /* With current existing use cases, when running multiple level of inference,
   * the first level is always detection.
   */
  if (prediction->prediction.infer_result->infer_result_type ==
      VVAS_INFER_RESULT_DETECTION) {
    detection =
        (VvasInferDetection *) prediction->prediction.infer_result->data;
  } else {
    GST_ERROR_OBJECT (self,
        "Non-detection model at previous level, un-expected flow error");
    return FALSE;
  }

  /* filtering input objects if width and height are not in the range
   * with configured values  */
  if ((detection->bbox.width < self->priv->infer->input_obj_min_width) ||
      (detection->bbox.height < self->priv->infer->input_obj_min_height) ||
      (detection->bbox.width > self->priv->infer->input_obj_max_width) ||
      (detection->bbox.height > self->priv->infer->input_obj_max_height)) {
    GST_DEBUG_OBJECT (self,
        "Width/Height are not within the configured range ");
    return FALSE;
  }

  pNumSubs->required_buffer++;
  if (prediction->sub_buffer) {
    GstVideoMeta *vmeta;

    if (!prediction->prediction.enabled) {
      GST_DEBUG_OBJECT (self,
          "Skipping inference on this node as it is disabled");
      pNumSubs->required_buffer--;
      return FALSE;
    }
    GST_LOG_OBJECT (self, "bbox buffer availble for node %p", node);
    vmeta = gst_buffer_get_video_meta (prediction->sub_buffer);
    if (vmeta) {
      GST_LOG_OBJECT (self, "bbox width = %d, height = %d and format = %s",
          vmeta->width, vmeta->height,
          gst_video_format_to_string (vmeta->format));
      GST_LOG_OBJECT (self,
          "infer preffed width = %d, height = %d and format = %s",
          self->priv->infer->pref_width, self->priv->infer->pref_height,
          gst_video_format_to_string (self->priv->infer->pref_format));
    }
    /* Sub-buffer can be useded for current level of inference only if
     * its height, width and format matched with preferred height, width and
     * format resp of current level inferance */
    if (vmeta && (vmeta->width == self->priv->infer->pref_width) &&
        vmeta->height == self->priv->infer->pref_height &&
        vmeta->format == self->priv->infer->pref_format) {
      pNumSubs->available_buffer++;

    }
  }
  return FALSE;
}

/**
 * @fn static gboolean prepare_inference_sub_buffers (GNode * node, gpointer data)
 * @param [in] node - node in a tree
 * @param [in] data - pointer to GstVvas_XInfer handle
 * @return TRUE when the traversal can be halted
 *         FALSE when the traversal can not be halted
 *
 * @brief This function finds either the sub-buffer can be used for
 *        current level of prediction and add them to current infer->sub_buffers
 *        queue
 */
static gboolean
prepare_inference_sub_buffers (GNode *node, gpointer data)
{
  /* Previous infer has meta i.e found something */
  Vvas_XInferNumSubs *numSubs = (Vvas_XInferNumSubs *) data;
  GstVvas_XInfer *self = GST_VVAS_XINFER (numSubs->self);
  GstInferencePrediction *prediction = NULL;
  GstVvas_XInferPrivate *priv = self->priv;
  VvasInferDetection *detection = NULL;

  /* ignore node which is not at same level of current inference */
  if (g_node_depth (node) != self->priv->infer->level) {
    GST_LOG_OBJECT (self, "ignoring node %p at level %d", node,
        g_node_depth (node));
    return FALSE;
  }

  if (priv->infer->num_input_class_filters
      && !check_filter_label_at_node (node, (gpointer) self)) {
    GST_DEBUG_OBJECT (self,
        "Skipping inference on this node as it's class is filtered out");
    return FALSE;
  }

  if (g_node_child_position (node->parent, node) >= MAX_ROI) {
    GST_DEBUG_OBJECT (self, "Sub buffers reached to max ROI "
        "supported by preprocessor i.e. %d", MAX_ROI);
    return TRUE;
  }
  prediction = (GstInferencePrediction *) node->data;
  /* With current existing use cases, when running multiple level of inference,
   * the first level is always detection.
   */
  if (prediction->prediction.infer_result->infer_result_type ==
      VVAS_INFER_RESULT_DETECTION) {
    detection =
        (VvasInferDetection *) prediction->prediction.infer_result->data;
  } else {
    GST_ERROR_OBJECT (self,
        "Non-detection model at previous level, un-expected flow error");
    return FALSE;
  }

  /* filtering input objects if width and height are not in the range
   * with configured values  */
  if ((detection->bbox.width < priv->infer->input_obj_min_width) ||
      (detection->bbox.height < priv->infer->input_obj_min_height) ||
      (detection->bbox.width > priv->infer->input_obj_max_width) ||
      (detection->bbox.height > priv->infer->input_obj_max_height)) {
    GST_DEBUG_OBJECT (self, "width/height are not within the configured range");
    return FALSE;
  }

  if (!prediction->sub_buffer) {
    GST_LOG_OBJECT (self, "bbox buffer not availble for node %p", node);
    numSubs->required_buffer++;
    priv->pre_proc->frame->is_ppe_required[g_node_child_position (node->parent,
            node)] = TRUE;
  } else {
    GstVideoMeta *vmeta;

    if (!prediction->prediction.enabled) {
      GST_DEBUG_OBJECT (self,
          "Skipping inference on this node as it is disabled");
      return FALSE;
    }
    vmeta = gst_buffer_get_video_meta (prediction->sub_buffer);
    if (vmeta) {
      GST_LOG_OBJECT (self, "bbox width = %d, height = %d and format = %s",
          vmeta->width, vmeta->height,
          gst_video_format_to_string (vmeta->format));
      GST_LOG_OBJECT (self,
          "infer preffed width = %d, height = %d and format = %s",
          priv->infer->pref_width, priv->infer->pref_height,
          gst_video_format_to_string (priv->infer->pref_format));
    }
    /* Add sub-buffer to current infer->sub_buffers queue only if
     * its height, width and format are matching with preferred
     * height, width and format resp of current inference */
    if (vmeta && (vmeta->width == priv->infer->pref_width) &&
        vmeta->height == priv->infer->pref_height &&
        vmeta->format == priv->infer->pref_format) {
      GstInferenceMeta *sub_meta;
      GstInferencePrediction *parent_prediction =
          (GstInferencePrediction *) node->parent->data;

      GST_DEBUG_OBJECT (self, "queueing subbuffer %p", prediction->sub_buffer);
      g_queue_push_tail (self->priv->infer->sub_buffers,
          prediction->sub_buffer);

      sub_meta =
          ((GstInferenceMeta *) gst_buffer_get_meta (prediction->sub_buffer,
              gst_inference_meta_api_get_type ()));
      if (!sub_meta) {
        GST_LOG_OBJECT (self, "add inference metadata to %p",
            prediction->sub_buffer);
        sub_meta =
            (GstInferenceMeta *) gst_buffer_add_meta (prediction->sub_buffer,
            gst_inference_meta_get_info (), NULL);
      }

      gst_inference_prediction_unref (sub_meta->prediction);
      /* increase the ref count as sub-buffer is used by current inference */
      gst_inference_prediction_ref (parent_prediction);
      sub_meta->prediction = prediction;
      numSubs->available_buffer++;
      priv->pre_proc->frame->
          is_ppe_required[g_node_child_position (node->parent, node)] = FALSE;
    } else {
      numSubs->required_buffer++;
      priv->pre_proc->frame->
          is_ppe_required[g_node_child_position (node->parent, node)] = TRUE;
    }
  }

  return FALSE;
}

#ifdef PRINT_METADATA_TREE
/**
 * @fn static gboolean printf_all_nodes (GNode * node, gpointer data)
 * @param [in] node - node in a tree
 * @param [in] data - pointer to GstVvas_XInfer handle
 * @return TRUE when the traversal can be halted
 *         FALSE when the traversal can not be halted
 *
 * @brief This function prints node
 */
static gboolean
printf_all_nodes (GNode *node, gpointer data)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (data);

  GST_LOG_OBJECT (self, "node = %p at level %d", node, g_node_depth (node));
  return FALSE;
}
#endif

/**
 * @fn static gboolean prepare_ppe_outbuf_at_level (GNode * node, gpointer data)
 * @param [in] node - node in a tree
 * @param [in] data - pointer to GstVvas_XInfer handle
 * @return TRUE when the traversal can be halted on error
 *         FALSE when the traversal can not be halted
 *
 * @brief This function create ppe output frame and populate its
 *        member with required information. The function execute
 *        for each node which is at depth equal to current
 *        inference level
 */
static gboolean
prepare_ppe_outbuf_at_level (GNode *node, gpointer data)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (data);
  GstVvas_XInferPrivate *priv = self->priv;
  GstVvasUsrMeta *gst_usrmeta = NULL;
  GstMetaInfo *info = NULL;

  if (priv->pre_proc->nframes_in_level >= MAX_ROI) {
    GST_DEBUG_OBJECT (self,
        "Max number of ROI's processed by preprocessor is %d. So not creating the output frame",
        MAX_ROI);
    return TRUE;
  }

  GST_LOG_OBJECT (self, "node = %p at level %d", node, g_node_depth (node));

  /* Process node only if current inference level and node depth are same */
  if (g_node_depth (node) == priv->infer->level) {
    GstBuffer *outbuf;
    GstFlowReturn fret;
    VvasVideoFrame *out_vvas_frame;
    GstInferencePrediction *prediction = (GstInferencePrediction *) node->data;
    VvasInferDetection *detection = NULL;
    GstInferencePrediction *parent_prediction =
        (GstInferencePrediction *) node->parent->data;
    GstInferenceMeta *infer_meta;
    gboolean bret;

    /* With current existing use cases, when running multiple level of inference,
     * the first level is always detection.
     */
    if (prediction->prediction.infer_result->infer_result_type ==
        VVAS_INFER_RESULT_DETECTION) {
      detection =
          (VvasInferDetection *) prediction->prediction.infer_result->data;
    } else {
      GST_ERROR_OBJECT (self,
          "Non-detection model at previous level, un-expected flow error");
      return FALSE;
    }

    if (priv->infer->num_input_class_filters
        && !check_filter_label_at_node (node, (gpointer) self)) {
      GST_DEBUG_OBJECT (self,
          "Skipping inference on this node as it's class is filtered out");
      return FALSE;
    }

    if (!priv->pre_proc->frame->
        is_ppe_required[g_node_child_position (node->parent, node)]) {
      GST_DEBUG_OBJECT (self,
          "Skipping inference on this node as scalinfg is not required");
      return FALSE;
    }
    if ((detection->bbox.width < priv->infer->input_obj_min_width) ||
        (detection->bbox.height < priv->infer->input_obj_min_height) ||
        (detection->bbox.width > priv->infer->input_obj_max_width) ||
        (detection->bbox.height > priv->infer->input_obj_max_height)) {
      GST_DEBUG_OBJECT (self,
          "Width/Height of the ROI is not within the configured range,so discarding");
      return FALSE;
    }
    if ((detection->bbox.width < priv->pre_proc->caps->min_width)
        || (detection->bbox.height < priv->pre_proc->caps->min_height)) {
      GST_DEBUG_OBJECT (self,
          "Width/Height of the ROI is less the minimum supported(%dx%d), discarding",
          priv->pre_proc->caps->min_width, priv->pre_proc->caps->min_height);
      return FALSE;
    }

    if (!prediction->prediction.enabled) {
      GST_DEBUG_OBJECT (self,
          "Skipping inference on this node as it is disabled");
      return FALSE;
    }

    GST_LOG_OBJECT (self, "found node %p at level inference level %d", node,
        priv->infer->level);

    /* acquire ppe output buffer */
    fret =
        gst_buffer_pool_acquire_buffer (priv->pre_proc->outpool, &outbuf, NULL);
    if (fret != GST_FLOW_OK) {
      GST_ERROR_OBJECT (self, "failed to allocate buffer from pool %p",
          priv->pre_proc->outpool);
      priv->is_error = TRUE;
      return TRUE;
    }

    /* copy GstVvasUsrMeta if any */
    gst_usrmeta =
        gst_buffer_get_vvas_usr_meta ((GstBuffer *) priv->pre_proc->
        frame->parent_buf);
    if (gst_usrmeta) {
      info = (GstMetaInfo *) ((GstMeta *) gst_usrmeta)->info;
      if (info && info->transform_func) {
        info->transform_func (outbuf, (GstMeta *) gst_usrmeta,
            priv->pre_proc->frame->parent_buf, _gst_meta_transform_copy, NULL);
        GST_LOG_OBJECT (self, "copy GstVvasUsrMeta %p", gst_usrmeta);
      }
    }

    /* Prepare VvasVideoFrame from GstBuffer required by core for pre-processing */
    bret =
        vvas_xinfer_prepare_ppe_output_frame (self, outbuf,
        priv->pre_proc->out_vinfo, &out_vvas_frame);
    if (!bret) {
      priv->is_error = TRUE;
      vvas_video_frame_free (out_vvas_frame);
      gst_buffer_unref (outbuf);
      return TRUE;
    }

    infer_meta =
        ((GstInferenceMeta *) gst_buffer_get_meta (outbuf,
            gst_inference_meta_api_get_type ()));
    if (!infer_meta) {
      GST_LOG_OBJECT (self, "add inference metadata to %p", outbuf);
      infer_meta =
          (GstInferenceMeta *) gst_buffer_add_meta (outbuf,
          gst_inference_meta_get_info (), NULL);
    }

    gst_inference_prediction_unref (infer_meta->prediction);
    /* Increase te ref count as it is required by ppe */
    gst_inference_prediction_ref (parent_prediction);
    infer_meta->prediction = prediction;

    if (priv->infer->attach_ppebuf) {
      if (prediction->sub_buffer) {
        GST_DEBUG_OBJECT (self,
            "removing existing sub_buffer %p", prediction->sub_buffer);
        gst_buffer_unref (prediction->sub_buffer);
      }
      prediction->sub_buffer = gst_buffer_ref (outbuf);
      GST_DEBUG_OBJECT (self,
          "acquired PPE output buffer %p and attached as sub_buffer", outbuf);
    }

    /* Add out_vvas_frame to array of ppe kernel output frame.
     * The ppe kenel would fill buffer in this frame with output data */
    priv->pre_proc->core_handle->output[priv->pre_proc->nframes_in_level] =
        out_vvas_frame;
    g_queue_push_tail (priv->pre_proc->buf_queue, outbuf);
    priv->pre_proc->nframes_in_level++;
  }

  return FALSE;
}

/**
 * @fn static gboolean vvas_xinfer_is_sub_buffer_useful (GstVvas_XInfer * self, GstBuffer * buf)
 * @param [in] self - handle to GstVvas_XInfer
 * @param [in] buf - input GstBuffer
 * @return TRUE when buffer parameters matched with inference requirement
 *         FALSE when buffer parameters not matched with inference requirement
 *
 * @brief This function compare the current inference parameters with input buf
 *        parameters
 */
static gboolean
vvas_xinfer_is_sub_buffer_useful (GstVvas_XInfer *self, GstBuffer *buf)
{
  GstVideoMeta *vmeta = NULL;

  if (!buf)
    return FALSE;

  vmeta = gst_buffer_get_video_meta (buf);
  if (vmeta && (vmeta->width == self->priv->infer->pref_width) &&
      vmeta->height == self->priv->infer->pref_height &&
      vmeta->format == self->priv->infer->pref_format) {
    GST_LOG_OBJECT (self,
        "buffer parameters matched with inference requirement");
    return TRUE;
  }
  return FALSE;
}

/**
 * @fn static gboolean vvas_xinfer_allocate_sink_internal_pool (GstVvas_XInfer * self)
 * @param [in] self - handle to GstVvas_XInfer
 * @return TRUE when pool created
 *         FALSE when Pool not created
 *
 * @brief This function creates the internal input pool of physical continuous
 *        buffer for xinfer. The internal input pool is created only
 *        when input to xinfer is
 *           - not vvas buffer
 *           - not dma buffer
 *           - not on same device
 *           - is not on same memory bank of same device
 *
 *        Pool parameters (device, xclbin, memory bank, stride alignment) are
 *        passed in so the same helper can serve both the PPE path and the
 *        infer-only HW-tensor path.
 */
static gboolean
vvas_xinfer_allocate_sink_internal_pool (GstVvas_XInfer *self,
    gint dev_idx, gchar *xclbin_loc, gint mem_bank, guint stride_align)
{
  GstVideoInfo info;
  GstBufferPool *pool = NULL;
  GstStructure *config;
  GstAllocator *allocator = NULL;
  GstAllocationParams alloc_params;
  GstCaps *caps = NULL;
  GstVideoAlignment align;

  caps = gst_pad_get_current_caps (GST_BASE_TRANSFORM (self)->sinkpad);

  /* get the video parameters of sink pad */
  if (!gst_video_info_from_caps (&info, caps)) {
    GST_WARNING_OBJECT (self, "Failed to parse caps %" GST_PTR_FORMAT, caps);
    gst_caps_unref (caps);
    return FALSE;
  }
  pool = gst_vvas_buffer_pool_new (stride_align ? stride_align : 1, 1);
  GST_LOG_OBJECT (self,
      "allocated internal sink pool %p (dev=%d bank=%d stride_align=%u)", pool,
      dev_idx, mem_bank, stride_align);

  /* Create new allocator */
  allocator =
      gst_vvas_allocator_new (dev_idx, xclbin_loc, USE_DMABUF, mem_bank);

  /* kernel need physically contiguous memory */
  gst_allocation_params_init (&alloc_params);
  alloc_params.flags = GST_MEMORY_FLAG_PHYSICALLY_CONTIGUOUS;

  config = gst_buffer_pool_get_config (pool);
  /* No max limit for allocated buffer */
  gst_buffer_pool_config_set_params (config, caps, GST_VIDEO_INFO_SIZE (&info),
      3, 0);
  gst_buffer_pool_config_set_allocator (config, allocator, &alloc_params);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);

  /* reset the video alignment before configuring */
  gst_video_alignment_reset (&align);

  /* Let's set our alignment info into the pool config */
  if (stride_align > 1) {
    for (guint idx = 0; idx < GST_VIDEO_INFO_N_PLANES (&info); idx++) {
      align.stride_align[idx] = (stride_align - 1);
    }
  }

  gst_buffer_pool_config_add_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
  gst_buffer_pool_config_set_video_alignment (config, &align);

  if (!gst_buffer_pool_set_config (pool, config)) {
    GST_ERROR_OBJECT (self, "Failed to set config on input pool");
    goto error;
  }

  if (self->priv->input_pool)
    gst_object_unref (self->priv->input_pool);

  /* Store for further reference */
  self->priv->input_pool = pool;

  GST_INFO_OBJECT (self, "allocated %" GST_PTR_FORMAT " pool", pool);
  gst_caps_unref (caps);
  /* reduce the ref count as its control taken by pool */
  if (allocator)
    gst_object_unref (allocator);

  return TRUE;

error:
  gst_caps_unref (caps);
  if (pool)
    gst_object_unref (pool);
  if (allocator)
    gst_object_unref (allocator);
  return FALSE;
}

/**
 * @fn static gboolean vvas_xinfer_copy_input_buffer (GstVvas_XInfer * self, GstBuffer * inbuf,
 *						      GstBuffer ** internal_inbuf)
 * @param [in] self - handle to GstVvas_XInfer
 * @param [in] inbuf - input buffer on sink pad of xinfer
 * @param [out] internal_inbuf - new buffer based on kernel requirement
 * @return TRUE on success
 *         FALSE on failure
 *
 * @brief This function copy the input buffer to internal input buffer of
 *        required video parameter of kernel
 *        Internal input pool of buffer is also created if it is not already
 *        available
 */
static gboolean
vvas_xinfer_copy_input_buffer (GstVvas_XInfer *self, GstBuffer *inbuf,
    GstBuffer **internal_inbuf, gint dev_idx, gchar *xclbin_loc,
    gint mem_bank, guint stride_align)
{
  GstBuffer *new_inbuf = NULL;
  GstFlowReturn fret;
  GstVideoFrame in_vframe, new_vframe;
  gboolean bret;

  memset (&in_vframe, 0x0, sizeof (GstVideoFrame));
  memset (&new_vframe, 0x0, sizeof (GstVideoFrame));

  if (!self->priv->input_pool) {
    /* allocate input internal pool */
    bret = vvas_xinfer_allocate_sink_internal_pool (self, dev_idx,
        xclbin_loc, mem_bank, stride_align);
    if (!bret)
      goto error;

    if (!gst_buffer_pool_is_active (self->priv->input_pool)) {
      gst_buffer_pool_set_active (self->priv->input_pool, TRUE);
    }
  }

  /* acquire buffer from own input pool */
  fret =
      gst_buffer_pool_acquire_buffer (self->priv->input_pool, &new_inbuf, NULL);
  if (fret != GST_FLOW_OK) {
    GST_ERROR_OBJECT (self, "failed to allocate buffer from pool %p",
        self->priv->input_pool);
    goto error;
  }
  GST_LOG_OBJECT (self, "acquired buffer %p from own pool", new_inbuf);

  /* map internal buffer in write mode */
  if (!gst_video_frame_map (&new_vframe, self->priv->in_vinfo, new_inbuf,
          GST_MAP_WRITE)) {
    GST_ERROR_OBJECT (self, "failed to map internal input buffer");
    goto error;
  }

  /* map input buffer in read mode */
  if (!gst_video_frame_map (&in_vframe, self->priv->in_vinfo, inbuf,
          GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "failed to map input buffer");
    goto error;
  }
  GST_CAT_LOG_OBJECT (GST_CAT_PERFORMANCE, self,
      "slow copy to internal input pool buffer");

  /* frame_copy will take care of stride too */
  gst_video_frame_copy (&new_vframe, &in_vframe);
  gst_video_frame_unmap (&in_vframe);
  gst_video_frame_unmap (&new_vframe);
  gst_buffer_copy_into (new_inbuf, inbuf,
      (GstBufferCopyFlags) GST_BUFFER_COPY_METADATA, 0, -1);
  *internal_inbuf = new_inbuf;

  return TRUE;

error:
  if (in_vframe.data[0]) {
    gst_video_frame_unmap (&in_vframe);
  }
  if (new_vframe.data[0]) {
    gst_video_frame_unmap (&new_vframe);
  }
  if (new_inbuf) {
    gst_buffer_unref (new_inbuf);
    new_inbuf = NULL;
  }
  return FALSE;
}

/**
 * @fn static gboolean vvas_xinfer_ensure_xrt_input_buffer (GstVvas_XInfer * self,
 *                                                         GstBuffer * inbuf,
 *                                                         gint dev_idx,
 *                                                         const gchar * xclbin_loc,
 *                                                         gint mem_bank,
 *                                                         guint stride_align,
 *                                                         GstBuffer ** new_inbuf)
 *
 * @brief Ensure the input GstBuffer is backed by an XRT BO. If upstream
 *        already delivered a VVAS or dmabuf memory, no copy is performed and
 *        @p *new_inbuf is set to NULL. Otherwise the data is copied into a
 *        buffer acquired from the lazily-created internal XRT pool, and the
 *        new buffer is returned via @p *new_inbuf (caller owns the ref).
 *
 *        Performance-instrumented: the copy path emits a GST_INFO log with
 *        wall-clock timing so the BO-materialization overhead is visible
 *        when this fallback triggers (test pipelines using filesrc /
 *        rawvideoparse will hit it). The fast path stays silent.
 *
 * @return TRUE on success, FALSE on error.
 */
static gboolean
vvas_xinfer_ensure_xrt_input_buffer (GstVvas_XInfer *self, GstBuffer *inbuf,
    gint dev_idx, gchar *xclbin_loc, gint mem_bank, guint stride_align,
    GstBuffer **new_inbuf)
{
  GstMemory *in_mem;
  gboolean need_copy;
  gboolean bret;
  GstClockTime t_start, t_pool_done, t_copy_done, t_sync_done;
  GstMemory *new_mem;

  *new_inbuf = NULL;

  in_mem = gst_buffer_get_memory (inbuf, 0);
  if (!in_mem) {
    GST_ERROR_OBJECT (self, "failed to get memory from input buffer");
    return FALSE;
  }

  /* Fast path: upstream already gave us XRT-friendly memory */
  need_copy = !gst_is_vvas_memory (in_mem) && !gst_is_dmabuf_memory (in_mem);
  gst_memory_unref (in_mem);
  if (!need_copy)
    return TRUE;

  /* Slow path: SW upstream and HW tensor required. Copy into our pool. */
  t_start = gst_util_get_timestamp ();

  if (!self->priv->input_pool) {
    bret = vvas_xinfer_allocate_sink_internal_pool (self, dev_idx,
        xclbin_loc, mem_bank, stride_align);
    if (!bret)
      return FALSE;

    if (!gst_buffer_pool_is_active (self->priv->input_pool)) {
      gst_buffer_pool_set_active (self->priv->input_pool, TRUE);
    }
  }
  t_pool_done = gst_util_get_timestamp ();

  bret = vvas_xinfer_copy_input_buffer (self, inbuf, new_inbuf,
      dev_idx, xclbin_loc, mem_bank, stride_align);
  if (!bret)
    return FALSE;
  t_copy_done = gst_util_get_timestamp ();

  /* Flush CPU cache so the device sees fresh data. */
  new_mem = gst_buffer_get_memory (*new_inbuf, 0);
  if (new_mem) {
    if (gst_is_vvas_memory (new_mem)) {
      gst_vvas_memory_set_flag (new_mem, VVAS_SYNC_TO_DEVICE);
      if (!gst_vvas_memory_sync_bo (new_mem)) {
        GST_ERROR_OBJECT (self, "sync_bo failed for internal HW input buffer");
        gst_memory_unref (new_mem);
        gst_buffer_unref (*new_inbuf);
        *new_inbuf = NULL;
        return FALSE;
      }
    }
    gst_memory_unref (new_mem);
  }
  t_sync_done = gst_util_get_timestamp ();

  GST_INFO_OBJECT (self,
      "SW->XRT_BO materialization (perf): pool_setup=%" G_GUINT64_FORMAT
      "us copy=%" G_GUINT64_FORMAT "us sync=%" G_GUINT64_FORMAT
      "us total=%" G_GUINT64_FORMAT "us",
      (t_pool_done - t_start) / GST_USECOND,
      (t_copy_done - t_pool_done) / GST_USECOND,
      (t_sync_done - t_copy_done) / GST_USECOND,
      (t_sync_done - t_start) / GST_USECOND);

  return TRUE;
}

/**
 * @fn static gboolean vvas_xinfer_ppe_init (GstVvas_XInfer * self)
 * @param [in] self - Handle to GstVvas_XInfer
 * @return TRUE on success
 *         FALSE on failure
 *
 * @brief Initialize the PPE private parameters
 * @detail This function open xrt context and call library init func and
 *	  populate vvas_handle for ppe image process library.
 *        The function also download xclbin if it is not already downloaded.
 *        Pre-process library init should be called after infer kernel init
 *        as pre-process parameters depends on infer parameters
 *
 */
static gboolean
vvas_xinfer_ppe_init (GstVvas_XInfer *self)
{
  GstVvas_XInferPrivate *priv = self->priv;
  VvasImageProcess *ppe_handle = NULL;
  VvasImageProcessCapabilities *lib_caps = NULL;
  VvasReturnType vret;
  bool flag = FALSE;

  GST_DEBUG_OBJECT (self, "Query Image Process Library caps");
  lib_caps =
      vvas_image_process_get_capabilities (priv->pre_proc->core_handle->name);
  if (!lib_caps) {
    GST_ERROR_OBJECT (self,
        "Failed to query Image Process Library caps for library: %s."
        " Check library-name in preprocess-config",
        priv->pre_proc->core_handle->name);
    return FALSE;
  }
  priv->pre_proc->caps = lib_caps;

  if (priv->pre_proc->is_interpolation_mode_set) {
    for (int i = 0; i < lib_caps->num_interpolation_modes; i++) {
      if (priv->pre_proc->user_interpolation_mode ==
          lib_caps->supported_interpolation_modes[i]) {
        priv->pre_proc->init_config.interpolation_mode =
            priv->pre_proc->user_interpolation_mode;
        flag = TRUE;
        break;
      }
    }
    if (!flag) {
      GST_ERROR_OBJECT (self, "Interpolation mode %d is not supported by"
          " image process library: %s", priv->pre_proc->user_interpolation_mode,
          priv->pre_proc->core_handle->name);
      return FALSE;
    }
  } else {
    priv->pre_proc->init_config.interpolation_mode =
        lib_caps->supported_interpolation_modes[0];
    GST_DEBUG_OBJECT (self,
        "Interpolation mode is not set. Configuring first supported mode: %d",
        priv->pre_proc->init_config.interpolation_mode);
  }

  if (!priv->pre_proc->use_software) {
    GST_DEBUG_OBJECT (self, "Creating vvas_context");
    priv->pre_proc->vvas_ctx =
        vvas_context_create (priv->pre_proc->dev_idx,
        priv->pre_proc->xclbin_loc, priv->core_log_level, &vret);
  } else {
    /* For Software Scaling, no need of device index and XCLBIN */
    priv->pre_proc->vvas_ctx =
        vvas_context_create (-1, NULL, priv->core_log_level, &vret);
  }
  if (!priv->pre_proc->vvas_ctx) {
    if (!priv->pre_proc->use_software &&
        !g_file_test (priv->pre_proc->xclbin_loc, G_FILE_TEST_EXISTS)) {
      GST_ERROR_OBJECT (self,
          "Couldn't create VVAS context: xclbin %s not found",
          priv->pre_proc->xclbin_loc);
    } else {
      GST_ERROR_OBJECT (self, "Couldn't create VVAS context");
    }
    return FALSE;
  }
  priv->pre_proc->dev_handle = priv->pre_proc->vvas_ctx->dev_handle;

  GST_DEBUG_OBJECT (self, "Creating vvas image process instance");

  guint64 t0 = 0;
  if (priv->infer_profiler.enabled) {
    t0 = vvas_profiler_now_us ();
  }
  ppe_handle =
      vvas_image_process_create (priv->pre_proc->vvas_ctx,
      (const char *) priv->pre_proc->core_handle->name, priv->core_log_level,
      &priv->pre_proc->init_config, &priv->pre_proc->out_param);
  if (!ppe_handle) {
    GST_ERROR_OBJECT (self,
        "Couldn't create Image Process instance for library: %s."
        " Possible invalid or missing library-config in preprocess-config",
        priv->pre_proc->core_handle->name);
    return FALSE;
  }
  if (priv->infer_profiler.enabled) {
    g_mutex_lock (&priv->infer_profiler.snap_lock);
    priv->infer_profiler.pre_proc.enabled = TRUE;
    priv->infer_profiler.pre_proc.init_us = vvas_profiler_now_us () - t0;
    g_mutex_unlock (&priv->infer_profiler.snap_lock);
    GST_INFO_OBJECT (self, "Pre-process initialization time: %lu us ",
        priv->infer_profiler.pre_proc.init_us);
  }
  priv->pre_proc->core_handle->handle = ppe_handle;

  if (priv->pre_proc->in_mem_bank >= 0) {
    /* assign user provided memory bank for image process library */
    priv->pre_proc->out_param.in_mem_bank = priv->pre_proc->in_mem_bank;
  } else {
    /* get input memory bank from image process library */
    priv->pre_proc->in_mem_bank = priv->pre_proc->out_param.in_mem_bank;
    GST_INFO_OBJECT (self, "input memory bank received from library = %d",
        priv->pre_proc->in_mem_bank);
  }

  if (priv->pre_proc->out_mem_bank >= 0) {
    /* assign user provided memory bank for image process library */
    priv->pre_proc->out_param.out_mem_bank = priv->pre_proc->out_mem_bank;
  } else {
    /* get output memory bank from image process library */
    priv->pre_proc->out_mem_bank = priv->pre_proc->out_param.out_mem_bank;
    GST_INFO_OBJECT (self, "output memory bank received from library = %d",
        priv->pre_proc->out_mem_bank);
  }

  GST_DEBUG_OBJECT (self,
      "Image Process Library alignment requirements: x[%u], width[%u] stride[%u]",
      priv->pre_proc->caps->alignment_req.x,
      priv->pre_proc->caps->alignment_req.width,
      priv->pre_proc->caps->alignment_req.stride);

  GST_INFO_OBJECT (self,
      "PPE input memory bank idx = %d and output memory bank idx = %d",
      priv->pre_proc->in_mem_bank, priv->pre_proc->out_mem_bank);

  GST_INFO_OBJECT (self, "completed preprocess init");

  return TRUE;
}

/**
 * @fn static gboolean vvas_xinfer_postproc_init (GstVvas_XInfer * self)
 * @param [in] self - handle to GstVvas_XInfer
 * @return TRUE on success
 *         FALSE on failure
 *
 * @brief Initialize the infer private parameters
 * @detail This function loads the postprocess library and populate the
 *         function address of symbols
 *
 */
static gboolean
vvas_xinfer_postproc_init (GstVvas_XInfer *self)
{
  GstVvas_XInferPrivate *priv = self->priv;
  VvasTensorInfo *t_info[MAX_TENSORS] = { };
  VvasReturnType vret = VVAS_RET_ERROR;

  int dev_idx = -1;
  if (priv->infer->vart_info.out_tensor_type == vart::TensorType::HW) {
    dev_idx = XDNA_DEVICE_IDX;
  }

  guint64 t0 = 0;
  if (priv->infer_profiler.enabled)
    t0 = vvas_profiler_now_us ();
  priv->post_proc->vvas_ctx =
      vvas_context_create (dev_idx, NULL, priv->core_log_level, &vret);
  if (!priv->post_proc->vvas_ctx) {
    GST_ERROR_OBJECT (self, "Couldn't create VVAS context");
    return FALSE;
  }

  if (priv->infer_profiler.enabled) {
    g_mutex_lock (&priv->infer_profiler.snap_lock);
    priv->infer_profiler.post_proc.enabled = TRUE;
    priv->infer_profiler.post_proc.init_us = vvas_profiler_now_us () - t0;
    g_mutex_unlock (&priv->infer_profiler.snap_lock);
    GST_INFO_OBJECT (self, "Post-process initialization time: %lu us",
        priv->infer_profiler.post_proc.init_us);
  }

  size_t num_tensors = 0;
  if (strstr (priv->post_proc->library_path,
          "libvvascore_postprocess_vart") != NULL) {
    num_tensors = priv->infer->model_config.num_in_tensors;
    for (size_t i = 0; i < num_tensors; i++) {
      t_info[i] = &priv->infer->model_config.in_tensors[i];
    }
  }

  for (size_t j = 0; j < priv->infer->model_config.num_out_tensors; j++) {
    t_info[num_tensors + j] = &priv->infer->model_config.out_tensors[j];
  }

  num_tensors += priv->infer->model_config.num_out_tensors;
  priv->post_proc->handle =
      vvas_postprocess_create (priv->post_proc->json_string,
      priv->post_proc->library_path, t_info, num_tensors,
      priv->infer->model_config.batch_size, priv->core_log_level);
  if (!priv->post_proc->handle) {
    if (!g_file_test (priv->post_proc->library_path, G_FILE_TEST_EXISTS)) {
      GST_ERROR_OBJECT (self,
          "vvas_postprocess_create failed: %s file not found",
          priv->post_proc->library_path);
    } else {
      GST_ERROR_OBJECT (self, "vvas_postprocess_create failed");
    }
    return FALSE;
  }

  /* Create Memory pool for tensor data */
  std::vector < size_t >sizes = { };

  for (guint i = 0; i < priv->infer->model_config.num_out_tensors; i++) {
    sizes.push_back (priv->infer->model_config.out_tensors[i].size);
  }

  /* Size of buffer pool = TENSOR_POOL_SIZE_MULTIPLIER * infer->max_queue
   * Assuming infer->max_queue = batch_size, then 1 batch infer is processing,
   * 1 batch is in Post Process Queue and 1 batch Post Process thread is processing.
   */
  VvasAllocationType mem_type =
      priv->infer->vart_info.out_tensor_type ==
      vart::TensorType::HW ? VVAS_ALLOC_TYPE_CMA : VVAS_ALLOC_TYPE_NON_CMA;
  priv->post_proc->tensor_pool =
      new VvasTensorPool (priv->post_proc->vvas_ctx,
      TENSOR_POOL_SIZE_MULTIPLIER * priv->infer->max_queue, mem_type,
      VVAS_ALLOC_FLAG_NONE, priv->infer->vart_info.mbank_idx, sizes);

  GST_DEBUG_OBJECT (self, "Post-Process created successfully");

  return TRUE;
}

/**
 * @fn static size_t get_tensor_size_in_bytes (Ort::ConstTensorTypeAndShapeInfo ort_tensor_info)
 * @param [in] ort_tensor_info - Ort::ConstTensorTypeAndShapeInfo
 * @return size of tensor in bytes
 *
 * @brief This function returns the size of tensor in bytes
 */
static size_t
get_tensor_size_in_bytes (ONNXTensorElementDataType element_type,
    const std::vector < int64_t > shape)
{
  auto element_count = std::accumulate (shape.begin (), shape.end (), 1,
      std::multiplies < int64_t > ());
  size_t size = element_count;

  switch (element_type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      size = element_count * sizeof (float);
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
      size = element_count * sizeof (int8_t);
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
      size = element_count * sizeof (uint16_t);
      break;
    default:
      break;
  }
  return size;
}

/**
 * @fn static ONNXTensorElementDataType get_ort_tensor_data_type (VvasTensorDataType data_type)
 * @param [in] data_type - VvasTensorDataType
 * @return ONNXTensorElementDataType
 *
 * @brief This function returns the ONNXTensorElementDataType for the given VvasTensorDataType
 */
static ONNXTensorElementDataType
get_ort_tensor_data_type (VvasTensorDataType data_type)
{
  switch (data_type) {
    case VVAS_TENSOR_DATA_TYPE_FLOAT32:
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    case VVAS_TENSOR_DATA_TYPE_INT8:
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8;
    case VVAS_TENSOR_DATA_TYPE_FP16:
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
    default:
      break;
  }
  return ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
}

/**
 * @fn static VvasTensorDataType get_vvas_data_type (Ort::TensorTypeAndShapeInfo ort_tensor_info)
 * @param [in] ort_tensor_info - Ort::TensorTypeAndShapeInfo
 * @return VvasTensorDataType
 *
 * @brief This function returns the VvasTensorDataType for the given Ort tensor type
 */
static VvasTensorDataType
get_vvas_tensor_data_type (Ort::ConstTensorTypeAndShapeInfo ort_tensor_info)
{
  VvasTensorDataType vvas_data_type = VVAS_TENSOR_DATA_TYPE_UNKNOWN;
  auto element_type = ort_tensor_info.GetElementType ();

  switch (element_type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      vvas_data_type = VVAS_TENSOR_DATA_TYPE_FLOAT32;
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
      vvas_data_type = VVAS_TENSOR_DATA_TYPE_INT8;
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
      vvas_data_type = VVAS_TENSOR_DATA_TYPE_FP16;
      break;
    default:
      break;
  }
  return vvas_data_type;
}

/**
 * @fn static VvasTensorDataType get_vvas_data_type (vart::DataType dt)
 * @param [in] dt - vart::DataType
 * @return VvasTensorDataType
 *
 * @brief This function returns the VvasTensorDataType for the given vart::Datatype
 */
static VvasTensorDataType
get_vvas_tensor_data_type (vart::DataType dt)
{
  VvasTensorDataType vvas_data_type = VVAS_TENSOR_DATA_TYPE_UNKNOWN;

  switch (dt) {
    case vart::DataType::FLOAT32:
      vvas_data_type = VVAS_TENSOR_DATA_TYPE_FLOAT32;
      break;
    case vart::DataType::INT8:
      vvas_data_type = VVAS_TENSOR_DATA_TYPE_INT8;
      break;
    case vart::DataType::BF16:
      vvas_data_type = VVAS_TENSOR_DATA_TYPE_BF16;
      break;
    case vart::DataType::FP16:
      vvas_data_type = VVAS_TENSOR_DATA_TYPE_FP16;
      break;
    default:
      break;
  }
  return vvas_data_type;
}

static void
vvas_xinfer_set_tensor_memory_layout (VvasTensorInfo * tensor, const char *layout)
{
  if (tensor->memory_layout) {
    g_free (tensor->memory_layout);
    tensor->memory_layout = NULL;
  }
  if (layout && layout[0] != '\0')
    tensor->memory_layout = g_strdup (layout);
}

static void
vvas_xinfer_free_tensor_info (VvasTensorInfo * tensor)
{
  if (tensor->name) {
    g_free (tensor->name);
    tensor->name = NULL;
  }
  if (tensor->memory_layout) {
    g_free (tensor->memory_layout);
    tensor->memory_layout = NULL;
  }
}

static gboolean
priv_fill_model_config_vart (GstVvas_XInfer *self)
{
  GstVvas_XInferPrivate *priv = self->priv;
  const std::vector < vart::NpuTensorInfo > input_tensors_info =
      priv->infer->vart_info.
      runner->get_tensors_info (vart::TensorDirection::INPUT,
      priv->infer->vart_info.inp_tensor_type);
  const std::vector < vart::NpuTensorInfo > output_tensors_info =
      priv->infer->vart_info.
      runner->get_tensors_info (vart::TensorDirection::OUTPUT,
      priv->infer->vart_info.out_tensor_type);
  size_t batch_size = priv->infer->vart_info.runner->get_batch_size ();

  priv->infer->model_config.num_in_tensors =
      priv->infer->vart_info.runner->get_num_input_tensors ();
  priv->infer->model_config.num_out_tensors =
      priv->infer->vart_info.runner->get_num_output_tensors ();

  GST_DEBUG_OBJECT (self, "Number of input tensors: %lu, "
      "Number of output tensors: %lu", priv->infer->model_config.num_in_tensors,
      priv->infer->model_config.num_out_tensors);

  /* Validate the number of input tensors, as per assumption */
  if (priv->infer->model_config.num_in_tensors != 1) {
    GST_ERROR_OBJECT (self,
        "Only single input tensor models are supported. Current model has %lu input tensors.",
        priv->infer->model_config.num_in_tensors);
    return FALSE;
  }

  for (size_t i = 0; i < priv->infer->model_config.num_in_tensors; i++) {
    priv->infer->model_config.in_tensors[i].name =
        g_strdup (input_tensors_info[i].name.c_str ());
    priv->infer->model_config.input_names.push_back (priv->infer->
        model_config.in_tensors[i].name);
    priv->infer->model_config.in_tensors[i].direction =
        VVAS_TENSOR_DATA_DIRECTION_INPUT;
    /* vart::nputensor stores shapes as a vector of uint32_t's but model_config.input_shapes is
       a vector of int64_t's.
       Need to cast each element to int64_t to push into model_config.input_shapes; */
    std::vector < int64_t > tmp_shape;
    std::transform (input_tensors_info[i].shape.begin (),
        input_tensors_info[i].shape.end (), std::back_inserter (tmp_shape),
        [](uint32_t val) {
        return static_cast < int64_t > (val);}
    );
    priv->infer->model_config.input_shapes.push_back (std::move (tmp_shape));

    size_t shape_size =
        std::min (priv->infer->model_config.input_shapes.back ().size (),
        static_cast < size_t >(MAX_SHAPE_SIZE));

    priv->infer->model_config.in_tensors[i].valid_shapes = shape_size;
    for (size_t j = 0; j < shape_size; ++j) {
      int64_t dim = priv->infer->model_config.input_shapes.back ()[j];
      if (dim < 0) {
        GST_DEBUG_OBJECT (self, "Negative dimension (%" G_GINT64_FORMAT
            ") in input_shape[%zu], setting to 0", dim, j);
        priv->infer->model_config.dynamic_shape = TRUE;
        priv->infer->model_config.in_tensors[i].shape[j] = 0;
      } else {
        priv->infer->model_config.in_tensors[i].shape[j] =
            static_cast < uint32_t > (dim);
      }
    }
    priv->infer->model_config.in_tensors[i].size =
        input_tensors_info[i].size_in_bytes;
    priv->infer->model_config.in_tensors[i].data_type =
        get_vvas_tensor_data_type (input_tensors_info[i].data_type);
    auto quant =
        priv->infer->vart_info.
        runner->get_quant_parameters (input_tensors_info[i].name);
    GST_DEBUG_OBJECT (self, "Input tensor[%zu] name: %s, scale: %f", i,
        priv->infer->model_config.in_tensors[i].name, quant.scale);
    if (priv->pre_proc->enabled && priv->pre_proc->is_quant_set) {
      priv->infer->model_config.in_tensors[i].scale_coeff =
          (1.0f / priv->pre_proc->quant_data.scale_factor);
    } else if (is_valid_positive_scale (quant.scale)) {
      priv->infer->model_config.in_tensors[i].scale_coeff =
          (1.0f / quant.scale);
    } else {
      priv->infer->model_config.in_tensors[i].scale_coeff = DEFAULT_QT_FCTR;
    }
  }

  /* Set batch size, model height, and model width based on input tensor layout.
   * GENERIC layouts are resolved from 4D shape heuristics so HW/SW preprocessing
   * can pick a concrete video format (NCHW/NHWC). */
  vart::MemoryLayout effective_layout = input_tensors_info[0].memory_layout;
  priv->infer->model_config.batch_size = static_cast<uint32_t>(batch_size);

  if (effective_layout == vart::MemoryLayout::GENERIC) {
    std::string generic_reason;
    uint32_t inferred_width = 0;
    uint32_t inferred_height = 0;

    if (!infer_generic_preprocess_layout (input_tensors_info[0], effective_layout,
            inferred_width, inferred_height, generic_reason)) {
      GST_ERROR_OBJECT (self, "GENERIC layout inference failed: %s",
          generic_reason.c_str ());
      goto vart_error_free_in_tensors;
    }

    priv->infer->model_config.model_width = inferred_width;
    priv->infer->model_config.model_height = inferred_height;
    GST_INFO_OBJECT (self,
        "GENERIC input layout inferred as %s (%ux%u) for PPE",
        vart::to_string (effective_layout).data (), inferred_width, inferred_height);
  } else if (effective_layout == vart::MemoryLayout::NCHW) {
    priv->infer->model_config.model_height = priv->infer->model_config.in_tensors[0].shape[2];
    priv->infer->model_config.model_width = priv->infer->model_config.in_tensors[0].shape[3];
  } else if (effective_layout == vart::MemoryLayout::NHWC) {
    priv->infer->model_config.model_height = priv->infer->model_config.in_tensors[0].shape[1];
    priv->infer->model_config.model_width = priv->infer->model_config.in_tensors[0].shape[2];
  } else if (effective_layout == vart::MemoryLayout::HCWNC4 ||
      effective_layout == vart::MemoryLayout::HCWNC8) {
    priv->infer->model_config.model_height = priv->infer->model_config.in_tensors[0].shape[0];
    priv->infer->model_config.model_width = priv->infer->model_config.in_tensors[0].shape[2];
  } else {
    GST_ERROR_OBJECT(self, "Unknown input tensor layout: %d",
        static_cast<int>(effective_layout));
    goto vart_error_free_in_tensors;
  }

  /* Use resolved effective_layout so postprocess gets NCHW/NHWC/HCWNC4, not GENERIC. */
  vvas_xinfer_set_tensor_memory_layout (&priv->infer->model_config.in_tensors[0],
      vart::to_string (effective_layout).data ());
  GST_DEBUG_OBJECT (self, "Input tensor[0] memory_layout: %s",
      priv->infer->model_config.in_tensors[0].memory_layout);

  if (priv->infer->model_config.num_out_tensors > MAX_TENSORS) {
    GST_ERROR_OBJECT (self,
        "number of output tensors: %zu, but max number of output tensors supported is: %d",
        priv->infer->model_config.num_out_tensors, MAX_TENSORS);
    goto vart_error_free_in_tensors;
  }

  if (priv->post_proc->enabled && !priv->post_proc->dequant_data.empty () &&
      priv->post_proc->dequant_data.size () !=
      priv->infer->model_config.num_out_tensors) {
    GST_ERROR_OBJECT (self,
        "postprocess dequantization.scale-factor count (%zu) must match num output tensors (%zu)",
        priv->post_proc->dequant_data.size (),
        priv->infer->model_config.num_out_tensors);
    goto vart_error_free_in_tensors;
  }

  /* Set output tensor information */
  for (size_t i = 0; i < priv->infer->model_config.num_out_tensors; i++) {
    priv->infer->model_config.out_tensors[i].name =
        g_strdup (output_tensors_info[i].name.c_str ());
    priv->infer->model_config.output_names.push_back (priv->infer->
        model_config.out_tensors[i].name);
    priv->infer->model_config.out_tensors[i].direction =
        VVAS_TENSOR_DATA_DIRECTION_OUTPUT;
    std::vector < int64_t > tmp_shape;
    std::transform (output_tensors_info[i].shape.begin (),
        output_tensors_info[i].shape.end (), std::back_inserter (tmp_shape),
        [](uint32_t val) {
        return static_cast < int64_t > (val);}
    );
    priv->infer->model_config.output_shapes.push_back (std::move (tmp_shape));

    size_t shape_size = priv->infer->model_config.output_shapes.back ().size ();
    priv->infer->model_config.out_tensors[i].valid_shapes = shape_size;
    for (size_t j = 0; j < shape_size; ++j) {
      int64_t dim = priv->infer->model_config.output_shapes.back ()[j];
      priv->infer->model_config.out_tensors[i].shape[j] =
          static_cast < uint32_t > (dim);
    }
    priv->infer->model_config.out_tensors[i].size =
        output_tensors_info[i].size_in_bytes;
    priv->infer->model_config.out_tensors[i].data_type =
        get_vvas_tensor_data_type (output_tensors_info[i].data_type);
    auto quant =
        priv->infer->vart_info.
        runner->get_quant_parameters (output_tensors_info[i].name);
    GST_DEBUG_OBJECT (self, "Output tensor[%zu] name: %s, scale: %f", i,
        priv->infer->model_config.out_tensors[i].name, quant.scale);
    if (priv->post_proc->enabled && priv->post_proc->is_dequant_set) {
      const float sf = priv->post_proc->dequant_data[i].scale_factor;
      priv->infer->model_config.out_tensors[i].scale_coeff = (1.0f / sf);
    } else if (is_valid_positive_scale (quant.scale)) {
      priv->infer->model_config.out_tensors[i].scale_coeff =
          (1.0f / quant.scale);
    } else {
      priv->infer->model_config.out_tensors[i].scale_coeff = DEFAULT_QT_FCTR;
    }
    vvas_xinfer_set_tensor_memory_layout (&priv->infer->model_config.out_tensors[i],
        vart::to_string (output_tensors_info[i].memory_layout).data ());
    GST_DEBUG_OBJECT (self, "Output tensor[%zu] memory_layout: %s", i,
        priv->infer->model_config.out_tensors[i].memory_layout);
  }

  priv->infer->input_tensor_format = get_tensor_format (priv->infer->model_format,
      effective_layout,
      priv->infer->model_config.in_tensors[0].data_type);
  if (priv->infer->input_tensor_format == VVAS_VIDEO_FORMAT_UNKNOWN) {
    GST_ERROR_OBJECT (self, "Failed to get input tensor format");
    goto vart_error_free_all_tensors;
  }

  return TRUE;

vart_error_free_all_tensors:
  for (size_t i = 0; i < priv->infer->model_config.num_out_tensors; i++) {
    vvas_xinfer_free_tensor_info (&priv->infer->model_config.out_tensors[i]);
  }
vart_error_free_in_tensors:
  for (size_t i = 0; i < priv->infer->model_config.num_in_tensors; i++) {
    vvas_xinfer_free_tensor_info (&priv->infer->model_config.in_tensors[i]);
  }
  return FALSE;
}

/**
 * @fn static gboolean vvas_xinfer_infer_init (GstVvas_XInfer * self)
 * @param [in] self - handle to GstVvas_XInfer
 * @return TRUE on success
 *         FALSE on failure
 *
 * @brief Initialize the infer private parameters
 * @detail This function calls kenrel init func and populate vvas_handle for infer kernel
 *
 */
static gboolean
vvas_xinfer_infer_init (GstVvas_XInfer *self)
{
  GstVvas_XInferPrivate *priv = self->priv;
  VvasReturnType vret;
  /* OnnxRuntime default allocator */
  Ort::AllocatorWithDefaultOptions allocator;
  if (priv->infer->runtime == MLRuntime::VART &&
      priv->infer->vart_info.inp_tensor_type == vart::TensorType::HW) {
    priv->infer->vvas_ctx = vvas_context_create (XDNA_DEVICE_IDX,
        NULL, priv->core_log_level, &vret);
  } else {
    priv->infer->vvas_ctx = vvas_context_create (-1, NULL,
        priv->core_log_level, &vret);
  }
  if (!priv->infer->vvas_ctx) {
    GST_ERROR_OBJECT (self, "Couldn't create VVAS context");
    return FALSE;
  }

  priv->infer->core_handle =
      (VvasCoreModule *) calloc (1, sizeof (VvasCoreModule));
  if (!priv->infer->core_handle) {
    GST_ERROR_OBJECT (self, "Failed to allocate memory");
    return FALSE;
  }

  if (priv->infer->runtime == MLRuntime::VART) {
    try {
      std::string input_tensor_type =
          priv->infer->vart_info.inp_tensor_type ==
          vart::TensorType::HW ? std::string ("HW") : std::string ("CPU");
      std::string output_tensor_type =
          priv->infer->vart_info.out_tensor_type ==
          vart::TensorType::HW ? std::string ("HW") : std::string ("CPU");
      uint32_t xdna_device_index = 0;
      GST_DEBUG_OBJECT (self,
          "Create VART runner E path = %s, in_tensor_type = %s, out_tensor_type = %s\n",
          priv->infer->vart_info.model_path.c_str (),
          input_tensor_type.c_str (), output_tensor_type.c_str ());
      std::unordered_map < std::string, std::any > options = {
        {"debug", false},
        {"no_failsafe", false},
        {"ai_analyzer_profiling", priv->infer->vart_info.ai_analyzer_profiling},
        {"input_tensor_type", input_tensor_type},
        {"output_tensor_type", output_tensor_type},
        {"xdna_device_index", xdna_device_index}
      };
      if (priv->infer->vart_info.is_columns_sharing_option_provided) {
        options["aie_columns_sharing"] =
            priv->infer->vart_info.aie_columns_sharing;
      }
      if (priv->infer->vart_info.is_start_column_option_provided) {
        options["start_column"] = priv->infer->vart_info.start_column;
      }
      if (priv->infer->vart_info.config_file_path != "") {
        options["config_json"] = priv->infer->vart_info.config_file_path;
      }
      if (priv->infer->vart_info.use_async) {
        /* Ensure async completion callbacks are delivered in submission
         * order so results are queued to post-process in order. */
        options["callback_order"] = std::string("submission");
        GST_INFO_OBJECT (self, "async inference enabled; callback_order=submission");
      }
      guint64 t0 = 0;
      if (priv->infer_profiler.enabled) {
        strcpy (priv->infer_profiler.backend_runtime, "VART");
        t0 = vvas_profiler_now_us ();
      }

      priv->infer->vart_info.runner =
          vart::RunnerFactory::create_runner (vart::RunnerType::VAIML,
          priv->infer->vart_info.model_path, options);

      if (priv->infer_profiler.enabled) {
        g_mutex_lock (&priv->infer_profiler.snap_lock);
        priv->infer_profiler.infer.init_us = vvas_profiler_now_us () - t0;
        priv->infer_profiler.infer.enabled = TRUE;
        g_mutex_unlock (&priv->infer_profiler.snap_lock);
        GST_INFO_OBJECT (self, "VART runner creation time: %lu us (%.3f ms)",
            priv->infer_profiler.infer.init_us,
            priv->infer_profiler.infer.init_us / 1000.0);
      }
      GST_DEBUG_OBJECT (self, "VART runner created successfully\n");
    }
    catch (const std::exception & e)
    {
      GST_ERROR_OBJECT (self, "Create runner ERROR :: %s\n", e.what ());
      free (priv->infer->core_handle);
      priv->infer->core_handle = NULL;
      return FALSE;
    }
  } else {
    GST_DEBUG_OBJECT (self, "Creating Onnx Runtime session for ONNX model: %s",
        priv->infer->ort_info.model_path.c_str ());
    /* Create an Onnx Session and store it */
    priv->infer->ort_info.env =
        std::make_unique < Ort::Env > (ORT_LOGGING_LEVEL_FATAL, "vvas_xinfer");
    Ort::SessionOptions session_options;
    session_options.SetLogSeverityLevel (ORT_LOGGING_LEVEL_FATAL);
    auto options = std::unordered_map < std::string, std::string > { };
    try {
      if (priv->infer->ort_info.enable_profiling) {
        if (priv->infer->ort_info.profiling_file_path != "")
          session_options.EnableProfiling (priv->infer->
              ort_info.profiling_file_path.c_str ());
        else
          session_options.EnableProfiling ("onnxruntime_profile_");
      }
      if (priv->infer->ort_info.ep == OnnxRuntimeEP::VITIS_AI) {
        if (priv->infer->ort_info.vai_conf.file_path != "") {
          options["config_file"] = priv->infer->ort_info.vai_conf.file_path;
          auto target =
              get_runtime_target (priv->infer->ort_info.vai_conf.file_path);
          if (target.has_value ()) {
            GST_DEBUG_OBJECT (self, "Setting target: %s",
                target.value ().c_str ());
            options["target"] = target.value ();
          }
        }
        if (priv->infer->ort_info.vai_conf.ai_analyzer_visualization)
          options["ai_analyzer_visualization"] = "True";
        else
          options["ai_analyzer_visualization"] = "False";
        if (priv->infer->ort_info.vai_conf.ai_analyzer_profiling)
          options["ai_analyzer_profiling"] = "True";
        else
          options["ai_analyzer_profiling"] = "False";
        if (priv->infer->ort_info.vai_conf.cache_dir != "")
          options["cache_dir"] = priv->infer->ort_info.vai_conf.cache_dir;
        if (priv->infer->ort_info.vai_conf.cache_key != "")
          options["cache_key"] = priv->infer->ort_info.vai_conf.cache_key;
        session_options.AppendExecutionProvider_VitisAI (options);
      }

      guint64 t0 = 0;
      if (priv->infer_profiler.enabled) {
        if (priv->infer->ort_info.ep == OnnxRuntimeEP::VITIS_AI)
          strcpy (priv->infer_profiler.backend_runtime,
              "onnxruntime:vitisai-ep");
        else
          strcpy (priv->infer_profiler.backend_runtime, "onnxruntime:cpu-ep");
        t0 = vvas_profiler_now_us ();
      }

      priv->infer->ort_info.session =
          std::make_unique < Ort::Session > (*priv->infer->ort_info.env,
          priv->infer->ort_info.model_path.c_str (), session_options);

      if (priv->infer_profiler.enabled) {
        g_mutex_lock (&priv->infer_profiler.snap_lock);
        priv->infer_profiler.infer.init_us = vvas_profiler_now_us () - t0;
        priv->infer_profiler.infer.enabled = TRUE;
        g_mutex_unlock (&priv->infer_profiler.snap_lock);
        GST_INFO_OBJECT (self, "%s session creation time: %lu us (%.3f ms)",
            priv->infer_profiler.backend_runtime,
            priv->infer_profiler.infer.init_us,
            priv->infer_profiler.infer.init_us / 1000.0);
      }
    }
    catch (const std::exception & e)
    {
      GST_ERROR_OBJECT (self, "Couldn't create ONNX Runtime session: %s",
          e.what ());
      priv->infer->ort_info.env.reset ();
      free (priv->infer->core_handle);
      priv->infer->core_handle = NULL;
      return false;
    }
  }

  if (priv->infer->runtime == MLRuntime::VART) {
    if (!priv_fill_model_config_vart (self))
      return FALSE;
  } else {
    /* Get input and output tensors to extract their info */
    auto num_in_tensors = priv->infer->ort_info.session->GetInputCount ();
    auto num_out_tensors = priv->infer->ort_info.session->GetOutputCount ();

    GST_DEBUG_OBJECT (self, "Number of input tensors: %lu, "
        "Number of output tensors: %lu", num_in_tensors, num_out_tensors);

    priv->infer->model_config.num_in_tensors = num_in_tensors;
    priv->infer->model_config.num_out_tensors = num_out_tensors;

    /* Extract information about the input tensor */
    GST_DEBUG_OBJECT (self, "Input shape format: %s",
        priv->infer->ort_info.input_tensor_layout.c_str ());
    for (size_t i = 0; i < num_in_tensors; i++) {
      auto name_ptr =
          priv->infer->ort_info.session->GetInputNameAllocated (i, allocator);
      priv->infer->model_config.in_tensors[i].name = g_strdup (name_ptr.get ());
      priv->infer->model_config.input_names.push_back (priv->
          infer->model_config.in_tensors[i].name);
      priv->infer->model_config.in_tensors[i].direction =
          VVAS_TENSOR_DATA_DIRECTION_INPUT;
      auto type_info = priv->infer->ort_info.session->GetInputTypeInfo (i);
      auto ort_tensor_info = type_info.GetTensorTypeAndShapeInfo ();
      auto tensor_shape = ort_tensor_info.GetShape ();
      if (tensor_shape[0] == -1) {
        priv->infer->model_config.dynamic_shape = TRUE;
        tensor_shape = get_fixed_shape (tensor_shape, priv->infer->batch_size);
      }
      priv->infer->model_config.input_shapes.push_back (tensor_shape);
      size_t shape_size =
          std::min (priv->infer->model_config.input_shapes.back ().size (),
          static_cast < size_t >(MAX_SHAPE_SIZE));
      priv->infer->model_config.in_tensors[i].valid_shapes = shape_size;
      for (size_t j = 0; j < shape_size; ++j) {
        int64_t dim = priv->infer->model_config.input_shapes.back ()[j];
        priv->infer->model_config.in_tensors[i].shape[j] =
            static_cast < uint32_t > (dim);
      }
      priv->infer->model_config.in_tensors[i].size =
          get_tensor_size_in_bytes (ort_tensor_info.GetElementType (),
          std::move (tensor_shape));
      priv->infer->model_config.in_tensors[i].data_type =
          get_vvas_tensor_data_type (ort_tensor_info);
      priv->infer->model_config.in_tensors[i].scale_coeff = DEFAULT_QT_FCTR;
      vvas_xinfer_set_tensor_memory_layout (&priv->infer->model_config.in_tensors[i],
          priv->infer->ort_info.input_tensor_layout.c_str ());
    }

    /* Set batch size, model height, and model width based on input tensor layout */
    priv->infer->model_config.batch_size =
        priv->infer->model_config.in_tensors[0].shape[0];
    if (priv->infer->ort_info.input_tensor_layout == "NCHW") {
      priv->infer->model_config.model_height =
          priv->infer->model_config.in_tensors[0].shape[2];
      priv->infer->model_config.model_width =
          priv->infer->model_config.in_tensors[0].shape[3];
    } else if (priv->infer->ort_info.input_tensor_layout == "NHWC") {
      priv->infer->model_config.model_height =
          priv->infer->model_config.in_tensors[0].shape[1];
      priv->infer->model_config.model_width =
          priv->infer->model_config.in_tensors[0].shape[2];
    } else {
      GST_ERROR_OBJECT (self, "Unknown input tensor layout: %s",
          priv->infer->ort_info.input_tensor_layout.c_str ());
      goto onnx_error_free_in_tensors;
    }

    /* Validate the number of input tensors, as per assumption */
    if (priv->infer->model_config.num_in_tensors != 1) {
      GST_ERROR_OBJECT (self,
          "Only single input tensor models are supported. Current model has %lu input tensors.",
          priv->infer->model_config.num_in_tensors);
      goto onnx_error_free_in_tensors;
    }

    if (priv->infer->model_config.num_out_tensors > MAX_TENSORS) {
      GST_ERROR_OBJECT (self,
          "number of output tensors: %zu, but max number of output tensors supported is: %d",
          priv->infer->model_config.num_out_tensors, MAX_TENSORS);
      goto onnx_error_free_in_tensors;
    }

    /* Set output tensor information */
    for (size_t i = 0; i < num_out_tensors; i++) {
      auto name_ptr =
          priv->infer->ort_info.session->GetOutputNameAllocated (i, allocator);
      priv->infer->model_config.out_tensors[i].name =
          g_strdup (name_ptr.get ());
      priv->infer->model_config.output_names.push_back (priv->
          infer->model_config.out_tensors[i].name);
      priv->infer->model_config.out_tensors[i].direction =
          VVAS_TENSOR_DATA_DIRECTION_OUTPUT;
      auto type_info = priv->infer->ort_info.session->GetOutputTypeInfo (i);
      auto ort_tensor_info = type_info.GetTensorTypeAndShapeInfo ();
      auto tensor_shape = ort_tensor_info.GetShape ();
      if (tensor_shape[0] == -1) {
        priv->infer->model_config.dynamic_shape = TRUE;
        tensor_shape = get_fixed_shape (tensor_shape, priv->infer->batch_size);
      }
      priv->infer->model_config.output_shapes.push_back (tensor_shape);
      size_t shape_size =
          priv->infer->model_config.output_shapes.back ().size ();
      priv->infer->model_config.out_tensors[i].valid_shapes = shape_size;
      for (size_t j = 0; j < shape_size; ++j) {
        int64_t dim = priv->infer->model_config.output_shapes.back ()[j];
        priv->infer->model_config.out_tensors[i].shape[j] =
            static_cast < uint32_t > (dim);
      }
      priv->infer->model_config.out_tensors[i].size =
          get_tensor_size_in_bytes (ort_tensor_info.GetElementType (),
          std::move (tensor_shape));
      priv->infer->model_config.out_tensors[i].data_type =
          get_vvas_tensor_data_type (ort_tensor_info);
      priv->infer->model_config.out_tensors[i].scale_coeff = DEFAULT_QT_FCTR;
      if (!priv->infer->ort_info.output_tensor_layout.empty ()) {
        vvas_xinfer_set_tensor_memory_layout (&priv->infer->model_config.out_tensors[i],
            priv->infer->ort_info.output_tensor_layout.c_str ());
      }
    }

    priv->infer->input_tensor_format =
        get_tensor_format (priv->infer->model_format,
        priv->infer->ort_info.input_tensor_layout,
        priv->infer->model_config.in_tensors[0].data_type);
    if (priv->infer->input_tensor_format == VVAS_VIDEO_FORMAT_UNKNOWN) {
      GST_ERROR_OBJECT (self,
          "Failed to get input tensor format for model format: %d, layout: %s, data type: %d",
          priv->infer->model_format,
          priv->infer->ort_info.input_tensor_layout.c_str (),
          priv->infer->model_config.in_tensors[0].data_type);
      goto onnx_error_free_all_tensors;
    }
    goto onnx_tensor_config_done;

  onnx_error_free_all_tensors:
    for (size_t i = 0; i < num_out_tensors; i++) {
      vvas_xinfer_free_tensor_info (&priv->infer->model_config.out_tensors[i]);
    }
  onnx_error_free_in_tensors:
    for (size_t i = 0; i < num_in_tensors; i++) {
      vvas_xinfer_free_tensor_info (&priv->infer->model_config.in_tensors[i]);
    }
    if (priv->infer->ort_info.session) {
      priv->infer->ort_info.session.reset ();
      priv->infer->ort_info.session = nullptr;
    }
    if (priv->infer->ort_info.env) {
      priv->infer->ort_info.env.reset ();
      priv->infer->ort_info.env = nullptr;
    }
    if (priv->infer->vvas_ctx) {
      vvas_context_destroy (priv->infer->vvas_ctx);
      priv->infer->vvas_ctx = NULL;
    }
    if (priv->infer->core_handle) {
      free (priv->infer->core_handle);
      priv->infer->core_handle = NULL;
    }
    return FALSE;
  onnx_tensor_config_done:;
  }

  /* set VvasVideoFormat for input tensor type based on network data type and layout */
  if (priv->infer->model_config.in_tensors[0].data_type ==
      VVAS_TENSOR_DATA_TYPE_UNKNOWN) {
    GST_ERROR_OBJECT (self, "Input Tensor Type %d is not compatible with VVAS",
        static_cast < int >(priv->infer->model_config.in_tensors[0].data_type));
    return FALSE;
  }

  {
    size_t numInputTensor = priv->infer->model_config.num_in_tensors;
    size_t numOutputTensor = priv->infer->model_config.num_out_tensors;
    std::ostringstream shape_stream;

    GST_DEBUG_OBJECT (self, "Input tensor format set to %d",
        priv->infer->input_tensor_format ==
        VVAS_VIDEO_FORMAT_UNKNOWN ? priv->infer->model_format : priv->
        infer->input_tensor_format);
    GST_DEBUG_OBJECT (self, "Number of Batches: %d",
        priv->infer->model_config.batch_size);
    GST_DEBUG_OBJECT (self, "Number of Inputs: %ld",
        priv->infer->model_config.num_in_tensors);
    GST_DEBUG_OBJECT (self, "Number of Outputs: %ld",
        priv->infer->model_config.num_out_tensors);
    GST_DEBUG_OBJECT (self, "Model width: %d",
        priv->infer->model_config.model_width);
    GST_DEBUG_OBJECT (self, "Model height: %d",
        priv->infer->model_config.model_height);

    GST_DEBUG_OBJECT (self, "Tensor Info...");
    for (size_t j = 0; j < numInputTensor; ++j) {
      GST_DEBUG_OBJECT (self, "Input tensor name [%zu]: %s",
          j, priv->infer->model_config.in_tensors[j].name);
      GST_DEBUG_OBJECT (self, "InputTensor[%lu] size %u", j,
          priv->infer->model_config.in_tensors[j].size);
      GST_DEBUG_OBJECT (self, "InputTensor[%lu] type %u", j,
          priv->infer->model_config.in_tensors[j].data_type);
      GST_DEBUG_OBJECT (self,
          "InputTensor[%lu] quantization_factor %lf", j,
          priv->infer->model_config.in_tensors[j].scale_coeff);
      for (size_t k = 0;
          k < priv->infer->model_config.in_tensors[j].valid_shapes; ++k) {
        shape_stream << priv->infer->model_config.in_tensors[j].shape[k];
        if (k + 1 < priv->infer->model_config.in_tensors[j].valid_shapes)
          shape_stream << "*";
      }
      GST_DEBUG_OBJECT (self, "Input tensor shape [%zu]: %s", j,
          shape_stream.str ().c_str ());
      shape_stream.str ("");
      shape_stream.clear ();
    }

    for (size_t j = 0; j < numOutputTensor; ++j) {
      GST_DEBUG_OBJECT (self, "Output tensor name [%zu]: %s",
          j, priv->infer->model_config.out_tensors[j].name);
      GST_DEBUG_OBJECT (self, "OutputTensor[%lu] size %u", j,
          priv->infer->model_config.out_tensors[j].size);
      GST_DEBUG_OBJECT (self, "OutputTensor[%lu] type %u", j,
          priv->infer->model_config.out_tensors[j].data_type);
      GST_DEBUG_OBJECT (self,
          "OutputTensor[%lu] quantization_factor %lf", j,
          priv->infer->model_config.out_tensors[j].scale_coeff);
      for (size_t k = 0;
          k < priv->infer->model_config.out_tensors[j].valid_shapes; ++k) {
        shape_stream << priv->infer->model_config.out_tensors[j].shape[k];
        if (k + 1 < priv->infer->model_config.out_tensors[j].valid_shapes)
          shape_stream << "*";
      }
      GST_DEBUG_OBJECT (self, "Output tensor shape [%zu]: %s", j,
          shape_stream.str ().c_str ());
      shape_stream.str ("");
      shape_stream.clear ();
    }
  }

  if (priv->infer->batch_size == 0 ||
      priv->infer->batch_size != priv->infer->model_config.batch_size) {
    GST_WARNING_OBJECT (self, "infer->batch_size (%d) can't be zero"
        " or other than model supported batch size."
        "taking batch-size %d",
        priv->infer->batch_size, priv->infer->model_config.batch_size);
    priv->infer->batch_size = priv->infer->model_config.batch_size;
  }

  if (priv->infer->max_queue < priv->infer->batch_size) {
    GST_WARNING_OBJECT (self, "inference-max-queue can't be less than "
        "batch-size. taking batch-size %d as default queue length",
        priv->infer->batch_size);
    priv->infer->max_queue = priv->infer->batch_size;
  }

  return TRUE;
}

/**
 * @fn static gboolean vvas_xinfer_ppe_deinit (GstVvas_XInfer * self)
 * @param [in] self - Handle to GstVvas_XInfer
 * @return TRUE on success
 *         FALSE on failure
 *
 * @brief De-Initialize the PPE private parameters
 * @detail This function calls image process deinit function and
 *        also close the xrt context. Deallocation of different
 *        memory is also part of this function.
 *
 */
static gboolean
vvas_xinfer_ppe_deinit (GstVvas_XInfer *self)
{
  GstVvas_XInferPrivate *priv = self->priv;
  VvasCoreModule *ppe_handle = priv->pre_proc->core_handle;
  VvasReturnType vret;

  if (ppe_handle) {
    if (ppe_handle->handle) {
      guint64 t0 = 0;
      if (priv->infer_profiler.enabled && priv->infer_profiler.pre_proc.enabled) {
        t0 = vvas_profiler_now_us ();
      }
      vret = vvas_image_process_destroy (ppe_handle->handle);
      if (vret != VVAS_RET_SUCCESS) {
        GST_ERROR_OBJECT (self, "failed to do preprocess deinit..");
      }
      if (priv->infer_profiler.enabled && priv->infer_profiler.pre_proc.enabled) {
        g_mutex_lock (&priv->infer_profiler.snap_lock);
        priv->infer_profiler.pre_proc.deinit_us = vvas_profiler_now_us () - t0;
        g_mutex_unlock (&priv->infer_profiler.snap_lock);
        GST_DEBUG_OBJECT (self, "Pre-process deinitialization time: %lu us",
            priv->infer_profiler.pre_proc.deinit_us);
      }
      GST_DEBUG_OBJECT (self, "successfully completed preprocess deinit");
    }

    g_free (ppe_handle->name);
    free (priv->pre_proc->core_handle);
    priv->pre_proc->core_handle = NULL;

  }

  if (priv->pre_proc->caps) {
    free (priv->pre_proc->caps);
    priv->pre_proc->caps = NULL;
  }
  if (priv->pre_proc->init_config.lib_config) {
    g_free ((gpointer) priv->pre_proc->init_config.lib_config);
    priv->pre_proc->init_config.lib_config = NULL;
  }
  if (priv->pre_proc->xclbin_loc)
    g_free (priv->pre_proc->xclbin_loc);

  /* Destroy VVAS Context */
  if (priv->pre_proc->vvas_ctx) {
    vvas_context_destroy (priv->pre_proc->vvas_ctx);
    priv->pre_proc->vvas_ctx = NULL;
  }

  priv->pre_proc.reset ();

  return TRUE;
}

/**
 * @fn static gboolean vvas_xinfer_postproc_deinit (GstVvas_XInfer * self)
 * @param [in] self - Handle to GstVvas_XInfer
 * @return TRUE on success
 *         FALSE on failure
 *
 * @brief De-Initialize the post process private parameters
 * @detail Deallocation of different memory is part of this function.
 *
 */
static gboolean
vvas_xinfer_postproc_deinit (GstVvas_XInfer *self)
{
  GstVvas_XInferPrivate *priv = self->priv;
  VvasReturnType vret;

  /* Delete Tensor Pool */
  if (priv->post_proc->tensor_pool) {
    delete priv->post_proc->tensor_pool;
  }

  /* Destroy VvasContext */
  if (priv->post_proc->vvas_ctx) {
    vvas_context_destroy (priv->post_proc->vvas_ctx);
    priv->post_proc->vvas_ctx = NULL;
  }

  /* Destroy Post Process handle */
  if (priv->post_proc->handle) {
    guint64 t0 = 0;
    if (priv->infer_profiler.enabled) {
      t0 = vvas_profiler_now_us ();
    }
    vret = vvas_postprocess_destroy (priv->post_proc->handle);
    if (VVAS_RET_SUCCESS != vret) {
      GST_ERROR_OBJECT (self, "Failed to destroy post process");
    }
    if (priv->infer_profiler.enabled && priv->infer_profiler.post_proc.enabled) {
      g_mutex_lock (&priv->infer_profiler.snap_lock);
      priv->infer_profiler.post_proc.deinit_us = vvas_profiler_now_us () - t0;
      g_mutex_unlock (&priv->infer_profiler.snap_lock);
      GST_DEBUG_OBJECT (self, "Post-process deinitialization time: %lu us",
          priv->infer_profiler.post_proc.deinit_us);
    }
  }

  /* Free string allocations */
  g_free (priv->post_proc->json_string);
  g_free (priv->post_proc->library_path);

  priv->post_proc->handle = NULL;
  priv->post_proc->json_string = NULL;
  priv->post_proc->library_path = NULL;

  priv->post_proc.reset ();

  return TRUE;
}

/**
 * @fn static gboolean vvas_xinfer_infer_deinit (GstVvas_XInfer * self)
 * @param [in] self - Handle to GstVvas_XInfer
 * @return TRUE on success
 *         FALSE on failure
 *
 * @brief This function calls kernel deinit function and
 *        deallocation of different memory is also part of this function.
 *
 */
static gboolean
vvas_xinfer_infer_deinit (GstVvas_XInfer *self)
{
  GstVvas_XInferPrivate *priv = self->priv;
  VvasCoreModule *infer_handle = priv->infer->core_handle;

  if (infer_handle) {
    if (priv->infer->ort_info.session) {
      priv->infer->ort_info.session.reset ();
      priv->infer->ort_info.session = nullptr;
      priv->infer->ort_info.env.reset ();
      priv->infer->ort_info.env = nullptr;
    }
    free (priv->infer->core_handle);
    priv->infer->core_handle = NULL;
  }

  if (priv->infer->input_class_filters) {
    g_list_free_full (priv->infer->input_class_filters,
        (GDestroyNotify) g_free);
    priv->infer->input_class_filters = NULL;
  }

  for (size_t i = 0; i < priv->infer->model_config.num_in_tensors; i++) {
    vvas_xinfer_free_tensor_info (&priv->infer->model_config.in_tensors[i]);
  }

  for (size_t i = 0; i < priv->infer->model_config.num_out_tensors; i++) {
    vvas_xinfer_free_tensor_info (&priv->infer->model_config.out_tensors[i]);
  }

  priv->infer->model_config.num_in_tensors = 0;
  priv->infer->model_config.num_out_tensors = 0;
  priv->infer->model_config.input_names.clear ();
  priv->infer->model_config.input_names.shrink_to_fit ();
  priv->infer->model_config.output_names.clear ();
  priv->infer->model_config.output_names.shrink_to_fit ();
  priv->infer->model_config.input_shapes.clear ();
  priv->infer->model_config.input_shapes.shrink_to_fit ();
  priv->infer->model_config.output_shapes.clear ();
  priv->infer->model_config.output_shapes.shrink_to_fit ();

  /* Destroy VVAS Context */
  if (priv->infer->vvas_ctx) {
    vvas_context_destroy (priv->infer->vvas_ctx);
    priv->infer->vvas_ctx = NULL;
  }

  guint64 t0 = 0;
  if (priv->infer_profiler.enabled && priv->infer_profiler.infer.enabled) {
    t0 = vvas_profiler_now_us ();
  }

  priv->infer.reset ();

  if (priv->infer_profiler.enabled && priv->infer_profiler.infer.enabled) {
    g_mutex_lock (&priv->infer_profiler.snap_lock);
    priv->infer_profiler.infer.deinit_us = vvas_profiler_now_us () - t0;
    g_mutex_unlock (&priv->infer_profiler.snap_lock);
    GST_DEBUG_OBJECT (self,
        "Inference deinitialization time (backend: %s): %lu us",
        priv->infer_profiler.backend_runtime,
        priv->infer_profiler.infer.deinit_us);
  }

  return TRUE;
}

/**
 * For VVAS memory exported as DMABUF, upstream may leave VVAS_SYNC_TO_DEVICE
 * unset after CPU writes; set it and flush via gst_vvas_memory_sync_bo().
 * If VVAS_SYNC_FROM_DEVICE is already set (another HW stage wrote the buffer,
 * e.g. vvas_xabrscaler), do not add VVAS_SYNC_TO_DEVICE: gst_vvas_memory_sync_bo()
 * rejects having both flags set.
 *
 * @param self plugin instance (for logging)
 * @param in_mem first plane memory from input buffer
 * @return TRUE on success, FALSE if sync_bo failed
 */
static gboolean
vvas_xinfer_sync_vvas_dmabuf_to_device_if_needed (GstVvas_XInfer *self,
    GstMemory *in_mem)
{
  if (!gst_is_dmabuf_memory (in_mem) || !gst_is_vvas_memory (in_mem))
    return TRUE;

  if (gst_vvas_memory_get_sync_flag (in_mem) & VVAS_SYNC_FROM_DEVICE) {
    GST_DEBUG_OBJECT (self,
        "skipping VVAS_SYNC_TO_DEVICE: input has VVAS_SYNC_FROM_DEVICE "
        "(device-writer upstream, e.g. HW scaler; avoid sync flag conflict)");
    return TRUE;
  }

  gst_vvas_memory_set_flag (in_mem, VVAS_SYNC_TO_DEVICE);
  if (!gst_vvas_memory_sync_bo (in_mem)) {
    GST_ERROR_OBJECT (self,
        "gst_vvas_memory_sync_bo failed for VVAS DMABUF input");
    return FALSE;
  }

  return TRUE;
}

/**
 * @fn static gboolean vvas_xinfer_prepare_ppe_input_frame (GstVvas_XInfer * self, GstBuffer * inbuf,
 *                                                          GstVideoInfo * in_vinfo, GstBuffer ** new_inbuf,
 *                                                          VvasVideoFrame ** vvas_frame)
 * @param [in] self - handle to GstVvas_XInfer
 * @param [in] inbuf - input GstBuffer
 * @param [in] in_vinfo - input video info
 * @param [out] new_inbuf - new buf if it is copied to internal buffer else NULL
 * @param [inout] vvas_frame - populated values from buffer and video info
 *
 * @return TRUE on success
 *         FALSE on failure
 *
 * @brief Prepare ppe frame from input buffer and input video info
 * @detail This function populate the vvas frame with required values extracted
 *	  from input buffer and video info. The function also copies the data
 *	  from inbuf to new internal buffer and return new buffer in new_inbuf
 *	  only if inbuf is
 *           - not vvas buffer
 *           - not dma buffer
 *           - not on same device
 *           - is not on same memory bank of same device
 *
 */
static gboolean
vvas_xinfer_prepare_ppe_input_frame (GstVvas_XInfer *self, GstBuffer *inbuf,
    GstVideoInfo *in_vinfo, GstBuffer **new_inbuf, VvasVideoFrame **vvas_frame)
{
  GstVvas_XInferPrivate *priv = self->priv;
  guint64 phy_addr = 0;
  vvasBOHandle bo_handle = NULL;
  gboolean free_bo = FALSE;
  gboolean bret = FALSE;
  GstMemory *in_mem = NULL;
  GstMapFlags map_flags;

  if (!priv->pre_proc->use_software) {
    in_mem = gst_buffer_get_memory (inbuf, 0);
    if (in_mem == NULL) {
      GST_ERROR_OBJECT (self, "failed to get memory from input buffer");
      goto error;
    }

    /* prepare HW input buffer to send it to preprocess */
    if (gst_is_vvas_memory (in_mem)) {
      if (gst_vvas_memory_can_avoid_copy (in_mem, priv->pre_proc->dev_idx,
              priv->pre_proc->in_mem_bank)) {
        phy_addr = gst_vvas_allocator_get_paddr (in_mem);
        bo_handle = gst_vvas_allocator_get_bo (in_mem);
      }
    } else if (gst_is_dmabuf_memory (in_mem)) {
      gint dma_fd = -1;

      dma_fd = gst_dmabuf_memory_get_fd (in_mem);
      if (dma_fd < 0) {
        GST_ERROR_OBJECT (self, "failed to get DMABUF FD");
        goto error;
      }

      /* dmabuf but not from vvas allocator */
      bo_handle = vvas_xrt_import_bo (priv->pre_proc->dev_handle, dma_fd);
      if (bo_handle == NULL) {
        GST_WARNING_OBJECT (self,
            "failed to get XRT BO...fall back to copy input");
      }
      /* Lets free the bo_handle after sub bo creation */
      free_bo = TRUE;

      GST_DEBUG_OBJECT (self, "received dma fd %d and its xrt BO = %p", dma_fd,
          bo_handle);

      phy_addr = vvas_xrt_get_bo_phy_addres (bo_handle);
    }

    /* not vvas or dma buffer */
    if (!phy_addr) {
      GST_DEBUG_OBJECT (self,
          "could not get phy_addr, copy input buffer to internal pool buffer");
      bret = vvas_xinfer_copy_input_buffer (self, inbuf, new_inbuf,
          priv->pre_proc->dev_idx, priv->pre_proc->xclbin_loc,
          priv->pre_proc->in_mem_bank, PPE_STRIDE_ALIGN);
      if (!bret)
        goto error;

      gst_memory_unref (in_mem);
      in_mem = gst_buffer_get_memory (*new_inbuf, 0);
      if (in_mem == NULL) {
        GST_ERROR_OBJECT (self, "failed to get memory from input buffer");
        goto error;
      }

      phy_addr = gst_vvas_allocator_get_paddr (in_mem);
      if (!phy_addr) {
        GST_ERROR_OBJECT (self, "failed to get physical address");
        goto error;
      }
      bo_handle = gst_vvas_allocator_get_bo (in_mem);
      inbuf = *new_inbuf;
    }
    /* VVAS DMABUF: flush CPU writes when needed; see
     * vvas_xinfer_sync_vvas_dmabuf_to_device_if_needed(). Non-DMABUF VVAS
     * may still need sync_bo when TO_DEVICE is set elsewhere. */
    bret = vvas_xinfer_sync_vvas_dmabuf_to_device_if_needed (self, in_mem);
    if (!bret)
      goto error;
    bret = gst_vvas_memory_sync_bo (in_mem);
    if (!bret)
      goto error;

    GST_LOG_OBJECT (self, "input paddr %p", (void *) phy_addr);

    gst_memory_unref (in_mem);
    in_mem = NULL;

    if (free_bo && bo_handle) {
      vvas_xrt_free_bo (bo_handle);
    }

    map_flags =
        static_cast < GstMapFlags >
        (GST_MAP_READ | GST_VIDEO_FRAME_MAP_FLAG_NO_REF);
    *vvas_frame =
        vvas_videoframe_from_gstbuffer (priv->pre_proc->vvas_ctx,
        priv->pre_proc->in_mem_bank, inbuf, in_vinfo, map_flags);
  } else {
    map_flags =
        static_cast < GstMapFlags >
        (GST_MAP_READ | GST_VIDEO_FRAME_MAP_FLAG_NO_REF);
    *vvas_frame =
        vvas_videoframe_from_gstbuffer (priv->pre_proc->vvas_ctx, -1, inbuf,
        in_vinfo, map_flags);
  }

  GST_LOG_OBJECT (self, "successfully prepared ppe input vvas frame");
  return TRUE;

error:
  if (in_mem)
    gst_memory_unref (in_mem);

  return FALSE;
}

/**
 * @fn static gboolean vvas_xinfer_prepare_infer_input_frame (GstVvas_XInfer * self, GstBuffer * inbuf,
 *							      GstVideoInfo * in_vinfo, VvasVideoFrame * vvas_frame)
 * @param [in] self - handle to GstVvas_XInfer
 * @param [in] inbuf - input GstBuffer
 * @param [in] in_vinfo - input video info
 * @param [out] vvas_frame - populated values from buffer and video info
 *
 * @return TRUE on success
 *         FALSE on failure
 *
 * @brief Prepare infer frame from input buffer and video info
 * @detail This function populate the vvas frame with required values extracted
 *	  from input buffer and video info. As infer kernel copies the data to
 *	  its tensor from virtual address, so this function also populate vaddr of frame
 *
 */
static gboolean
vvas_xinfer_prepare_infer_input_frame (GstVvas_XInfer *self, GstBuffer *inbuf,
    GstVideoInfo *in_vinfo, VvasVideoFrame **vvas_frame, GstBuffer **new_inbuf)
{
  GstVvas_XInferPrivate *priv = self->priv;
  std::unique_ptr < GstCaps, decltype (&gst_caps_unref) > curr_sinkcaps (gst_pad_get_current_caps (GST_BASE_TRANSFORM (self)->sinkpad), &gst_caps_unref);       /* automatically unref caps at function exit!! */

  if (new_inbuf)
    *new_inbuf = NULL;

  int mbank_idx = -1;
  VvasContext *vvas_ctx = priv->infer->vvas_ctx;
  if (priv->infer->runtime == MLRuntime::VART &&
      priv->infer->vart_info.inp_tensor_type == vart::TensorType::HW) {

    /* If upstream did not honour our propose_allocation (e.g. filesrc /
     * multifilesrc / rawvideoparse), the buffer here is plain SW memory
     * but VART zero-copy needs an XRT BO. Materialize one. */
    GstBuffer *internal_inbuf = NULL;
    if (!vvas_xinfer_ensure_xrt_input_buffer (self, inbuf,
            XDNA_DEVICE_IDX, NULL, priv->infer->vart_info.mbank_idx,
            1 /* no stride alignment for raw tensor data */ ,
            &internal_inbuf)) {
      return FALSE;
    }
    if (internal_inbuf) {
      if (!new_inbuf) {
        /* Caller cannot take ownership; refuse rather than leak. */
        GST_ERROR_OBJECT (self,
            "internal HW input buffer created but caller did not provide ownership slot");
        gst_buffer_unref (internal_inbuf);
        return FALSE;
      }
      *new_inbuf = internal_inbuf;
      inbuf = internal_inbuf;
    }

    std::unique_ptr < GstMemory,
        decltype (&gst_memory_unref) > in_mem (gst_buffer_get_memory (inbuf, 0),
        gst_memory_unref);

    if (gst_is_vvas_memory (in_mem.get ())) {
      /* VVAS DMA memory — extract actual bank and context from allocator */
      guint mbank = gst_vvas_memory_get_mem_bank (in_mem.get ());
      if (mbank == (guint) - 1) {
        GST_ERROR_OBJECT (self,
            "failed to get memory bank for HW input tensor type");
        return FALSE;
      }
      mbank_idx = static_cast < int >(mbank);
      vvas_ctx =
          static_cast <
          VvasContext * >(gst_vvas_memory_get_vvas_ctx (in_mem.get ()));
      if (!vvas_ctx) {
        GST_ERROR_OBJECT (self, "failed to get VVAS context");
        return FALSE;
      }
    } else if (gst_is_dmabuf_memory (in_mem.get ())) {
      /* External dmabuf (e.g. v4l2src io-mode=dmabuf) — use default bank */
      mbank_idx = DEFAULT_MBANK_IDX;
    }

    /* Flush CPU cache for DMABUF-exported VVAS buffers before VART HW
     * zero-copy inference when appropriate; see
     * vvas_xinfer_sync_vvas_dmabuf_to_device_if_needed(). */
    if (!vvas_xinfer_sync_vvas_dmabuf_to_device_if_needed (self, in_mem.get ())) {
      return FALSE;
    }
  }

  GstMapFlags map_flags = static_cast < GstMapFlags >
      (GST_MAP_READ | GST_VIDEO_FRAME_MAP_FLAG_NO_REF);

  *vvas_frame =
      vvas_videoframe_from_gstbuffer (vvas_ctx, mbank_idx, inbuf, in_vinfo,
      map_flags);

#ifdef DUMP_INFER_INPUT
  {
    int ret;
    VvasReturnType vret;
    char str[100];
    VvasVideoFrameMapInfo map_info = { 0 };

    vret = vvas_video_frame_map (*vvas_frame, VVAS_DATA_MAP_READ, &map_info);

    sprintf (str, "inferinput_%d_%dx%d.bgr",
        self->priv->infer->level, map_info.width, map_info.height);

    if (self->priv->fp == NULL) {
      self->priv->fp = fopen (str, "w+");
    }

    ret =
        fwrite (map_info.planes[0].data, 1,
        map_info.width * map_info.height * 3, self->priv->fp);

    if (self->priv->fp) {
      fclose (self->priv->fp);
      self->priv->fp = NULL;
    }
    printf ("written %s infer input frame size = %d  %dx%d\n", str, ret,
        map_info.width, map_info.height);

    vret = vvas_video_frame_unmap (*vvas_frame, &map_info);
  }
#endif

  GST_LOG_OBJECT (self, "successfully prepared inference input vvas frame");
  return TRUE;
}

static int
vvas_image_process_create_frame_rect (GstVvas_XInfer *self,
    VvasVideoFrame *input[MAX_ROI],
    VvasVideoFrame *output[MAX_ROI], vvas_ms_roi *roi_data)
{
  guint chan_id;
  GstVvas_XInferPrivate *priv = self->priv;
  VvasReturnType vret;
  VvasImageProcessFrameRect src_rect = { 0 };
  VvasImageProcessFrameRect dst_rect = { 0 };
  VvasVideoInfo in_info = { 0 }, out_vinfo = {
    0
  };

  GST_DEBUG_OBJECT (self,
      "Creating frame rect for %d image process tasks", roi_data->nobj);

  priv->pre_proc->frame->input_roi.nobj = priv->pre_proc->roi.nobj;
  priv->pre_proc->frame->output_roi.nobj = priv->pre_proc->roi.nobj;

  vvas_video_frame_get_videoinfo (input[0], &in_info);

  for (chan_id = 0; chan_id < roi_data->nobj; chan_id++) {

    vvas_video_frame_get_videoinfo (output[chan_id], &out_vinfo);

    src_rect.x = roi_data->roi[chan_id].x_cord;
    src_rect.y = roi_data->roi[chan_id].y_cord;
    src_rect.width = roi_data->roi[chan_id].width;
    src_rect.height = roi_data->roi[chan_id].height;
    src_rect.frame = input[0];

    dst_rect.x = 0;
    dst_rect.y = 0;
    dst_rect.width = out_vinfo.width;
    dst_rect.height = out_vinfo.height;
    dst_rect.frame = output[chan_id];

    if (!vvas_image_process_align_rect_params (&self->priv->pre_proc->
            caps->alignment_req, in_info.fmt, &src_rect)) {
      GST_ERROR_OBJECT (self, "failed to align src_rect");
      return 0;
    }

    if (!vvas_image_process_align_rect_params (&self->priv->pre_proc->
            caps->alignment_req, out_vinfo.fmt, &dst_rect)) {
      GST_ERROR_OBJECT (self, "failed to align dst_rect");
      return 0;
    }

    /* Add the frame for processing */
    vret =
        vvas_image_process_add_frame (priv->pre_proc->core_handle->handle,
        &src_rect, &dst_rect, &priv->pre_proc->param);

    priv->pre_proc->frame->input_roi.roi[chan_id].width =
        (uint32_t) src_rect.width;
    priv->pre_proc->frame->input_roi.roi[chan_id].height =
        (uint32_t) src_rect.height;
    priv->pre_proc->frame->input_roi.roi[chan_id].x_cord =
        (uint32_t) src_rect.x;
    priv->pre_proc->frame->input_roi.roi[chan_id].y_cord =
        (uint32_t) src_rect.y;
    priv->pre_proc->frame->output_roi.roi[chan_id].width =
        (uint32_t) dst_rect.width;
    priv->pre_proc->frame->output_roi.roi[chan_id].height =
        (uint32_t) dst_rect.height;
    priv->pre_proc->frame->output_roi.roi[chan_id].x_cord =
        (uint32_t) dst_rect.x;
    priv->pre_proc->frame->output_roi.roi[chan_id].y_cord =
        (uint32_t) dst_rect.y;

    if (VVAS_IS_ERROR (vret)) {
      GST_ERROR_OBJECT (self, "failed to add frame for processing");
      return 0;
    }

    GST_DEBUG_OBJECT (self, "In width %u In Height %u", src_rect.width,
        src_rect.height);
  }

  return 1;
}

static gboolean
preprocessor_node_foreach (GNode *node, gpointer ptr)
{
  GstVvas_XInfer *self = (GstVvas_XInfer *) ptr;
  GstVvas_XInferPrivate *priv = self->priv;
  VvasInferDetection *detection = NULL;

  if (g_node_depth (node) == priv->infer->level) {
    GstInferencePrediction *pred = (GstInferencePrediction *) node->data;
    detection = (VvasInferDetection *) pred->prediction.infer_result->data;

    if (priv->infer->num_input_class_filters
        && !check_filter_label_at_node (node, (gpointer) self)) {
      GST_DEBUG_OBJECT (self,
          "Skipping inference on this node as it's class is filtered out");
      return FALSE;
    }
    /* discarding objects it's width and height are not in the range
     * with configured values  */
    if ((detection->bbox.width < priv->infer->input_obj_min_width) ||
        (detection->bbox.height < priv->infer->input_obj_min_height) ||
        (detection->bbox.width > priv->infer->input_obj_max_width) ||
        (detection->bbox.height > priv->infer->input_obj_max_height)) {
      return FALSE;
    }
    if (!priv->pre_proc->frame->
        is_ppe_required[g_node_child_position (node->parent, node)]) {
      GST_DEBUG_OBJECT (self,
          "Skipping preprocess on this node as scaling is not required");
      return FALSE;
    }

    if (priv->pre_proc->roi.nobj >= MAX_ROI) {
      GST_DEBUG_OBJECT (self, "reached max ROI "
          "supported by preprocessor i.e. %d", MAX_ROI);
      return TRUE;
    }
    GST_DEBUG_OBJECT (self, "Got node %p at level %d", node,
        priv->infer->level);
    if ((detection->bbox.width < priv->pre_proc->caps->min_width)
        || (detection->bbox.height < priv->pre_proc->caps->min_height)) {
      GST_DEBUG_OBJECT (self,
          "Width/Height of the ROI is less the minimum supported(%dx%d), discarding",
          priv->pre_proc->caps->min_width, priv->pre_proc->caps->min_height);
      return FALSE;
    }

    if (!pred->prediction.enabled) {
      return FALSE;
    }

    priv->pre_proc->roi.roi[priv->pre_proc->roi.nobj].x_cord =
        detection->bbox.x;
    priv->pre_proc->roi.roi[priv->pre_proc->roi.nobj].y_cord =
        detection->bbox.y;
    priv->pre_proc->roi.roi[priv->pre_proc->roi.nobj].width =
        detection->bbox.width;
    priv->pre_proc->roi.roi[priv->pre_proc->roi.nobj].height =
        detection->bbox.height;

    GST_DEBUG_OBJECT (self,
        "bbox : x = %d, y = %d, width = %d, height = %d",
        detection->bbox.x, detection->bbox.y,
        detection->bbox.width, detection->bbox.height);

    priv->pre_proc->roi.nobj++;
  }

  return FALSE;
}

static int32_t
vvas_xinfer_do_pre_processing (GstVvas_XInfer *self,
    VvasVideoFrame *input[MAX_ROI], VvasVideoFrame *output[MAX_ROI],
    GstBuffer *inbuf)
{
  int ret;
  VvasReturnType vret;
  GstInferenceMeta *vvas_meta = NULL;
  GstVvas_XInferPrivate *priv = self->priv;
  GNode *root;

  priv->pre_proc->roi.nobj = 0;

  vvas_meta = ((GstInferenceMeta *) gst_buffer_get_meta ((GstBuffer *)
          inbuf, gst_inference_meta_api_get_type ()));

  if (priv->infer->level != 1) {
    if (!vvas_meta) {
      GST_ERROR_OBJECT (self,
          "metadata not available to extract bbox co-ordinates");
      return 0;
    }

    root = (GNode *) vvas_meta->prediction->prediction.node;
    g_node_traverse ((GNode *) root, G_PRE_ORDER, G_TRAVERSE_ALL,
        priv->infer->level, preprocessor_node_foreach, (gpointer) self);
  } else {
    /* Input Params : in level-1 scale up/down entire frames */

    VvasVideoInfo vinfo;
    vvas_video_frame_get_videoinfo (input[0], &vinfo);
    priv->pre_proc->roi.roi[priv->pre_proc->roi.nobj].x_cord = 0;
    priv->pre_proc->roi.roi[priv->pre_proc->roi.nobj].y_cord = 0;
    priv->pre_proc->roi.roi[priv->pre_proc->roi.nobj].width = vinfo.width;
    priv->pre_proc->roi.roi[priv->pre_proc->roi.nobj].height = vinfo.height;
    priv->pre_proc->roi.nobj = 1;
  }

  /* Set frame rect */
  guint64 t0 = 0, t1 = 0;
  if (priv->infer_profiler.enabled && priv->infer_profiler.pre_proc.enabled) {
    t0 = vvas_profiler_now_us ();
  }

  ret =
      vvas_image_process_create_frame_rect (self, input, output,
      &priv->pre_proc->roi);
  if (!ret) {
    return ret;
  }

  vret = vvas_image_process_frames (priv->pre_proc->core_handle->handle);
  if (VVAS_IS_ERROR (vret)) {
    GST_ERROR_OBJECT (self, "Failed to process frames");
    return 0;
  }

  if (priv->infer_profiler.enabled && priv->infer_profiler.pre_proc.enabled) {
    t1 = vvas_profiler_now_us ();
    g_mutex_lock (&priv->infer_profiler.snap_lock);
    vvas_profiler_stats_update (&priv->infer_profiler.pre_proc,
        priv->pre_proc->roi.nobj, t1 - t0);
    g_mutex_unlock (&priv->infer_profiler.snap_lock);
  }

  return 1;
}

/**
 * @fn static gboolean vvas_xinfer_prepare_ppe_output_frame (GstVvas_XInfer * self, GstBuffer * outbuf,
 *							     GstVideoInfo * out_vinfo, VvasVideoFrame ** vvas_frame)
 * @param [in] self - Handle to GstVvas_XInfer
 * @param [in] outbuf - Gstreamer buffer from out pool
 * @param [in] out_vinfo - Required output video information
 * @param [out] vvas_frame - Handle to VvasVideoFrame
 * @return TRUE on success
 *         FALSE - GstMemory or GstVideoMeta is not available
 *
 * @brief This function populate the vvas_frame with required information extracted
 *        from outbuf and out_vinfo
 */
static gboolean
vvas_xinfer_prepare_ppe_output_frame (GstVvas_XInfer *self, GstBuffer *outbuf,
    GstVideoInfo *out_vinfo, VvasVideoFrame **vvas_frame)
{
  GstVvas_XInferPrivate *priv = self->priv;
  GstMapFlags map_flags = static_cast < GstMapFlags >
      (GST_MAP_WRITE | GST_VIDEO_FRAME_MAP_FLAG_NO_REF);;

  if (priv->infer->input_tensor_format != VVAS_VIDEO_FORMAT_UNKNOWN) {
    *vvas_frame =
        vvas_videoframe_from_gstbuffer_with_vvas_video_format (priv->
        pre_proc->vvas_ctx, priv->pre_proc->out_mem_bank, outbuf, out_vinfo,
        priv->infer->input_tensor_format, map_flags);
  } else {
    *vvas_frame =
        vvas_videoframe_from_gstbuffer (priv->pre_proc->vvas_ctx,
        priv->pre_proc->out_mem_bank, outbuf, out_vinfo, map_flags);
  }
  if (!*vvas_frame) {
    GST_ERROR_OBJECT (self,
        "failed to create vvas frame from ppe output frame");
    goto error;
  }
  GST_DEBUG_OBJECT (self, "successfully prepared output vvas frame");

  if (!priv->pre_proc->use_software) {
    GstMemory *out_mem = gst_buffer_get_memory (outbuf, 0);
    if (out_mem == NULL) {
      GST_ERROR_OBJECT (self, "failed to get memory from output buffer");
      goto error;
    }
    GST_DEBUG_OBJECT (self,
        "Setting VVAS_SYNC_FROM_DEVICE on output buffer memory");

    gst_vvas_memory_set_sync_flag (out_mem, VVAS_SYNC_FROM_DEVICE);
    gst_memory_unref (out_mem);
  }

  return TRUE;

error:
  return FALSE;
}

/**
 * @fn static gpointer vvas_xinfer_ppe_loop (gpointer data)
 * @param [in] data - Handle to GstVvas_XInfer
 * @return NULL when thread exit normally
 *
 * @brief The function to execute as the PPE thread
 * @detail The function receive the PPE input frame and do
 *         1. At level == 1
 *            Prepare output frame and send for pre-procssing
 *         2. At level > 1
 *             a. Skip frame if no prediction in previous inference level
 *             b. IF sub buffer from level 1 present and can be used, then
 *                add them to infer->batch_queue to be process by infer
 *             c. Otherwise create ppe output frame of each node at current level
 *                and send for ppe processing.
 */
static gpointer
vvas_xinfer_ppe_loop (gpointer data)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (data);
  gst_vvas_log_bridge_attach_thread (GST_OBJECT (self));
  GstVvas_XInferPrivate *priv = self->priv;
  VvasCoreModule *ppe_handle = priv->pre_proc->core_handle;
  GstVvasUsrMeta *gst_usrmeta = NULL;
  GstMetaInfo *info = NULL;
  Vvas_XInferNumSubs numSubs = { self, 0, 0 };
  /* Update state of PPE thread to running */
  priv->pre_proc->thread_state = VVAS_THREAD_RUNNING;

  /* Loop thread unless stop called on EOS or ERROR or ctrl^c */
  while (!priv->stop) {
    int ret;
    GstFlowReturn fret = GST_FLOW_OK;
    gboolean bret;
    GstInferenceMeta *parent_meta = NULL;
    guint out_frames_count = 0; /* no of output expected from PPE */
    guint oidx;
    gboolean do_ppe = FALSE;

    g_mutex_lock (&priv->pre_proc->lock);
    if (priv->pre_proc->need_data && !priv->stop) {
      /* wait for input to PPE */
      g_cond_wait (&priv->pre_proc->has_input, &priv->pre_proc->lock);
    }
    g_mutex_unlock (&priv->pre_proc->lock);

    if (priv->stop)
      goto exit;

    /* here ppe receive a frame */

    /* on EOS, send same event to Infer thread */
    if (priv->pre_proc->frame->event &&
        GST_EVENT_TYPE (priv->pre_proc->frame->event) == GST_EVENT_EOS) {
      Vvas_XInferFrame *event_frame;

      event_frame = g_slice_new0 (Vvas_XInferFrame);
      event_frame->event = priv->pre_proc->frame->event;
      event_frame->skip_processing = TRUE;

      g_mutex_lock (&priv->infer->lock);
      g_queue_push_tail (priv->infer->batch_queue, event_frame);
      g_mutex_unlock (&priv->infer->lock);
      GST_INFO_OBJECT (self, "received EOS event, push frame %p and exit",
          event_frame);
      goto exit;
    }
    if (priv->pre_proc->frame->skip_processing) {
      /** Skipping this frame as all labels are filtered out */
      GST_DEBUG_OBJECT (self, "Skipping inference on this frame");
      goto skipframe;
    }
    /* PPE get metadata at level > 1 */
    parent_meta = (GstInferenceMeta *) gst_buffer_get_meta
        (priv->pre_proc->frame->parent_buf, gst_inference_meta_api_get_type ());

    if (parent_meta && parent_meta->prediction
        && !parent_meta->prediction->prediction.enabled) {
      /** Skipping this frame as root node has enabled = FALSE */
      GST_DEBUG_OBJECT (self, "Skipping inference on this frame");
      goto skipframe;
    }

    if (priv->infer->level == 1) {
      GstBuffer *child_buf = NULL;
      GstVideoInfo *child_vinfo = NULL;
      if (parent_meta && parent_meta->prediction &&
          vvas_xinfer_is_sub_buffer_useful (self,
              parent_meta->prediction->sub_buffer)) {
        GstVideoMeta *vmeta = NULL;
        Vvas_XInferFrame *infer_frame;
        VvasVideoFrame *vvas_frame;

        child_buf = gst_buffer_ref (parent_meta->prediction->sub_buffer);
        child_vinfo = gst_video_info_new ();
        vmeta = gst_buffer_get_video_meta (child_buf);
        gst_video_info_set_format (child_vinfo, vmeta->format,
            vmeta->width, vmeta->height);
        /* use sub_buffer directly in inference stage. No need of PPE */
        out_frames_count = 0;

        infer_frame = g_slice_new0 (Vvas_XInferFrame);

        bret = vvas_xinfer_prepare_infer_input_frame (self, child_buf,
            child_vinfo, &vvas_frame, &infer_frame->internal_inbuf);
        if (!bret) {
          GST_ERROR_OBJECT (self, "Failed to prepare infer input frame");
          if (infer_frame->internal_inbuf)
            gst_buffer_unref (infer_frame->internal_inbuf);
          g_slice_free (Vvas_XInferFrame, infer_frame);
          gst_buffer_unref (child_buf);
          gst_video_info_free (child_vinfo);
          goto error;
        }

        infer_frame->parent_buf = priv->pre_proc->frame->parent_buf;
        infer_frame->parent_vinfo =
            gst_video_info_copy (priv->pre_proc->frame->parent_vinfo);
        infer_frame->last_parent_buf = TRUE;
        infer_frame->vvas_frame = vvas_frame;
        infer_frame->child_buf = child_buf;
        infer_frame->child_vinfo = gst_video_info_copy (child_vinfo);
        infer_frame->skip_processing = FALSE;
        infer_frame->use_roi_data = FALSE;
        infer_frame->tensors = NULL;

        g_mutex_lock (&priv->infer->lock);
        g_queue_push_tail (priv->infer->batch_queue, infer_frame);
        g_mutex_unlock (&priv->infer->lock);

        gst_video_info_free (child_vinfo);
        do_ppe = FALSE;

      } else {
        GstBuffer *outbuf;
        VvasVideoFrame *out_vvas_frame;

        /* first level of inference will be on full frame */
        out_frames_count = 1;
        /* acquire ppe output buffer */
        fret =
            gst_buffer_pool_acquire_buffer (priv->pre_proc->outpool, &outbuf,
            NULL);
        if (fret != GST_FLOW_OK) {
          GST_ERROR_OBJECT (self, "failed to allocate buffer from pool %p",
              priv->pre_proc->outpool);
          goto error;
        }

        GST_LOG_OBJECT (self, "acquired PPE output buffer %p", outbuf);
        /* copy GstVvasUsrMeta if any */
        gst_usrmeta =
            gst_buffer_get_vvas_usr_meta ((GstBuffer *) priv->pre_proc->
            frame->parent_buf);
        if (gst_usrmeta) {
          info = (GstMetaInfo *) ((GstMeta *) gst_usrmeta)->info;
          if (info && info->transform_func) {
            info->transform_func (outbuf, (GstMeta *) gst_usrmeta,
                priv->pre_proc->frame->parent_buf, _gst_meta_transform_copy,
                NULL);
            GST_LOG_OBJECT (self, "copy GstVvasUsrMeta %p", gst_usrmeta);
          }
        }

        if (parent_meta) {      /* level-1 has metadata already,
                                   make current inference as siblings */
          GstInferenceMeta *out_meta = NULL;

          out_meta =
              ((GstInferenceMeta *) gst_buffer_get_meta (outbuf,
                  gst_inference_meta_api_get_type ()));
          if (!out_meta) {
            GST_LOG_OBJECT (self, "add inference metadata to %p", outbuf);
            out_meta =
                (GstInferenceMeta *) gst_buffer_add_meta (outbuf,
                gst_inference_meta_get_info (), NULL);
          }

          gst_inference_prediction_unref (out_meta->prediction);
          gst_inference_prediction_ref (parent_meta->prediction);
          out_meta->prediction = parent_meta->prediction;
        }

        /* Preprocessing required, prepare output frame for ppe */
        bret =
            vvas_xinfer_prepare_ppe_output_frame (self, outbuf,
            priv->pre_proc->out_vinfo, &out_vvas_frame);
        if (!bret) {
          vvas_video_frame_free (out_vvas_frame);
          gst_buffer_unref (outbuf);
          goto error;
        }

        /* one output expected from ppe */
        priv->pre_proc->core_handle->output[0] = out_vvas_frame;
        g_queue_push_tail (priv->pre_proc->buf_queue, outbuf);
        do_ppe = TRUE;
      }
    } else {                    /* level > 1 */
      if (parent_meta && parent_meta->prediction) {
        guint sub_bufs_len = 0;

        GST_LOG_OBJECT (self, "infer_level = %d & meta data depth = %d",
            priv->infer->level,
            g_node_max_height ((GNode *) parent_meta->prediction->
                prediction.node));

        if (priv->infer->level >
            g_node_max_height ((GNode *) parent_meta->prediction->
                prediction.node)) {
          goto skipframe;
        }

        numSubs.available_buffer = 0;
        numSubs.required_buffer = 0;

        /* Check either sub buffer is useful */
        if (priv->infer->level <=
            g_node_max_height ((GNode *) parent_meta->prediction->
                prediction.node)) {
          g_node_traverse ((GNode *) parent_meta->prediction->prediction.node,
              G_PRE_ORDER, G_TRAVERSE_ALL, -1, prepare_inference_sub_buffers,
              &numSubs);
        }

        sub_bufs_len = g_queue_get_length (priv->infer->sub_buffers);
        if (numSubs.available_buffer) {
          GstVideoInfo *child_vinfo = NULL;
          GstBuffer *child_buf = NULL;

          /* use sub_buffer directly in inference stage. No need of PPE */
          out_frames_count = 0;

          child_vinfo = gst_video_info_new ();

          GST_DEBUG_OBJECT (self,
              "input buffer %p has %u in inference level %u",
              priv->pre_proc->frame->parent_buf, sub_bufs_len,
              priv->infer->level);

          for (oidx = 0; oidx < sub_bufs_len; oidx++) {
            GstVideoMeta *vmeta;
            Vvas_XInferFrame *infer_frame;
            VvasVideoFrame *vvas_frame;

            child_buf =
                gst_buffer_ref (static_cast <
                GstBuffer * >(g_queue_pop_head (priv->infer->sub_buffers)));
            vmeta = gst_buffer_get_video_meta (child_buf);
            gst_video_info_set_format (child_vinfo, vmeta->format,
                vmeta->width, vmeta->height);

            infer_frame = g_slice_new0 (Vvas_XInferFrame);

            bret = vvas_xinfer_prepare_infer_input_frame (self, child_buf,
                child_vinfo, &vvas_frame, &infer_frame->internal_inbuf);
            if (!bret) {
              GST_ERROR_OBJECT (self, "Failed to prepare infer input frame");
              if (infer_frame->internal_inbuf)
                gst_buffer_unref (infer_frame->internal_inbuf);
              g_slice_free1 (sizeof (Vvas_XInferFrame), infer_frame);
              gst_buffer_unref (child_buf);
              gst_video_info_free (child_vinfo);
              goto error;
            }

            infer_frame->parent_buf = priv->pre_proc->frame->parent_buf;
            infer_frame->parent_vinfo =
                gst_video_info_copy (priv->pre_proc->frame->parent_vinfo);
            infer_frame->last_parent_buf =
                (oidx ==
                (numSubs.available_buffer + numSubs.required_buffer -
                    1)) ? TRUE : FALSE;
            infer_frame->vvas_frame = vvas_frame;
            infer_frame->child_buf = child_buf;
            infer_frame->child_vinfo = gst_video_info_copy (child_vinfo);
            infer_frame->skip_processing = FALSE;
            infer_frame->use_roi_data = FALSE;
            infer_frame->tensors = NULL;

            g_mutex_lock (&priv->infer->lock);
            /* add frame for infer processing */
            g_queue_push_tail (priv->infer->batch_queue, infer_frame);
            g_mutex_unlock (&priv->infer->lock);
          }
          gst_video_info_free (child_vinfo);
          do_ppe = FALSE;
        }
        if (numSubs.required_buffer) {
          priv->pre_proc->nframes_in_level = 0;

          /* ppe_kernel->output array will be filled on node traversal */
          g_node_traverse ((GNode *) parent_meta->prediction->prediction.node,
              G_PRE_ORDER, G_TRAVERSE_ALL, priv->infer->level,
              prepare_ppe_outbuf_at_level, self);
          if (priv->is_error)
            goto error;

          GST_DEBUG_OBJECT (self, "number of nodes at level-%d = %d",
              priv->infer->level, priv->pre_proc->nframes_in_level);

          out_frames_count = priv->pre_proc->nframes_in_level;
          if (!out_frames_count)
            goto skipframe;
          do_ppe = TRUE;
        }
      } else {
        /* no level-1 inference available, skip this frame */
        Vvas_XInferFrame *infer_frame;

      skipframe:
        out_frames_count = 0;
        do_ppe = FALSE;

        infer_frame = g_slice_new0 (Vvas_XInferFrame);
        infer_frame->parent_buf = priv->pre_proc->frame->parent_buf;
        infer_frame->parent_vinfo =
            gst_video_info_copy (priv->pre_proc->frame->parent_vinfo);
        infer_frame->last_parent_buf = TRUE;
        infer_frame->vvas_frame = NULL;
        infer_frame->child_buf = NULL;
        infer_frame->child_vinfo = NULL;
        infer_frame->skip_processing = TRUE;
        infer_frame->use_roi_data = FALSE;
        infer_frame->tensors = NULL;

        GST_DEBUG_OBJECT (self, "skipping buffer %p in ppe & inference stage",
            infer_frame->parent_buf);
        g_mutex_lock (&priv->infer->lock);
        if (priv->stop) {
          g_slice_free1 (sizeof (Vvas_XInferFrame), infer_frame);
          g_mutex_unlock (&priv->infer->lock);
          goto exit;
        }
        /* send input frame to inference thread */
        g_queue_push_tail (priv->infer->batch_queue, infer_frame);

        if (priv->infer->batch_size ==
            g_queue_get_length (priv->infer->batch_queue)) {
          g_cond_signal (&priv->infer->cond);
        }
        g_mutex_unlock (&priv->infer->lock);
      }
    }

    if (do_ppe) {
      /* Add parent frame as input */
      priv->pre_proc->core_handle->input[0] = priv->pre_proc->frame->vvas_frame;

      /* Run PPE */
      ret =
          vvas_xinfer_do_pre_processing (self,
          priv->pre_proc->core_handle->input,
          priv->pre_proc->core_handle->output,
          priv->pre_proc->frame->child_buf);
      if (!ret) {
        GST_ERROR_OBJECT (self, "kernel start failed");
        goto error;
      }

      GST_DEBUG_OBJECT (self, "completed preprocessing of %d output frames",
          out_frames_count);

      for (oidx = 0; oidx < out_frames_count; oidx++) {
        Vvas_XInferFrame *infer_frame;
        GstBuffer *inbuf;
        /* input to inference stage */
        VvasVideoFrame *in_vvas_frame =
            priv->pre_proc->core_handle->output[oidx];

        infer_frame = g_slice_new0 (Vvas_XInferFrame);
        inbuf =
            static_cast <
            GstBuffer * >(g_queue_pop_head (priv->pre_proc->buf_queue));

        /* output of PPE will be input of inference stage */
        /* vvas_xinfer_prepare_infer_input_frame */
        /* not needed as we have VvasVideoFrame from PPE */

        infer_frame->parent_buf = priv->pre_proc->frame->parent_buf;
        infer_frame->parent_vinfo =
            gst_video_info_copy (priv->pre_proc->frame->parent_vinfo);
        infer_frame->last_parent_buf =
            (oidx == (out_frames_count - 1)) ? TRUE : FALSE;
        infer_frame->vvas_frame = in_vvas_frame;
        infer_frame->child_buf = inbuf;
        infer_frame->child_vinfo =
            gst_video_info_copy (priv->pre_proc->out_vinfo);
        infer_frame->skip_processing = FALSE;
        infer_frame->input_roi.nobj = 1;
        infer_frame->output_roi.nobj = 1;
        infer_frame->input_roi.roi[0] =
            priv->pre_proc->frame->input_roi.roi[oidx];
        infer_frame->output_roi.roi[0] =
            priv->pre_proc->frame->output_roi.roi[oidx];
        infer_frame->use_roi_data = TRUE;
        infer_frame->tensors = NULL;

        /* send input frame to inference thread */
        g_mutex_lock (&priv->infer->lock);
        GST_LOG_OBJECT (self, "pushing child_buf %p in infer_frame %p to queue",
            infer_frame->child_buf, infer_frame);
        g_queue_push_tail (priv->infer->batch_queue, infer_frame);
        g_mutex_unlock (&priv->infer->lock);
      }
    }

    /* on low latency each frame is send for processing without filling the
     * batch */
    g_mutex_lock (&priv->infer->lock);
    if ((priv->infer->level > 1 && priv->infer->low_latency) ||
        (g_queue_get_length (priv->infer->batch_queue) >=
            priv->infer->batch_size)) {
      /* Signal infer thread that frame is available */
      g_cond_signal (&priv->infer->cond);
    }
    g_mutex_unlock (&priv->infer->lock);

    /* free PPE input members. NULL each pointer after free so the SIGINT
     * defensive cleanup in gst_vvas_xinfer_stop() does not re-free a stale
     * pointer when the thread is interrupted between iterations. */
    if (priv->pre_proc->frame->child_buf) {
      gst_buffer_unref (priv->pre_proc->frame->child_buf);
      priv->pre_proc->frame->child_buf = NULL;
    }
    if (priv->pre_proc->frame->child_vinfo) {
      gst_video_info_free (priv->pre_proc->frame->child_vinfo);
      priv->pre_proc->frame->child_vinfo = NULL;
    }
    if (priv->pre_proc->frame->vvas_frame) {
      vvas_video_frame_free (priv->pre_proc->frame->vvas_frame);
      priv->pre_proc->frame->vvas_frame = NULL;
    }
    if (priv->pre_proc->frame->parent_vinfo) {
      gst_video_info_free (priv->pre_proc->frame->parent_vinfo);
      priv->pre_proc->frame->parent_vinfo = NULL;
    }

    memset (ppe_handle->input, 0x0, sizeof (VvasVideoFrame *) * MAX_ROI);
    memset (ppe_handle->output, 0x0, sizeof (VvasVideoFrame *) * MAX_ROI);

    g_mutex_lock (&priv->pre_proc->lock);
    priv->pre_proc->need_data = TRUE;
    g_cond_signal (&priv->pre_proc->need_input);
    g_mutex_unlock (&priv->pre_proc->lock);
  }

exit:
  /* Inform Infer thread */
  priv->pre_proc->thread_state = VVAS_THREAD_EXITED;

  g_mutex_lock (&priv->infer->lock);
  g_cond_signal (&priv->infer->cond);
  g_mutex_unlock (&priv->infer->lock);
  return NULL;

error:
  /* Drain any GstBuffers pushed into buf_queue but not yet forwarded to
   * the infer thread. Without this they are leaked when g_queue_free()
   * is called later without element-level unrefs. */
  if (priv->pre_proc->buf_queue) {
    GstBuffer *leaked_buf = NULL;
    while ((leaked_buf =
            static_cast <
            GstBuffer * >(g_queue_pop_head (priv->pre_proc->buf_queue))) !=
        NULL) {
      gst_buffer_unref (leaked_buf);
    }
  }

  g_mutex_lock (&priv->pre_proc->lock);
  priv->stop = TRUE;
  g_cond_signal (&priv->pre_proc->need_input);
  g_mutex_unlock (&priv->pre_proc->lock);

  GST_ELEMENT_ERROR (self, STREAM, FAILED, ("failed to process frame in PPE."),
      ("failed to process frame in PPE."));
  priv->last_fret = GST_FLOW_ERROR;
  goto exit;
}

/**
 * @fn static void update_child_bbox (GNode * node, gpointer data)
 * @param [in] node - a node in tree
 * @param [inout] data - used to typecast to Vvas_XInferNodeInfo
 * @return None
 *
 * @brief Transform child prediction coordinate as per parent coordinates
 */
static void
update_child_bbox (GNode *node, gpointer data)
{
  Vvas_XInferNodeInfo *node_info = (Vvas_XInferNodeInfo *) data;
  gdouble hfactor, vfactor;
  gint fw = 1, fh = 1, tw, th;
  int32_t xOffset = 0, yOffset = 0;
  GstInferencePrediction *cur_prediction =
      (GstInferencePrediction *) node->data;
  GstInferencePrediction *parent_prediction =
      (GstInferencePrediction *) node->parent->data;
  VvasInferDetection *parent_detection = NULL;
  VvasInferDetection *cur_detection = NULL;
  VvasInferScaleInfo scale_info;

  /* Aligned coordinates from Image Process Library */
  int tx = 0;
  int ty = 0;

  /* Actual x & y from Parent box */
  int bx = 0;
  int by = 0;

  if (cur_prediction->prediction.scaled_to_root)
    return;

  if (node_info->use_roi_data) {
    tw = node_info->input_roi.roi[0].width;
    th = node_info->input_roi.roi[0].height;
    fw = node_info->output_roi.roi[0].width;
    fh = node_info->output_roi.roi[0].height;

    tx = node_info->input_roi.roi[0].x_cord;
    ty = node_info->input_roi.roi[0].y_cord;

    if ((parent_prediction->prediction.infer_result) &&
        (parent_prediction->prediction.infer_result->infer_result_type ==
            VVAS_INFER_RESULT_DETECTION)) {
      parent_detection =
          (VvasInferDetection *) parent_prediction->prediction.
          infer_result->data;

      bx = parent_detection->bbox.x;
      by = parent_detection->bbox.y;
    }
  } else {
    tw = GST_VIDEO_INFO_WIDTH (node_info->parent_vinfo);
    th = GST_VIDEO_INFO_HEIGHT (node_info->parent_vinfo);
    fw = GST_VIDEO_INFO_WIDTH (node_info->child_vinfo);
    fh = GST_VIDEO_INFO_HEIGHT (node_info->child_vinfo);
  }

  hfactor = tw * 1.0 / fw;
  vfactor = th * 1.0 / fh;

  scale_info.from_width = fw;
  scale_info.from_height = fh;
  scale_info.to_width = tw;
  scale_info.to_height = th;

  if (node_info->use_roi_data) {
    if ((parent_prediction->prediction.infer_result) &&
        (parent_prediction->prediction.infer_result->infer_result_type ==
            VVAS_INFER_RESULT_NONE)) {
      xOffset = node_info->input_roi.roi[0].x_cord;
      yOffset = node_info->input_roi.roi[0].y_cord;
    }
    xOffset -=
        (int32_t) ((double) node_info->output_roi.roi[0].x_cord * hfactor);
    yOffset -=
        (int32_t) ((double) node_info->output_roi.roi[0].y_cord * vfactor);

    /* Coordinate are modified by library if it is not
     * aligned, this will result in bounding box
     * shifting towards left. thus we need to offset
     * to delta, where delta is the difference between
     * actual bounding box and aligned box coordinates.
     * Values of bx and by remain zero in the case
     * of first level inference as there is no bounding
     * box so this offset only need to add in case of
     * bounding box.
     */
    if ((parent_prediction->prediction.infer_result) &&
        (parent_prediction->prediction.infer_result->infer_result_type ==
            VVAS_INFER_RESULT_DETECTION)) {
      xOffset -= (bx - tx);
      yOffset -= (by - ty);
    }
  }

  if ((parent_prediction->prediction.infer_result) &&
      (parent_prediction->prediction.infer_result->infer_result_type ==
          VVAS_INFER_RESULT_DETECTION)) {
    parent_detection =
        (VvasInferDetection *) parent_prediction->prediction.infer_result->data;
    scale_info.x = parent_detection->bbox.x;
    scale_info.y = parent_detection->bbox.y;
  } else {
    scale_info.x = 0;
    scale_info.y = 0;
  }
  scale_info.xOffset = xOffset;
  scale_info.yOffset = yOffset;

  /* Call transform of the respective model category */
  if (cur_prediction->prediction.infer_result &&
      cur_prediction->prediction.infer_result->data &&
      cur_prediction->prediction.infer_result->transform) {
    cur_prediction->prediction.infer_result->
        transform (cur_prediction->prediction.infer_result->data, &scale_info);
  }

  if (((cur_prediction->prediction.infer_result) &&
          (cur_prediction->prediction.infer_result->infer_result_type ==
              VVAS_INFER_RESULT_DETECTION))) {
    cur_detection =
        (VvasInferDetection *) cur_prediction->prediction.infer_result->data;

    /* check if updated coordinates are extended beyong original image
     * and clip it to make within image boudary. */
    if (GST_VIDEO_INFO_WIDTH (node_info->parent_vinfo) <
        (gint) (cur_detection->bbox.width + cur_detection->bbox.x)) {
      cur_detection->bbox.width -=
          (cur_detection->bbox.width +
          cur_detection->bbox.x -
          GST_VIDEO_INFO_WIDTH (node_info->parent_vinfo));
    }
    if (GST_VIDEO_INFO_HEIGHT (node_info->parent_vinfo) <
        (gint) (cur_detection->bbox.height + cur_detection->bbox.y)) {
      cur_detection->bbox.height -=
          (cur_detection->bbox.height +
          cur_detection->bbox.y -
          GST_VIDEO_INFO_HEIGHT (node_info->parent_vinfo));
    }
  }

  cur_prediction->prediction.scaled_to_root = TRUE;

  if (g_node_n_children (node)) {
    /* This child node has also its children
     * scale its children to match with parent,
     * Generally, each instance of infer attaches and scales single level
     * of inference metadata, but in case of MultiModels or models like
     * retinaface attaches more than one level of metadata in single
     * instance of vvas_xinfer, and as g_node_children_foreach doesn’t
     * descend beneath the child nodes, making a recursive call here to the
     * child of current node, this will go till the leaf node.
     * Passing the same node_info to all the nodes because in MultiModel
     * case output metadata will be in respect to the input frame only.
     */
    g_node_children_foreach (node, G_TRAVERSE_ALL,
        update_child_bbox, node_info);
  }
}

/**
 * @fn static gboolean vvas_xinfer_add_metadata_at_level_1 (GstVvas_XInfer * self, GstBuffer * parent_buf,
 *							    GstVideoInfo * parent_vinfo, GstBuffer * infer_inbuf)
 * @param [in] self - Handle to GstVvas_XInfer
 * @param [in] parent_buf - Parent buffer
 * @param [in] parent_vinfo - Video info of parent buffer
 * @param [in] infer_inbuf - Buffer on which inference/prediction happened
 *
 * @return TRUE on success
 *         FALSE on failure
 *
 * @brief Add full frame prediction metadata to parent
 */
static gboolean
vvas_xinfer_add_metadata_at_level_1 (GstVvas_XInfer *self,
    GstBuffer *parent_buf, GstVideoInfo *parent_vinfo, GstBuffer *infer_inbuf)
{
  GstInferenceMeta *parent_meta = NULL;
  GstInferenceMeta *child_meta = NULL;

  parent_meta = (GstInferenceMeta *) gst_buffer_get_meta (parent_buf,
      gst_inference_meta_api_get_type ());

  if (parent_buf != infer_inbuf) {
    child_meta = (GstInferenceMeta *) gst_buffer_add_meta (infer_inbuf,
        gst_inference_meta_get_info (), NULL);
    if (!child_meta) {
      GST_ERROR_OBJECT (self, "failed to add metadata to buffer %p",
          infer_inbuf);
      return FALSE;
    }

    if (parent_meta) {          /* assign parent prediction to inference */
      gst_inference_prediction_unref (child_meta->prediction);
      child_meta->prediction =
          gst_inference_prediction_ref (parent_meta->prediction);
    }

    if (self->priv->infer->attach_ppebuf) {
      if (child_meta->prediction->sub_buffer != infer_inbuf) {
        if (child_meta->prediction->sub_buffer)
          gst_buffer_unref (child_meta->prediction->sub_buffer);
        GST_LOG_OBJECT (self, "attaching %p as sub_buffer", infer_inbuf);
        child_meta->prediction->sub_buffer = gst_buffer_ref (infer_inbuf);
      } else {
        gst_buffer_ref (infer_inbuf);
      }
    }
  } else {
    /* parent buffer as inference buffer i.e. No PPE */
    if (!parent_meta) {
      child_meta = (GstInferenceMeta *) gst_buffer_add_meta (infer_inbuf,
          gst_inference_meta_get_info (), NULL);
      if (!child_meta) {
        GST_ERROR_OBJECT (self, "failed to add metadata to buffer %p",
            infer_inbuf);
        return FALSE;
      }

      child_meta->prediction->prediction.width =
          GST_VIDEO_INFO_WIDTH (parent_vinfo);
      child_meta->prediction->prediction.height =
          GST_VIDEO_INFO_HEIGHT (parent_vinfo);
    } else {
      /* no need to add metadata as it is already present */
    }
  }

  return TRUE;
}

/**
 * @fn static void construct_gst_inference_tree (GstInferencePrediction *root, VvasInferPrediction *core_pred, gboolean attach_tensors, VvasList *tensors_list)
 * @param [in] root - The prediction node to which current inference nodes should be added.
 * @param [in] core_pred - Prediction tree returned from core library.
 * @param [in] attach_tensors - Whether or not to attach tensors.
 * @param [in] tensors_list - List of tensors returned from core.
 * @return void
 *
 * @brief The function constructs GstInferencePrediction tree from the VvasInferPrediction received from core.
 */
static void
construct_gst_inference_tree (GstInferencePrediction *root,
    VvasInferPrediction *core_pred, gboolean attach_tensors,
    VvasList *tensors_list)
{
  VvasList *iter = NULL, *pred_nodes = NULL;

  /* This flag will only be valid when model-class is RAWTENSOR
   * Will be FALSE if any other model class is set
   */
  if (attach_tensors) {
    root->prediction.tensors =
        vvas_list_concat (root->prediction.tensors, tensors_list);
  }

  pred_nodes = vvas_inferprediction_get_nodes (core_pred);
  for (iter = pred_nodes; iter != NULL; iter = iter->next) {
    VvasInferPrediction *child = (VvasInferPrediction *) iter->data;
    GstInferencePrediction *gst_subtree = NULL;

    gst_subtree = gstinfer_from_vvas_infer (child);
    /* Will reach here if post-processing is enabled in case of RAWTENSOR */
    if (attach_tensors) {
      gst_subtree->prediction.tb_ref =
          tensor_buf_ref ((TensorBuf *) tensors_list->data);
    }
    gst_inference_prediction_append (root, gst_subtree);
  }

  if (pred_nodes) {
    vvas_list_free (pred_nodes);
  }
}

static
    std::optional <
    std::vector <
    std::vector <
    vart::NpuTensor >>>
priv_create_vart_input_sw (std::shared_ptr < vart::Runner > runner,
    vector < VvasVideoFrameMapInfo > &mapped_inputs, guint batch_size)
{
  std::vector < std::vector < vart::NpuTensor >> input_tensors;

  /* For now infer plugin only supports models with one input so there
   *  is not loop over num inputs.
   */
  GST_CAT_DEBUG (GST_CAT_DEFAULT, "Vart Creating SW Inputs");
  for (guint b = 0; b < batch_size; b++) {
    std::vector < vart::NpuTensor > input;
    try {
      const auto & tensor_info =
          runner->get_tensors_info (vart::TensorDirection::INPUT,
          vart::TensorType::CPU)[0];
      auto tensor = vart::NpuTensor (tensor_info,
          reinterpret_cast < void *>(mapped_inputs[b].planes[0].data),
          vart::MemoryType::USER_POINTER_NON_CMA);
      input.push_back (std::move (tensor));
    } catch (const std::runtime_error & e)
    {
      GST_ERROR ("Failed to create NpuTensor: %s", e.what ());
      return std::nullopt;
    }
    input_tensors.push_back (std::move (input));
  }
  return input_tensors;
}

static
    std::optional <
    std::vector <
    std::vector <
    vart::NpuTensor >>>
priv_create_vart_input_hw (std::shared_ptr < vart::Runner > runner,
    vector < VvasVideoFrame *>&inp_vvas_frame, guint batch_size)
{
  std::vector < std::vector < vart::NpuTensor >> input_tensors;

  /* For now infer plugin only supports models with one input so there
   *  is not loop over num inputs.
   */
  GST_CAT_DEBUG (GST_CAT_DEFAULT, "Vart Creating HW Inputs");
  for (guint b = 0; b < batch_size; b++) {
    std::vector < vart::NpuTensor > input;
    try {
      const auto & tensor_info =
          runner->get_tensors_info (vart::TensorDirection::INPUT,
          vart::TensorType::HW)[0];
      auto tensor = vart::NpuTensor (tensor_info,
          vvas_video_frame_get_bo (inp_vvas_frame[b]),
          vart::MemoryType::XRT_BO);
      input.push_back (std::move (tensor));
    } catch (const std::runtime_error & e)
    {
      GST_ERROR ("Failed to create NpuTensor: %s", e.what ());
      return std::nullopt;
    }
    input_tensors.push_back (std::move (input));
  }
  return input_tensors;
}

static
    std::optional <
    std::vector <
    std::vector <
    vart::NpuTensor >>>
priv_create_vart_output_sw (std::shared_ptr < vart::Runner > runner,
    vector < vector < VvasMemoryMapInfo >> &mapped_outputs,
    guint batch_size, size_t num_out_tensors)
{
  GST_CAT_DEBUG (GST_CAT_DEFAULT, "Vart Creating SW Outputs");
  std::vector < std::vector < vart::NpuTensor >> output_tensors;
  for (guint b = 0; b < batch_size; b++) {
    std::vector < vart::NpuTensor > output;
    for (size_t tensor_nu = 0; tensor_nu < num_out_tensors; tensor_nu++) {
      try {
        const auto & tensor_info =
            runner->get_tensors_info (vart::TensorDirection::OUTPUT,
            vart::TensorType::CPU)[tensor_nu];
        auto tensor = vart::NpuTensor (tensor_info,
            reinterpret_cast < void *>(mapped_outputs[b][tensor_nu].data),
            vart::MemoryType::USER_POINTER_NON_CMA);
        output.push_back (std::move (tensor));
      } catch (const std::runtime_error & e)
      {
        GST_ERROR ("Failed to create NpuTensor: %s", e.what ());
        return std::nullopt;
      }
    }
    output_tensors.push_back (std::move (output));
  }
  return output_tensors;
}

static
    std::optional <
    std::vector <
    std::vector <
    vart::NpuTensor >>>
priv_create_vart_output_hw (std::shared_ptr < vart::Runner > runner,
    vector < vector < VvasMemory *>>&out_vvas_mem,
    guint batch_size, size_t num_out_tensors)
{
  GST_CAT_DEBUG (GST_CAT_DEFAULT, "Vart Creating HW Outputs");
  std::vector < std::vector < vart::NpuTensor >> output_tensors;
  for (guint b = 0; b < batch_size; b++) {
    std::vector < vart::NpuTensor > output;
    for (size_t tensor_nu = 0; tensor_nu < num_out_tensors; tensor_nu++) {
      try {
        const auto & tensor_info =
            runner->get_tensors_info (vart::TensorDirection::OUTPUT,
            vart::TensorType::HW)[tensor_nu];
        auto tensor = vart::NpuTensor (tensor_info,
            vvas_memory_get_bo (out_vvas_mem[b][tensor_nu]),
            vart::MemoryType::XRT_BO);
        output.push_back (std::move (tensor));
      } catch (const std::runtime_error & e)
      {
        GST_ERROR ("Failed to create NpuTensor: %s", e.what ());
        return std::nullopt;
      }
    }
    output_tensors.push_back (std::move (output));
  }
  return output_tensors;
}

static gboolean
priv_run_vart_inference (std::shared_ptr < vart::Runner > runner,
    std::vector < std::vector < vart::NpuTensor >> &inputs,
    std::vector < std::vector < vart::NpuTensor >> &outputs)
{
  GST_CAT_DEBUG (GST_CAT_DEFAULT, "Running model inference using VART");

  try {
    auto ret = runner->execute (inputs, outputs);
    if (ret != vart::StatusCode::SUCCESS) {
      GST_ERROR ("VART inference failed with status: %d",
          static_cast < int >(ret));
      return FALSE;
    }
  }
  catch (const std::exception & e)
  {
    GST_ERROR ("VART Run() encountered an error: %s", e.what ());
    return FALSE;
  }

  return TRUE;
}


static
    std::optional <
    std::vector <
    Ort::Value >>
priv_create_onnx_input (GstVvas_XInferPrivate *priv,
    Ort::MemoryInfo & memory_info,
    vector < VvasVideoFrameMapInfo > &mapped_inputs, guint batch_size)
{
  std::vector < Ort::Value > input_tensors;
  for (guint b = 0; b < batch_size; b++) {
    try {
      auto tensor = Ort::Value::CreateTensor (memory_info,
          reinterpret_cast < void *>(mapped_inputs[b].planes[0].data),
          priv->infer->model_config.in_tensors[0].size,
          priv->infer->model_config.input_shapes[0].data (),
          priv->infer->model_config.input_shapes[0].size (),
          get_ort_tensor_data_type (priv->infer->model_config.
              in_tensors[0].data_type));
      if (!tensor.IsTensor ()) {
        GST_ERROR ("Failed to create input tensor");
        return std::nullopt;
      }
      input_tensors.push_back (std::move (tensor));
    }
    catch (const Ort::Exception & exception)
    {
      GST_ERROR ("OnnxRuntime CreateTensor() encountered an error: %s",
          exception.what ());
      return std::nullopt;
    }
  }
  return input_tensors;
}

static
    std::optional <
    std::vector <
    Ort::Value >>
priv_create_onnx_output (GstVvas_XInferPrivate *priv,
    Ort::MemoryInfo & memory_info,
    vector < vector < VvasMemoryMapInfo >> &mapped_outputs, guint batch_size,
    int num_out_tensors)
{
  std::vector < Ort::Value > output_tensors;
  for (guint b = 0; b < batch_size; b++) {
    for (gint tensor_nu = 0; tensor_nu < num_out_tensors; tensor_nu++) {
      try {
        Ort::Value tensor = Ort::Value::CreateTensor (memory_info,
            reinterpret_cast < void *>(mapped_outputs[b][tensor_nu].data),
            priv->infer->model_config.out_tensors[tensor_nu].size,
            priv->infer->model_config.output_shapes[tensor_nu].data (),
            priv->infer->model_config.output_shapes[tensor_nu].size (),
            get_ort_tensor_data_type (priv->infer->
                model_config.out_tensors[tensor_nu].data_type));
        if (!tensor.IsTensor ()) {
          GST_ERROR ("Failed to create output tensor");
          return std::nullopt;
        }
        output_tensors.push_back (std::move (tensor));
      }
      catch (const Ort::Exception & exception)
      {
        GST_ERROR ("OnnxRuntime CreateTensor() encountered an error: %s",
            exception.what ());
        return std::nullopt;
      }
    }
  }
  return output_tensors;
}

static gboolean
priv_run_onnx_inference (GstVvas_XInferPrivate *priv,
    std::vector < Ort::Value > &inputs, std::vector < Ort::Value > &outputs)
{
  GST_DEBUG ("Running model inference using ONNX");
  guint64 t0 = 0, t1 = 0;
  if (priv->infer_profiler.enabled) {
    t0 = vvas_profiler_now_us ();
  }

  try {
    priv->infer->ort_info.session->Run (Ort::RunOptions {
        nullptr}
        , priv->infer->model_config.input_names.data (),
        inputs.data (), priv->infer->model_config.num_in_tensors,
        priv->infer->model_config.output_names.data (), outputs.data (),
        priv->infer->model_config.num_out_tensors);
  }
  catch (const Ort::Exception & exception)
  {
    GST_ERROR ("OnnxRuntime Run() encountered an error: %s", exception.what ());
    return FALSE;
  }

  if (priv->infer_profiler.enabled) {
    t1 = vvas_profiler_now_us ();
    g_mutex_lock (&priv->infer_profiler.snap_lock);
    vvas_profiler_stats_update (&priv->infer_profiler.infer,
        priv->infer->batch_size, t1 - t0);
    g_mutex_unlock (&priv->infer_profiler.snap_lock);
  }

  return TRUE;
}

static void vvas_xinfer_free_infer_frame (Vvas_XInferFrame * infer_frame);

/**
 * @fn static void vvas_xinfer_enqueue_group_to_postprocess
 * @param [in] self - Handle to GstVvas_XInfer
 * @param [in] ctx - Job context whose group of frames is to be forwarded
 *
 * @brief Forwards a completed inference group to the Post Process thread.
 *        For async VART, callbacks are delivered in submission order so this
 *        also preserves output ordering. When Post Process is disabled, or the
 *        pipeline is stopping / the Post Process thread is not running, the
 *        group's frames (and any acquired tensors) are freed here instead so
 *        nothing is leaked. Does not delete ctx.
 */
static void
vvas_xinfer_enqueue_group_to_postprocess (GstVvas_XInfer * self,
    InferJobContext * ctx)
{
  GstVvas_XInferPrivate *priv = self->priv;
  auto free_group = [&] () {
    for (auto * frame : ctx->group) {
      if (!frame)
        continue;
      if (frame->tensors) {
        if (priv->post_proc->tensor_pool)
          priv->post_proc->tensor_pool->release_memories (*frame->tensors);
        delete frame->tensors;
        frame->tensors = nullptr;
      }
      vvas_xinfer_free_infer_frame (frame);
    }
  };

  if (!priv->post_proc->enabled) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED,
        ("Postprocessing is required for vvas_xinfer"),
        ("A successfully created vvas_xinfer instance must have "
         "postprocessing enabled"));
    priv->last_fret = GST_FLOW_ERROR;
    free_group ();
    return;
  }

  g_mutex_lock (&priv->post_proc->lock);

  if (!priv->stop && (VVAS_THREAD_RUNNING == priv->post_proc->thread_state)) {
    while (!priv->stop &&
        (VVAS_THREAD_RUNNING == priv->post_proc->thread_state) &&
        (priv->post_proc->queue_length -
            g_queue_get_length (priv->post_proc->queue)) < ctx->group.size ()) {
      GST_DEBUG_OBJECT (self, "Waiting for free space in Post Process Queue");
      g_cond_wait (&priv->post_proc->cond, &priv->post_proc->lock);
      GST_DEBUG_OBJECT (self, "Post Process queue has spaces now");
    }

    if (!priv->stop && (VVAS_THREAD_RUNNING == priv->post_proc->thread_state)) {
      for (auto * frame : ctx->group) {
        GST_DEBUG_OBJECT (self, "Pushing frame %p, parent_buf: %p to Post Process thread",
            frame, frame ? frame->parent_buf : nullptr);
        g_queue_push_tail (priv->post_proc->queue, frame);
      }
      GST_DEBUG_OBJECT (self, "Signaling PostProcessing thread");
      g_cond_signal (&priv->post_proc->cond);
      g_mutex_unlock (&priv->post_proc->lock);
    } else {
      g_mutex_unlock (&priv->post_proc->lock);
      /* Stop raised or Post Process stopped while waiting: free frames. */
      free_group ();
    }
  } else {
    g_mutex_unlock (&priv->post_proc->lock);
    /* Stopping or Post Process not running: free frames to avoid leaks. */
    free_group ();
  }
}

/**
 * @fn static void vvas_xinfer_unmap_job_io
 * @param [in] self - Handle to GstVvas_XInfer
 * @param [in] ctx - Job context holding mapped input frames and output tensors
 *
 * @brief Unmaps the input frames and output tensor memories that were mapped
 *        for an inference submission. HW (zero-copy) tensors are not mapped and
 *        hence not unmapped.
 */
static void
vvas_xinfer_unmap_job_io (GstVvas_XInfer * self, InferJobContext * ctx)
{
  GstVvas_XInferPrivate *priv = self->priv;

  for (guint b = 0; b < ctx->batch.size (); b++) {
    /* SW-mapped output tensors (mapped only when output tensor type is CPU) */
    if (!ctx->hw_output) {
      priv->post_proc->tensor_pool->unmap_memories (*ctx->batch[b]->tensors,
          ctx->mapped_outputs[b]);
    }
    /* SW-mapped input frames (mapped only when input tensor type is CPU) */
    if (!ctx->hw_input) {
      auto vret = vvas_video_frame_unmap (ctx->batch[b]->vvas_frame,
          &ctx->mapped_inputs[b]);
      if (VVAS_RET_SUCCESS != vret) {
        GST_ERROR_OBJECT (self, "couldn't unmap input frame: %u", b);
      }
    }
  }
}

/**
 * @fn static void vvas_xinfer_vart_infer_finalize
 * @param [in] self - Handle to GstVvas_XInfer
 * @param [in] ctx - Job context for the completed VART inference
 * @param [in] jh - Completion status of the inference job
 *
 * @brief Finalizes a VART inference submission: updates profiling, unmaps IO,
 *        releases backend tensor wrappers and forwards the group to Post
 *        Process. Used both as the execute_async completion callback and as the
 *        inline finalize for synchronous VART. Deletes ctx. Does NOT touch
 *        async_in_flight (that bookkeeping is handled by the caller/callback).
 */
static void
vvas_xinfer_vart_infer_finalize (GstVvas_XInfer * self, InferJobContext * ctx,
    const vart::JobHandle & jh)
{
  GstVvas_XInferPrivate *priv = self->priv;

  if (jh.status != vart::StatusCode::SUCCESS) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED,
        ("failed to process frame in inference."),
        ("VART inference failed with status: %d", (int) jh.status));
    priv->last_fret = GST_FLOW_ERROR;
    priv->stop = TRUE;
  } else if (priv->infer_profiler.enabled) {
    guint64 t1 = vvas_profiler_now_us ();
    g_mutex_lock (&priv->infer_profiler.snap_lock);
    vvas_profiler_stats_update (&priv->infer_profiler.infer,
        ctx->batch.size (), t1 - ctx->t0_us);
    g_mutex_unlock (&priv->infer_profiler.snap_lock);
  }

  vvas_xinfer_unmap_job_io (self, ctx);

  /* Release backend tensor wrappers. The output tensor memory itself is
   * released back to the pool by the Post Process thread. */
  ctx->vart_in.reset ();
  ctx->vart_out.reset ();

  vvas_xinfer_enqueue_group_to_postprocess (self, ctx);

  delete ctx;
}

/**
 * @fn static void vvas_xinfer_onnxrt_infer_finalize
 * @param [in] self - Handle to GstVvas_XInfer
 * @param [in] ctx - Job context for the completed ONNX inference
 *
 * @brief Finalizes an ONNX inference submission. ONNX inference (and its
 *        profiling) runs synchronously before this call, so this only unmaps IO
 *        and forwards the group to Post Process. Deletes ctx.
 */
static void
vvas_xinfer_onnxrt_infer_finalize (GstVvas_XInfer * self, InferJobContext * ctx)
{
  vvas_xinfer_unmap_job_io (self, ctx);
  vvas_xinfer_enqueue_group_to_postprocess (self, ctx);
  delete ctx;
}

/**
 * @fn static gpointer vvas_xinfer_infer_loop (gpointer data)
 * @param [in] data - Handle to GstVvas_XInfer
 * @return NULL when thread exit normally
 *
 * @brief The function to execute as the Infer thread
 * @detail The function receives the input frame and do
 *         1. Wait till no of input is same as decided batch size
 *            Once the sufficient frame of batch size are available then
 *         2. At level == 1
 *             Add metadata to buffer as input buffer came from either
 *             submit_input_buffer or PPE_thread this case, so metadata is not
 *             available
 *         3. Send batch of frames to Infer kernel and wait for output
 *         4. Push the buffers to the Post Process Queue, and signal the Post Process thread
 */
static gpointer
vvas_xinfer_infer_loop (gpointer data)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (data);
  gst_vvas_log_bridge_attach_thread (GST_OBJECT (self));
  GstVvas_XInferPrivate *priv = self->priv;
  vector < Vvas_XInferFrame * >infer_frames (priv->infer->max_queue, nullptr);
  vector < Vvas_XInferFrame * >batch_frames (priv->infer->batch_size, nullptr);
  guint batch_len = 0;
  guint cur_batch_size = 0;
  guint total_queued_size = 0, cur_queued_size = 0;
  gboolean got_eos = FALSE;
  gboolean timeout_triggered = FALSE;
  VvasReturnType vret;
  bool hw_input = false;
  bool hw_output = false;
  vector < VvasVideoFrameMapInfo > mapped_inputs;
  vector < vector < VvasMemoryMapInfo >> mapped_outputs;
  vector < VvasVideoFrame * >hw_inputs;
  vector < vector < VvasMemory * > >hw_outputs;

  /* Mark thread is running */
  priv->infer->thread_state = VVAS_THREAD_RUNNING;

  /* Execute till stop raised */
  while (!priv->stop) {
    guint idx;
    guint min_batch = 0;

    g_mutex_lock (&priv->infer->lock);
    batch_len = g_queue_get_length (priv->infer->batch_queue);

    g_cond_signal (&priv->infer->batch_full);
    if (!((guint) priv->stop) && batch_len < priv->infer->batch_size &&
        priv->pre_proc->thread_state != VVAS_THREAD_EXITED && !priv->is_eos
        && !priv->is_pad_eos) {
      /* wait for batch size frames */
      GST_DEBUG_OBJECT (self, "wait for the next batch");
      if (self->batch_timeout) {
        gint64 end_time =
            g_get_monotonic_time () +
            self->batch_timeout * G_TIME_SPAN_MILLISECOND;
        if (!g_cond_wait_until (&priv->infer->cond, &priv->infer->lock,
                end_time)) {
          GST_DEBUG_OBJECT (self,
              "Infer batch submit timeout triggered!!, batch length is %d, "
              "batch-size is %d, current batch timeout is %d (milliseconds)",
              g_queue_get_length (priv->infer->batch_queue)
              , priv->infer->batch_size, self->batch_timeout);
          timeout_triggered = TRUE;
        }
      } else {
        g_cond_wait (&priv->infer->cond, &priv->infer->lock);
      }
    }
    g_mutex_unlock (&priv->infer->lock);

    if (priv->stop)
      goto exit;

    if (priv->infer->level == 1 || !priv->infer->low_latency) {
      if ((g_queue_get_length (priv->infer->batch_queue) <
              priv->infer->batch_size)
          && priv->pre_proc->thread_state != VVAS_THREAD_EXITED && !priv->is_eos
          && !priv->is_pad_eos && !timeout_triggered) {
        GST_ERROR_OBJECT (self,
            "unexpected behaviour!!! "
            "batch length (%d) < required batch size %d",
            g_queue_get_length (priv->infer->batch_queue),
            priv->infer->batch_size);
        goto error;
      }
    }

    /* here we have sufficient buffer to process */
    /* get updated batch length */
    g_mutex_lock (&priv->infer->lock);
    batch_len = g_queue_get_length (priv->infer->batch_queue);
    g_mutex_unlock (&priv->infer->lock);

  infer_pending:

    if (priv->stop)
      goto exit;

    min_batch = (guint) batch_len > priv->infer->batch_size ?
        priv->infer->batch_size : batch_len;
    min_batch = priv->infer->max_queue - total_queued_size < min_batch ?
        priv->infer->max_queue - total_queued_size : min_batch;
    GST_DEBUG_OBJECT (self, "preparing batch of %d frames", min_batch);

    cur_queued_size = 0;

    for (idx = 0; idx < min_batch; idx++) {
      Vvas_XInferFrame *inframe = NULL;
      g_mutex_lock (&priv->infer->lock);
      inframe =
          static_cast <
          Vvas_XInferFrame * >(g_queue_pop_head (priv->infer->batch_queue));
      g_mutex_unlock (&priv->infer->lock);

      /* Store this input buffer */
      infer_frames[total_queued_size + idx] = inframe;

      cur_queued_size++;

      if (!inframe->skip_processing) {
        /* This frame needs to be infered, put it in batch_frames */
        batch_frames[cur_batch_size++] = inframe;

        if (priv->infer->level == 1) {
          /* inference input buffer will come from either submit_input_buffer
           * or PPE_thread in case of level-1,
           * so metadata is not added in ppe_thread
           */
          GstBuffer *infer_buf =
              inframe->child_buf ? inframe->child_buf : inframe->parent_buf;
          vvas_xinfer_add_metadata_at_level_1 (self, inframe->parent_buf,
              inframe->parent_vinfo, infer_buf);
        }
      } else {
        GST_LOG_OBJECT (self, "skipping frame %p from inference", inframe);
      }

      if (cur_batch_size == priv->infer->batch_size) {
        GST_LOG_OBJECT (self, "input batch is ready for inference");
        /* incrementing to represent number elements popped */
        break;
      }

      if (priv->infer->level > 1 && priv->infer->low_latency) {
        if (inframe->last_parent_buf) {
          GST_LOG_OBJECT (self, "in low latency mode, push current batch");
          /* incrementing to represent number elements popped */
          break;
        }
      }
    }                           /* close of for loop */

    total_queued_size += cur_queued_size;

    if (!priv->infer->low_latency && cur_batch_size < priv->infer->batch_size &&
        !priv->is_eos && !priv->is_pad_eos
        && total_queued_size < priv->infer->max_queue && !timeout_triggered) {
      GST_DEBUG_OBJECT (self,
          "current batch %d is not enough. continue to fetch data",
          cur_batch_size);
      continue;
    }

    /* Reset for next iteration */
    timeout_triggered = FALSE;

    bool do_infer = (cur_batch_size && priv->last_fret == GST_FLOW_OK);
    if (do_infer) {
      hw_input = (priv->infer->runtime == MLRuntime::VART) &&
        (priv->infer->vart_info.inp_tensor_type == vart::TensorType::HW);
      hw_output = (priv->infer->runtime == MLRuntime::VART) &&
        (priv->infer->vart_info.out_tensor_type == vart::TensorType::HW);

      GST_DEBUG_OBJECT (self, "HW_INPUT = %d, HW_OUTPUT = %d", hw_input,
          hw_output);

      mapped_inputs.clear ();
      mapped_inputs.resize (cur_batch_size);
      mapped_outputs.clear ();
      mapped_outputs.resize (cur_batch_size);
      hw_inputs.clear ();
      hw_outputs.clear ();

      /*Map inputs and outputs to help create input/output tensors */
      for (guint b = 0; b < cur_batch_size; b++) {
        /* Map the inputs */
        if (hw_input) {
          hw_inputs.push_back (batch_frames[b]->vvas_frame);
        } else {
          vret = vvas_video_frame_map (batch_frames[b]->vvas_frame,
              VVAS_DATA_MAP_READ, &mapped_inputs[b]);
          if (VVAS_RET_SUCCESS != vret) {
            GST_ERROR_OBJECT (self, "couldn't map input frame for reading");
            goto error;
          }
        }

        /* Prepare pointers to store output tensor data */
        batch_frames[b]->tensors = new vector < VvasMemory * >;
        GST_DEBUG_OBJECT (self, "acquiring memory for tensors");
        *batch_frames[b]->tensors =
            priv->post_proc->tensor_pool->acquire_memories ();
        GST_DEBUG_OBJECT (self, "acquired memory for tensors: %p %p",
            batch_frames[b]->tensors, (*batch_frames[b]->tensors)[0]);
        if (hw_output) {
          hw_outputs.push_back (*(batch_frames[b]->tensors));
        } else {
          vector < VvasMemoryMapInfo > tensor_map_info =
              priv->post_proc->tensor_pool->
              map_memories (*batch_frames[b]->tensors, VVAS_DATA_MAP_WRITE);
          mapped_outputs[b] = std::move (tensor_map_info);
        }
      }

      /* Create backend input/output tensors. For VART these are moved into
        * the job context so they remain valid until the (possibly async)
        * completion. For ONNX inference runs synchronously right here. */
      std::optional<std::vector<std::vector<vart::NpuTensor>>> vart_in {};
      std::optional<std::vector<std::vector<vart::NpuTensor>>> vart_out {};

      if(priv->infer->runtime == MLRuntime::VART){
        if(hw_input){
          vart_in = priv_create_vart_input_hw (priv->infer->vart_info.runner,
              hw_inputs, cur_batch_size);
        } else {
          vart_in = priv_create_vart_input_sw (priv->infer->vart_info.runner,
              mapped_inputs, cur_batch_size);
        }
        if(!vart_in) {
          goto error;
        }

        if(hw_output){
          vart_out = priv_create_vart_output_hw (priv->infer->vart_info.runner,
              hw_outputs, cur_batch_size, priv->infer->model_config.num_out_tensors);
        } else {
          vart_out = priv_create_vart_output_sw (priv->infer->vart_info.runner,
              mapped_outputs, cur_batch_size, priv->infer->model_config.num_out_tensors);
        }
        if(!vart_out) {
          goto error;
        }
      } else if (priv->infer->runtime == MLRuntime::ONNXRT) {
        Ort::MemoryInfo memory_info =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto onnx_in = priv_create_onnx_input (priv, memory_info,
              mapped_inputs, cur_batch_size);
        if(!onnx_in)
          goto error;

        auto onnx_out = priv_create_onnx_output (priv, memory_info,
              mapped_outputs, cur_batch_size, priv->infer->model_config.num_out_tensors);
        if(!onnx_out)
          goto error;

        if (!priv_run_onnx_inference (priv, *onnx_in, *onnx_out)){
          goto error;
        }
      } else {
          GST_ERROR_OBJECT (self, "Invalid ml inference runtime");
          goto error;
      }

      /* Build the job context; ownership of the group/batch frames, IO
        * mappings and backend tensors transfers to ctx so the loop-local
        * vectors can be reused for the next batch immediately. */
      InferJobContext *ctx = new InferJobContext ();
      ctx->hw_input = hw_input;
      ctx->hw_output = hw_output;
      ctx->t0_us = 0;
      ctx->group.assign (infer_frames.begin (),
          infer_frames.begin () + total_queued_size);
      ctx->batch.assign (batch_frames.begin (),
          batch_frames.begin () + cur_batch_size);
      ctx->mapped_inputs = std::move (mapped_inputs);
      ctx->mapped_outputs = std::move (mapped_outputs);
      ctx->vart_in = std::move (vart_in);
      ctx->vart_out = std::move (vart_out);

      /* Detect EOS within this group so the infer thread can break after
        * dispatch; the EOS frame itself is forwarded to Post Process. */
      for (guint i = 0; i < total_queued_size; i++) {
        if (infer_frames[i] && infer_frames[i]->event &&
            GST_EVENT_TYPE (infer_frames[i]->event) == GST_EVENT_EOS) {
          got_eos = TRUE;
          GST_INFO_OBJECT (self,
              "received EOS, will exit thread %" GST_PTR_FORMAT,
              infer_frames[i]->event);
        }
        infer_frames[i] = nullptr;
      }
      /* reset all entries in batch_frames to nullptr */
      fill (batch_frames.begin(), batch_frames.end(), nullptr);

      if (priv->infer->runtime == MLRuntime::VART) {
        if (priv->infer_profiler.enabled)
          ctx->t0_us = vvas_profiler_now_us ();

        if (priv->infer->vart_info.use_async) {
          /* Reserve an in-flight slot before submitting so the drain
            * barrier and the completion callback are balanced. */
          g_mutex_lock (&priv->infer->async_lock);
          priv->infer->async_in_flight++;
          g_mutex_unlock (&priv->infer->async_lock);

          bool submitted = false;
          while (!priv->stop) {
            auto vart_callback = [self, ctx] (const vart::JobHandle & h) {
              GstVvas_XInferPrivate *p = self->priv;
              GST_DEBUG_OBJECT(self, "VART inference async callback recieved for job id = %u", h.job_id);
              vvas_xinfer_vart_infer_finalize (self, ctx, h);
              g_mutex_lock (&p->infer->async_lock);
              if (p->infer->async_in_flight > 0)
                p->infer->async_in_flight--;
              g_cond_broadcast (&p->infer->async_cond);
              g_mutex_unlock (&p->infer->async_lock);
            };
            auto jh = priv->infer->vart_info.runner->execute_async (
                *ctx->vart_in, *ctx->vart_out,
                vart_callback);
            if (jh.status == vart::StatusCode::SUCCESS) {
              submitted = true;
              GST_DEBUG_OBJECT(self, "VART inference async job submitted id = %u",
                  jh.job_id);
              break;
            } else if (jh.status == vart::StatusCode::RESOURCE_UNAVAILABLE) {
              /* Transient: all execution slots busy, retry submission. */
              GST_LOG_OBJECT (self, "async submit slots busy, retrying");
              g_usleep (100);
              continue;
            } else {
              GST_ERROR_OBJECT (self, "async submit failed with status %d",
                  (int) jh.status);
              break;
            }
          }

          if (!submitted) {
            /* Submission failed (non-transient) or stop was raised: finalize
              * inline with failure and release the reserved in-flight slot. */
            vvas_xinfer_vart_infer_finalize (self, ctx,
                vart::JobHandle {vart::StatusCode::FAILURE, 0});
            g_mutex_lock (&priv->infer->async_lock);
            if (priv->infer->async_in_flight > 0)
              priv->infer->async_in_flight--;
            g_cond_broadcast (&priv->infer->async_cond);
            g_mutex_unlock (&priv->infer->async_lock);
          }
        } else {
          auto ok = priv_run_vart_inference (priv->infer->vart_info.runner,
              *ctx->vart_in, *ctx->vart_out);
          vvas_xinfer_vart_infer_finalize (self, ctx,
              vart::JobHandle {ok ? vart::StatusCode::SUCCESS :
                  vart::StatusCode::FAILURE, 0});
        }
      } else {
        /* ONNX inference already ran synchronously above. */
        vvas_xinfer_onnxrt_infer_finalize (self, ctx);
      }
    } else if (total_queued_size) {
      /* No inference for this group (skip-only/event/EOS frames, or the
        * pipeline is already in an error state). Such a group has no async
        * job; drain any in-flight async jobs first so it cannot overtake
        * their results, then forward it directly. */
      InferJobContext *ctx = new InferJobContext ();
      ctx->hw_input = false;
      ctx->hw_output = false;
      ctx->t0_us = 0;
      ctx->group.assign (infer_frames.begin (),
          infer_frames.begin () + total_queued_size);

      for (guint i = 0; i < total_queued_size; i++) {
        if (infer_frames[i] && infer_frames[i]->event &&
            GST_EVENT_TYPE (infer_frames[i]->event) == GST_EVENT_EOS) {
          got_eos = TRUE;
          GST_INFO_OBJECT (self,
              "received EOS, will exit thread %" GST_PTR_FORMAT,
              infer_frames[i]->event);
        }
        infer_frames[i] = nullptr;
      }
      fill (batch_frames.begin(), batch_frames.end(), nullptr);

      g_mutex_lock (&priv->infer->async_lock);
      while (priv->infer->async_in_flight > 0 && !priv->stop)
        g_cond_wait (&priv->infer->async_cond, &priv->infer->async_lock);
      g_mutex_unlock (&priv->infer->async_lock);

      vvas_xinfer_enqueue_group_to_postprocess (self, ctx);
      delete ctx;
    }

    if (priv->infer->level > 1 && (batch_len - cur_queued_size > 0)) {
      GST_LOG_OBJECT (self, "processing pending %d inference frames",
          batch_len - cur_queued_size);
      batch_len = batch_len - cur_queued_size;
      /* reset variables for next batch */
      total_queued_size = 0;
      cur_batch_size = 0;
      goto infer_pending;
    }

    /* reset variables for next batch */
    total_queued_size = 0;
    cur_batch_size = 0;

    if (got_eos) {
      GST_DEBUG_OBJECT (self, "Exiting thread because of EOS");
      break;
    }
  }                             /* End of while loop */

exit:
  /* Wait unconditionally (even on stop/error) for all in-flight async
   * inference jobs to complete before tearing down. Their completion callbacks
   * reference the tensor pool, Post Process queue and this element; they must
   * not run after the infer thread returns and resources are freed. The runner
   * still completes already-submitted jobs, so async_in_flight reaches 0. */
  g_mutex_lock (&priv->infer->async_lock);
  while (priv->infer->async_in_flight > 0)
    g_cond_wait (&priv->infer->async_cond, &priv->infer->async_lock);
  g_mutex_unlock (&priv->infer->async_lock);

  priv->infer->thread_state = VVAS_THREAD_EXITED;

  /* Free any frames still held in local arrays that were already popped from
   * the queue (e.g. when stop is set mid-batch). These are not in any queue
   * so the stop-path cleanup in gst_vvas_xinfer_stop() won't reach them. */
  for (auto * frame:infer_frames) {
    if (frame) {
      if (frame->tensors) {
        priv->post_proc->tensor_pool->release_memories (*frame->tensors);
        delete frame->tensors;
        frame->tensors = nullptr;
      }
      vvas_xinfer_free_infer_frame (frame);
    }
  }

  infer_frames.clear ();
  infer_frames.shrink_to_fit ();

  batch_frames.clear ();
  batch_frames.shrink_to_fit ();

  /* wake up Post Processing thread, if it is waiting */
  g_mutex_lock (&priv->post_proc->lock);
  g_cond_signal (&priv->post_proc->cond);
  g_mutex_unlock (&priv->post_proc->lock);

  GST_DEBUG_OBJECT (self, "infer thread exiting");

  return NULL;

error:
  /* Release any tensors and input mappings held for the current batch
   * to avoid leaking tensor pool memory and mapped frames on error. */
  for (guint b = 0; b < cur_batch_size; b++) {
    if (batch_frames[b] && batch_frames[b]->tensors) {
      /* SW-mapped output tensors (mapped only when output tensor type is CPU) */
      if (!hw_output) {
        priv->post_proc->tensor_pool->unmap_memories (
            *batch_frames[b]->tensors, mapped_outputs[b]);
      }
      priv->post_proc->tensor_pool->
          release_memories (*batch_frames[b]->tensors);
      delete batch_frames[b]->tensors;
      batch_frames[b]->tensors = nullptr;
    }
    /* SW-mapped input frames (mapped only when input tensor type is CPU) */
    if (!hw_input && batch_frames[b] && batch_frames[b]->vvas_frame) {
      vvas_video_frame_unmap (batch_frames[b]->vvas_frame, &mapped_inputs[b]);
    }
  }

  GST_ELEMENT_ERROR (self, STREAM, FAILED,
      ("failed to process frame in inference."),
      ("failed to process frame in inference."));
  priv->last_fret = GST_FLOW_ERROR;
  priv->stop = TRUE;

  goto exit;
}

static gboolean
vvas_xinfer_handle_gst_metadata (GstVvas_XInfer *self,
    vector < Vvas_XInferFrame *>&infer_frames, guint num_buffers)
{
  GstVvas_XInferPrivate *priv = self->priv;

  for (guint idx = 0; idx < num_buffers; idx++) {
    if (!infer_frames[idx]->child_buf)
      continue;

    if (priv->infer->level == 1) {
      if (infer_frames[idx]->parent_buf == infer_frames[idx]->child_buf)
        continue;

      /*
       * The parent_buf != child_buf, indicating that the parent_buf was scaled down to meet the
       * resolution requirement of the model. The returned inference results are based on the
       * model's resolution. Hence scale these results to match the resolution of the parent_buf
       */
      GstInferenceMeta *child_meta;
      Vvas_XInferNodeInfo node_info = { 0 };
      node_info.self = self;
      node_info.parent_vinfo = infer_frames[idx]->parent_vinfo;
      node_info.child_vinfo = infer_frames[idx]->child_vinfo;
      node_info.input_roi = infer_frames[idx]->input_roi;
      node_info.output_roi = infer_frames[idx]->output_roi;
      node_info.use_roi_data = infer_frames[idx]->use_roi_data;

      /* child_buf received from PPE, so update metadata in parent buf */
      child_meta =
          (GstInferenceMeta *)
          gst_buffer_get_meta (infer_frames[idx]->child_buf,
          gst_inference_meta_api_get_type ());
      if (!child_meta)
        continue;
      if (!g_node_n_children ((GNode *) child_meta->prediction->
              prediction.node))
        continue;

      GST_DEBUG_OBJECT (self, "number of children: %u",
          g_node_n_children ((GNode *) child_meta->prediction->
              prediction.node));

      /*scale child prediction to match with parent */
      g_node_children_foreach ((GNode *) child_meta->prediction->
          prediction.node, G_TRAVERSE_ALL, update_child_bbox, &node_info);

      if (!gst_buffer_is_writable (infer_frames[idx]->parent_buf)) {
        GST_DEBUG_OBJECT (self, "create writable buffer of %p",
            infer_frames[idx]->parent_buf);
        GstBuffer *writable_buf = NULL;
        writable_buf = gst_buffer_make_writable (infer_frames[idx]->parent_buf);
        infer_frames[idx]->parent_buf = writable_buf;
      }

      GstInferenceMeta *parent_meta;
      parent_meta =
          (GstInferenceMeta *)
          gst_buffer_get_meta (infer_frames[idx]->parent_buf,
          gst_inference_meta_api_get_type ());
      if (!parent_meta) {
        GST_DEBUG_OBJECT (self, "add inference metadata to parent: %p",
            infer_frames[idx]->parent_buf);
        parent_meta = (GstInferenceMeta *)
            gst_buffer_add_meta (infer_frames[idx]->parent_buf,
            gst_inference_meta_get_info (), NULL);
        if (!parent_meta) {
          GST_ERROR_OBJECT (self, "failed to add metadata to parent buffer");
          return false;
        }
        /* assigning childmeta to parent metadata prediction */
        gst_inference_prediction_unref (parent_meta->prediction);
        parent_meta->prediction = child_meta->prediction;
        child_meta->prediction = gst_inference_prediction_new ();

        parent_meta->prediction->prediction.width =
            GST_VIDEO_INFO_WIDTH (infer_frames[idx]->parent_vinfo);
        parent_meta->prediction->prediction.height =
            GST_VIDEO_INFO_HEIGHT (infer_frames[idx]->parent_vinfo);
        GST_LOG_OBJECT (self, "add inference metadata to %p",
            infer_frames[idx]->parent_buf);
      } else {
        gst_inference_prediction_unref (child_meta->prediction);
        child_meta->prediction = gst_inference_prediction_new ();
      }

      if (self->priv->infer->attach_ppebuf) {
        /* remove as child_buf will be attached as sub_buffer */
        gst_buffer_unref (infer_frames[idx]->child_buf);
        gst_buffer_remove_meta (infer_frames[idx]->child_buf,
            GST_META_CAST (child_meta));
        infer_frames[idx]->child_buf = NULL;
      }
    } else {                    /* inference level > 1 */
      GstInferenceMeta *child_meta = NULL;
      GstInferencePrediction *parent_prediction = NULL;
      Vvas_XInferNodeInfo node_info = { 0 };

      node_info.self = self;
      node_info.parent_vinfo = infer_frames[idx]->parent_vinfo;
      node_info.child_vinfo = infer_frames[idx]->child_vinfo;
      node_info.input_roi = infer_frames[idx]->input_roi;
      node_info.output_roi = infer_frames[idx]->output_roi;
      node_info.use_roi_data = infer_frames[idx]->use_roi_data;

      child_meta =
          (GstInferenceMeta *)
          gst_buffer_get_meta (infer_frames[idx]->child_buf,
          gst_inference_meta_api_get_type ());
      if (!child_meta)
        continue;
      parent_prediction = (GstInferencePrediction *)
          child_meta->prediction->prediction.node->parent->data;

      g_node_children_foreach ((GNode *) child_meta->prediction->
          prediction.node, G_TRAVERSE_ALL, update_child_bbox, &node_info);
      gst_inference_prediction_unref (parent_prediction);
      child_meta->prediction = gst_inference_prediction_new ();
    }
  }
  return true;
}

/**
 * @fn static void vvas_xinfer_free_infer_frame (Vvas_XInferFrame *infer_frame)
 * @param [in] infer_frame - Handle to Vvas_XInferFrame
 * @return void
 * @brief Free the memory allocated for Vvas_XInferFrame
 * @detail This function frees the memory allocated for Vvas_XInferFrame
 *        and its members
 */
static void
vvas_xinfer_free_infer_frame (Vvas_XInferFrame *infer_frame)
{
  /* Release resources */
  if (infer_frame->vvas_frame) {
    vvas_video_frame_free (infer_frame->vvas_frame);
  }

  if (infer_frame->child_buf) {
    gst_buffer_unref (infer_frame->child_buf);
  }

  if (infer_frame->internal_inbuf) {
    gst_buffer_unref (infer_frame->internal_inbuf);
  }

  if (infer_frame->child_vinfo) {
    gst_video_info_free (infer_frame->child_vinfo);
  }

  if (infer_frame->parent_vinfo) {
    gst_video_info_free (infer_frame->parent_vinfo);
  }

  g_slice_free1 (sizeof (Vvas_XInferFrame), infer_frame);
}

/**
 * @fn static gboolean vvas_xinfer_send_postprocess_buffer_downstream (GstVvas_XInfer *self, Vvas_XInferFrame *infer_frame)
 * @param [in] self - Handle to GstVvas_XInfer
 * @param [in] infer_frame - Handle to Vvas_XInferFrame
 * @return TRUE on success
 *        FALSE on failure
 * @brief Send the post processed buffer downstream
 * @detail This function sends the post processed buffer downstream and handles the flow return value
 */
static gboolean
vvas_xinfer_send_postprocess_buffer_downstream (GstVvas_XInfer *self,
    Vvas_XInferFrame *infer_frame)
{
  GstVvas_XInferPrivate *priv = self->priv;
  GstInferenceMeta *parent_meta = NULL;
  GstBuffer *writable_buf = NULL;
  gchar *infer_meta_str = NULL;
  gboolean ret = TRUE;

  parent_meta =
      (GstInferenceMeta *) gst_buffer_get_meta (infer_frame->parent_buf,
      gst_inference_meta_api_get_type ());

  /* Attaching empty metadata if attach_empty_meta is true */
  if (!parent_meta && priv->infer->attach_empty_meta) {
    if (!gst_buffer_is_writable (infer_frame->parent_buf)) {
      GST_DEBUG_OBJECT (self, "create writable buffer of %p",
          infer_frame->parent_buf);
      writable_buf = gst_buffer_make_writable (infer_frame->parent_buf);
      infer_frame->parent_buf = writable_buf;
    }

    parent_meta = (GstInferenceMeta *)
        gst_buffer_add_meta (infer_frame->parent_buf,
        gst_inference_meta_get_info (), NULL);

    /* assigning childmeta to parent metadata prediction */
    gst_inference_prediction_unref (parent_meta->prediction);
    parent_meta->prediction = gst_inference_prediction_new ();
    parent_meta->prediction->prediction.width =
        GST_VIDEO_INFO_WIDTH (infer_frame->parent_vinfo);
    parent_meta->prediction->prediction.height =
        GST_VIDEO_INFO_HEIGHT (infer_frame->parent_vinfo);
  }
#ifdef PRINT_METADATA_TREE
  /* convert metadata to string for debug log */
  if (parent_meta) {
    infer_meta_str =
        gst_inference_prediction_to_string (parent_meta->prediction);
    GST_DEBUG_OBJECT (self, "output inference metadata : %s", infer_meta_str);
    g_free (infer_meta_str);

    g_node_traverse ((GNode *) parent_meta->prediction->prediction.node,
        G_PRE_ORDER, G_TRAVERSE_ALL, -1, printf_all_nodes, self);
  }
#endif
  /* This is the last parent buf, it need to be sent downstream */
  GST_DEBUG_OBJECT (self, "Pushing frame %p, parent_buf: %p downstream",
      infer_frame, infer_frame->parent_buf);

  priv->last_fret = gst_pad_push (GST_BASE_TRANSFORM_SRC_PAD (self),
      infer_frame->parent_buf);

  if (priv->last_fret < GST_FLOW_OK) {
    switch (priv->last_fret) {
      case GST_FLOW_FLUSHING:
      case GST_FLOW_EOS:
        GST_DEBUG_OBJECT (self, "failed to push buffer. reason %s",
            gst_flow_get_name (priv->last_fret));
        break;
      default:
        GST_ELEMENT_ERROR (self, STREAM, FAILED,
            ("failed to push buffer."),
            ("failed to push buffer. reason %s (%d)",
                gst_flow_get_name (priv->last_fret), priv->last_fret.load ()));
        ret = FALSE;
        break;
    }
  }
  return ret;
}

/**
 * @fn static gpointer vvas_xinfer_postprocess_loop (gpointer data)
 * @param [in] data - Handle to GstVvas_XInfer
 * @return NULL when thread exit normally
 *
 * @brief The function to execute as the PostProcess thread
 * @detail The function receives the input frame and do
 *         1. Wait for buffers from the Infer Thread in Post Porcess Queue
 *         2. Send batch of frames to PostProcess kernel and wait for output
 *         3. Scale and handle GstInferenceMetadata
 *         4. push the frame to downstream
 *         5. Free the memory allocated for tensors
 *         6. Free the memory allocated for Vvas_XInferFrame
 */
static gpointer
vvas_xinfer_postprocess_loop (gpointer data)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (data);
  gst_vvas_log_bridge_attach_thread (GST_OBJECT (self));
  GstVvas_XInferPrivate *priv = self->priv;

  vector < Vvas_XInferFrame * >infer_frames (priv->post_proc->queue_length,
      nullptr);
  vector <
      Vvas_XInferFrame * >postprocess_frames (priv->post_proc->queue_length,
      nullptr);

  gboolean got_eos = FALSE;
  guint queue_length = 0;
  guint postprocess_frames_count = 0;

  GST_DEBUG_OBJECT (self, "PostProcess thread started with queue_size: %u",
      priv->post_proc->queue_length);
  priv->post_proc->thread_state = VVAS_THREAD_RUNNING;

  while (!priv->stop) {

    queue_length = 0;
    postprocess_frames_count = 0;

    g_mutex_lock (&priv->post_proc->lock);

    while (!priv->stop &&
        !(queue_length = g_queue_get_length (priv->post_proc->queue))) {
      /* wait untill infer thread signals. */
      GST_DEBUG_OBJECT (self, "PostProcess thread waiting for frames");
      g_cond_wait (&priv->post_proc->cond, &priv->post_proc->lock);
      queue_length = g_queue_get_length (priv->post_proc->queue);
    }

    /* check for stop */
    if (priv->stop) {
      GST_DEBUG_OBJECT (self, "PostProcess thread exiting due to Stop signal");
      g_mutex_unlock (&priv->post_proc->lock);
      break;
    }

    GST_DEBUG_OBJECT (self, "PostProcess thread got %u frames", queue_length);

    for (guint idx = 0; idx < queue_length; idx++) {
      infer_frames[idx] =
          static_cast <
          Vvas_XInferFrame * >(g_queue_pop_head (priv->post_proc->queue));
      GST_DEBUG_OBJECT (self, "Popped frame %p, parent_buf: %p from queue",
          infer_frames[idx], infer_frames[idx]->parent_buf);
    }

    /* Removed buffers from the queue, inform Infer thread if it is waiting for us */
    g_cond_signal (&priv->post_proc->cond);
    g_mutex_unlock (&priv->post_proc->lock);

    for (guint idx = 0; idx < queue_length; idx++) {
      if (!infer_frames[idx]->skip_processing && infer_frames[idx]->tensors) {
        /* this infer_frames[idx] is proessed by infer thread, we need to be post processed */
        postprocess_frames[postprocess_frames_count++] = infer_frames[idx];
      }
    }

    vector < VvasInferPrediction * >predictions (postprocess_frames_count,
        nullptr);
    vector <
        VvasMemory *
        >tensors_vvas_mem (priv->infer->model_config.num_out_tensors *
        postprocess_frames_count, nullptr);

    if (postprocess_frames_count) {
      /* prepare data pointer for doing post processing */
      vector <
          int8_t * >tensor_buf (priv->infer->model_config.num_out_tensors *
          postprocess_frames_count, nullptr);

      for (guint i = 0; i < postprocess_frames_count; i++) {
        for (guint j = 0; j < priv->infer->model_config.num_out_tensors; j++) {
          guint index = (i * priv->infer->model_config.num_out_tensors) + j;
          tensors_vvas_mem[index] = postprocess_frames[i]->tensors->at (j);
        }
      }
      GST_DEBUG_OBJECT (self, "Running post processing with %u frames",
          postprocess_frames_count);

      guint64 t0 = 0, t1 = 0;
      if (priv->infer_profiler.enabled) {
        t0 = vvas_profiler_now_us ();
      }

      auto vret = vvas_postprocess_tensor (priv->post_proc->handle,
          tensors_vvas_mem.data (), postprocess_frames_count,
          predictions.data ());

      if (priv->infer_profiler.enabled) {
        t1 = vvas_profiler_now_us ();
        g_mutex_lock (&priv->infer_profiler.snap_lock);
        vvas_profiler_stats_update (&priv->infer_profiler.post_proc,
            postprocess_frames_count, t1 - t0);
        g_mutex_unlock (&priv->infer_profiler.snap_lock);
      }

      if (vret != VVAS_RET_SUCCESS) {
        GST_ERROR_OBJECT (self,
            "vvas_postprocess_tensor failed to process frames, " "ret: %d",
            vret);
        goto error;
      }

      GST_DEBUG_OBJECT (self, "PostProcessing done");

      /* Convert VvasInferPrediction to GstInferPrediction */
      for (guint i = 0; i < postprocess_frames_count; i++) {
        GstInferenceMeta *gst_meta = NULL;
        GstBuffer *gst_buf = NULL;

        if (priv->infer_profiler.enabled) {
          g_mutex_lock (&priv->infer_profiler.snap_lock);
          priv->infer_profiler.total_frames++;
          g_mutex_unlock (&priv->infer_profiler.snap_lock);
        }

        /* Post Processing done, release tensor memory to the pool */
        GST_DEBUG_OBJECT (self, "releasing tensor memories: %p",
            postprocess_frames[i]->tensors);
        priv->post_proc->tensor_pool->
            release_memories (*postprocess_frames[i]->tensors);

        delete postprocess_frames[i]->tensors;
        postprocess_frames[i]->tensors = nullptr;

        if (!predictions[i]) {
          continue;
        }

        gst_buf =
            postprocess_frames[i]->
            child_buf ? postprocess_frames[i]->child_buf :
            postprocess_frames[i]->parent_buf;

        gst_meta = (GstInferenceMeta *) gst_buffer_get_meta (gst_buf,
            gst_inference_meta_api_get_type ());

        VvasList *tensors_list = predictions[i]->tensors;
        if (priv->attach_tensors) {
          /** Set tensors list to NULL in prediction returned from core.
           * We are going to free this VvasInferPredition tree.
           * List will be added to GstInferencePrediction tree.
           */
          predictions[i]->tensors = NULL;
        }

        if (gst_meta) {
          if (gst_meta->prediction) {
            construct_gst_inference_tree (gst_meta->prediction,
                predictions[i], priv->attach_tensors, tensors_list);
          } else {
            gst_meta->prediction = gst_inference_prediction_new ();
            construct_gst_inference_tree (gst_meta->prediction,
                predictions[i], priv->attach_tensors, tensors_list);
          }
        } else {
          gst_meta =
              (GstInferenceMeta *) gst_buffer_add_meta (gst_buf,
              gst_inference_meta_get_info (), NULL);
          construct_gst_inference_tree (gst_meta->prediction,
              predictions[i], priv->attach_tensors, tensors_list);
        }
        vvas_inferprediction_free (predictions[i]);
        predictions[i] = NULL;
      }
    }

    /* signal listeners that we have processed one batch */
    g_signal_emit (self, vvas_signals[SIGNAL_VVAS], 0);

    /* Handle GstInferenceMeta metadata */
    auto res =
        vvas_xinfer_handle_gst_metadata (self, infer_frames, queue_length);
    if (!res) {
      GST_ERROR_OBJECT (self, "failed to handle metadata");
      goto error;
    }

    /* Done with all processing, free all other buffers and send buffers downstream now */
    for (guint idx = 0; idx < queue_length; idx++) {
      if (infer_frames[idx]->last_parent_buf) {
        if (GST_FLOW_OK == priv->last_fret) {
          auto ret = vvas_xinfer_send_postprocess_buffer_downstream (self,
              infer_frames[idx]);
          if (!ret) {
            GST_ERROR_OBJECT (self, "failed to send buffer downstream");
            vvas_xinfer_free_infer_frame (infer_frames[idx]);
            infer_frames[idx] = nullptr;
            goto error;
          }
        } else {
          /* Free parent buffer */
          GST_DEBUG_OBJECT (self, "Freeing buffer %p",
              infer_frames[idx]->parent_buf);
          gst_buffer_unref (infer_frames[idx]->parent_buf);
        }
      }

      if (infer_frames[idx]->event) {
        /* This is event frame */
        if (GST_EVENT_EOS == GST_EVENT_TYPE (infer_frames[idx]->event)) {
          got_eos = TRUE;
          /* EOS event will be sent from _sink_event() */
          GST_INFO_OBJECT (self,
              "received EOS, exiting thread %" GST_PTR_FORMAT,
              infer_frames[idx]->event);
        }

        if (GST_EVENT_CUSTOM_DOWNSTREAM ==
            GST_EVENT_TYPE (infer_frames[idx]->event)) {
          GST_INFO_OBJECT (self,
              "received PAD-EOS, sending downstream %" GST_PTR_FORMAT,
              infer_frames[idx]->event);
          GST_BASE_TRANSFORM_CLASS (parent_class)->sink_event ((GstBaseTransform
                  *) data, infer_frames[idx]->event);
          priv->is_pad_eos = FALSE;
        }
      }

      vvas_xinfer_free_infer_frame (infer_frames[idx]);
      infer_frames[idx] = nullptr;
    }

    if (got_eos) {
      GST_DEBUG_OBJECT (self, "Exiting thread because of EOS");
      break;
    }
  }                             /*End of while loop */

exit:
  priv->post_proc->thread_state = VVAS_THREAD_EXITED;

  GST_DEBUG_OBJECT (self, "Post Process thread exiting");

  return nullptr;

error:
  GST_ELEMENT_ERROR (self, STREAM, FAILED,
      ("Post Processing failed."), ("Post Processing failed."));
  priv->last_fret = GST_FLOW_ERROR;
  priv->stop = TRUE;

  /* Release tensor memories for postprocess frames on error path.
   * Do NOT free the frames here -- they alias infer_frames[] entries
   * and will be freed by the infer_frames loop below. */
  for (guint idx = 0; idx < postprocess_frames_count; idx++) {
    if (postprocess_frames[idx]) {
      if (postprocess_frames[idx]->tensors) {
        priv->post_proc->
            tensor_pool->release_memories (*postprocess_frames[idx]->tensors);
        delete postprocess_frames[idx]->tensors;
        postprocess_frames[idx]->tensors = nullptr;
      }
    }
  }

  /* Free all infer_frames (includes those that were also in postprocess_frames) */
  for (guint idx = 0; idx < queue_length; idx++) {
    if (infer_frames[idx]) {
      vvas_xinfer_free_infer_frame (infer_frames[idx]);
      infer_frames[idx] = nullptr;
    }
  }

  goto exit;
}

/**
 * @fn static inline gboolean vvas_xinfer_send_ppe_frame (GstVvas_XInfer * self, GstBuffer * parent_buf,
 *							   GstVideoInfo * parent_vinfo,
 *							   GstBuffer * child_buf, GstVideoInfo * child_vinfo,
 *							   VvasVideoFrame * vvas_frame, gboolean skip_process,
 *							   gboolean is_first_parent, GstEvent * event)
 * @param [in] self - handle to GstVvas_XInfer
 * @param [in] parent_buf - input gstreamer buffer from upstream
 * @param [in] parent_vinfo - video info of input buffer
 * @param [in] child_buf - Buffer which need to be preprocessed
 * @param [in] child_vinfo - Video info of child buffer
 * @param [in] vvas_frame - Pointer to VvasVideoFrame
 * @param [in] skip_process - Processing of frame need to be skipped or not
 * @param [in] is_first_parent - used to update last_parent_buf
 * @param [in] event - GstEvent
 *
 * @return TRUE on success
 *         FALSE on failure
 *
 * @brief This function prepare ppe_frame with buffer which need to be pre
 *        processed and generate ppe.has_input. The function is called from
 *        submit_input_buffer when input buffer is available on sink pad of
 *        infer
 */
static inline gboolean
vvas_xinfer_send_ppe_frame (GstVvas_XInfer *self, GstBuffer *parent_buf,
    GstVideoInfo *parent_vinfo,
    GstBuffer *child_buf, GstVideoInfo *child_vinfo,
    VvasVideoFrame *vvas_frame, gboolean skip_process,
    gboolean is_first_parent, GstEvent *event)
{
  GstVvas_XInferPrivate *priv = self->priv;

  g_mutex_lock (&priv->pre_proc->lock);
  if (!priv->pre_proc->need_data && !priv->stop) {
    /* ppe inbuf is not consumed wait till thread consumes it */
    g_cond_wait (&priv->pre_proc->need_input, &priv->pre_proc->lock);
  }

  /* Do not process if stop is set */
  if (priv->stop) {
    g_mutex_unlock (&priv->pre_proc->lock);
    return FALSE;
  }
  priv->pre_proc->frame->parent_buf = parent_buf;
  priv->pre_proc->frame->parent_vinfo = parent_vinfo;
  priv->pre_proc->frame->last_parent_buf = is_first_parent;
  priv->pre_proc->frame->vvas_frame = vvas_frame;
  priv->pre_proc->frame->child_buf = child_buf;
  priv->pre_proc->frame->child_vinfo = child_vinfo;
  priv->pre_proc->frame->event = NULL;
  priv->pre_proc->frame->skip_processing = skip_process;
  /* ppe data is available now */
  priv->pre_proc->need_data = FALSE;

  GST_LOG_OBJECT (self, "send frame to ppe loop with skip_processing %d",
      skip_process);
  /* wakeup ppe thread as data is available for processing */
  g_cond_signal (&priv->pre_proc->has_input);
  g_mutex_unlock (&priv->pre_proc->lock);
  return TRUE;
}

/**
 * @fn static GstFlowReturn gst_vvas_xinfer_submit_input_buffer (GstBaseTransform * trans,
 *								 gboolean is_discont, GstBuffer * inbuf)
 * @param [in] trans - Handle to GstBaseTransform
 * @param [in] is_discont - Indicates whether input buffer is coming after discontinuity from previous input buffer
 * @param [in] inbuf - input buffer from upstream
 *
 * @return GST_FLOW_ERROR in case of error
 *         GST_FLOW_OK exit without error
 *
 * @brief Function which accepts a new input buffer from upstream and processes it
 * @detail This function called when the plugin receives a buffer from upstream
 *        element. Two way of handling of input buffer is provided
 *        1. When preprocessing is enable
 *           - xinfer is at level 1
 *           The input buffer is mapped to ppe frame and send to ppe
 *           thread to process by emitting ppe.has_input signal
 *           - xinfer is at level > 1
 *           It is checked either the sub-buffer from previous inference can
 *           be used for inference or either previous infer has some detection.
 *           Accordingly the input buffer or sub-buffer are mapped to ppe frame
 *           and send to ppe thread to process by emitting ppe.has_input signal.
 *        2. When no preprocessing
 *           - xinfer is at level 1
 *           Mapped input buffer to infer_frame and add to infer->batch_queue to
 *           be processed by infer thread
 *           - xinfer is at level > 1
 *           Skip inbuffer if previous infer do not detect anything
 *           Check either previous infer sub-buffer can be used and if yes add
 *           them to infer->batch_queue after mapping to infer_frame.
 *
 */
static GstFlowReturn
gst_vvas_xinfer_submit_input_buffer (GstBaseTransform *trans,
    gboolean is_discont, GstBuffer *inbuf)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (trans);
  GST_VVAS_LOG_SCOPE (self);
  GstVvas_XInferPrivate *priv = self->priv;
  gboolean bret = FALSE;
  gboolean skip_preprocess = FALSE;
  GST_LOG_OBJECT (self, "received %" GST_PTR_FORMAT, inbuf);

  g_mutex_lock (&priv->infer->lock);
  if (!priv->stop && g_queue_get_length (priv->infer->batch_queue) >=
      (priv->infer->batch_size << 1)) {
    GST_LOG_OBJECT (self, "inference batch queue is full. Wait for free space");
    g_cond_wait (&priv->infer->batch_full, &priv->infer->lock);
  }
  g_mutex_unlock (&priv->infer->lock);

  if (priv->stop) {
    gst_buffer_unref (inbuf);
    return GST_FLOW_FLUSHING;
  }

  if (priv->pre_proc->enabled) {        /* send frames to PPE thread */
    GstBuffer *new_inbuf = NULL;
    VvasVideoFrame *vvas_frame = NULL;
    GstInferenceMeta *parent_meta = NULL;
    GstBuffer *child_buf = NULL;
    GstVideoInfo *child_vinfo = NULL;

    if (priv->infer->level == 1) {
      parent_meta = (GstInferenceMeta *) gst_buffer_get_meta (inbuf,
          gst_inference_meta_api_get_type ());
      if (!parent_meta
          || !vvas_xinfer_is_sub_buffer_useful (self,
              parent_meta->prediction->sub_buffer)) {
        /* PPE needs HW buffer as input */
        bret = vvas_xinfer_prepare_ppe_input_frame (self, inbuf,
            priv->in_vinfo, &new_inbuf, &vvas_frame);
        if (!bret) {
          if (new_inbuf)
            gst_buffer_unref (new_inbuf);
          gst_buffer_unref (inbuf);
          return GST_FLOW_ERROR;
        }
        child_buf = (new_inbuf == NULL ? gst_buffer_ref (inbuf) : new_inbuf);
        child_vinfo = gst_video_info_copy (priv->in_vinfo);
      }
    } else {                    /* priv->infer->level > 1 */
      /* Thread-safety for split (tee) pipelines: make the input buffer
       * writable so this xinfer branch gets its own copy of all metadata
       * (including the inference prediction tree). Without this, multiple
       * L2 xinfer streaming threads race on the same GNode tree, causing
       * segfaults in g_node_child_position / g_node_insert_before.
       * gst_buffer_make_writable triggers gst_inference_meta_transform
       * which deep-copies the prediction tree via vvas_treenode_copy_deep. */
      if (priv->infer->attach_ppebuf) {
        inbuf = gst_buffer_make_writable (inbuf);
      }
      parent_meta = (GstInferenceMeta *) gst_buffer_get_meta (inbuf,
          gst_inference_meta_api_get_type ());

      if (parent_meta) {
        /* Previous infer has meta i.e found something */
        Vvas_XInferNumSubs numSubs = { self, 0, 0 };

        /* Check either sub-buffer from previous infer can be used */
        if (priv->infer->level <=
            g_node_max_height ((GNode *) parent_meta->prediction->
                prediction.node)) {
          g_node_traverse ((GNode *) parent_meta->prediction->prediction.node,
              G_PRE_ORDER, G_TRAVERSE_ALL, -1, check_bbox_buffers_availability,
              &numSubs);
        }

        if (!numSubs.required_buffer) {
          skip_preprocess = TRUE;
        }

        if (numSubs.available_buffer < numSubs.required_buffer) {
          /* PPE needs HW buffer as input */
          bret =
              vvas_xinfer_prepare_ppe_input_frame (self, inbuf, priv->in_vinfo,
              &new_inbuf, &vvas_frame);
          if (!bret) {
            if (new_inbuf)
              gst_buffer_unref (new_inbuf);
            gst_buffer_unref (inbuf);
            return GST_FLOW_ERROR;
          }
        }
      }
      child_buf = (new_inbuf == NULL ? gst_buffer_ref (inbuf) : new_inbuf);
      child_vinfo = gst_video_info_copy (priv->in_vinfo);
    }

    /* mapped inbuf/child_buf to ppe_frame and raise signal ppe.has_input */
    GstVideoInfo *parent_vinfo_copy = gst_video_info_copy (priv->in_vinfo);
    bret = vvas_xinfer_send_ppe_frame (self, inbuf,
        parent_vinfo_copy, child_buf, child_vinfo,
        vvas_frame, skip_preprocess, TRUE, NULL);
    if (!bret) {
      gst_video_info_free (parent_vinfo_copy);
      gst_video_info_free (child_vinfo);
      vvas_video_frame_free (vvas_frame);
      gst_buffer_unref (inbuf);
      gst_buffer_unref (child_buf);
      return GST_FLOW_FLUSHING;
    }
  } else {                      /* !priv->pre_proc->enabled */
    /* send frames to inference thread directly */
    Vvas_XInferFrame *infer_frame = NULL;
    VvasVideoFrame *vvas_frame = NULL;
    GstInferenceMeta *infer_meta = NULL;

    infer_meta =
        (GstInferenceMeta *) gst_buffer_get_meta (inbuf,
        gst_inference_meta_api_get_type ());

    if (priv->infer->level == 1) {
      GstBuffer *infer_buf = NULL, *child_buf = NULL;
      GstVideoInfo *infer_vinfo = NULL, *child_vinfo = NULL;

      infer_frame = g_slice_new0 (Vvas_XInferFrame);

      if (infer_meta && infer_meta->prediction->sub_buffer &&
          vvas_xinfer_is_sub_buffer_useful (self,
              infer_meta->prediction->sub_buffer)) {
        GstVideoMeta *vmeta = NULL;

        child_vinfo = infer_vinfo = gst_video_info_new ();
        child_buf = infer_buf =
            gst_buffer_ref (infer_meta->prediction->sub_buffer);

        vmeta = gst_buffer_get_video_meta (child_buf);
        gst_video_info_set_format (child_vinfo, vmeta->format,
            vmeta->width, vmeta->height);
      } else {
        /* inference operates on big buffer, no child_buf */
        child_buf = NULL;
        child_vinfo = NULL;
        infer_vinfo = priv->in_vinfo;
        infer_buf = inbuf;
      }

      bret = vvas_xinfer_prepare_infer_input_frame (self, infer_buf,
          infer_vinfo, &vvas_frame, &infer_frame->internal_inbuf);
      if (!bret) {
        if (infer_frame->internal_inbuf)
          gst_buffer_unref (infer_frame->internal_inbuf);
        g_slice_free (Vvas_XInferFrame, infer_frame);
        if (child_vinfo)
          gst_video_info_free (child_vinfo);
        if (child_buf)
          gst_buffer_unref (child_buf);
        return GST_FLOW_ERROR;
      }

      infer_frame->parent_buf = inbuf;
      infer_frame->parent_vinfo = gst_video_info_copy (priv->in_vinfo);
      infer_frame->last_parent_buf = TRUE;
      infer_frame->child_buf = child_buf;
      infer_frame->child_vinfo = child_vinfo;
      infer_frame->vvas_frame = vvas_frame;
      infer_frame->skip_processing = FALSE;
      infer_frame->event = NULL;

      GST_LOG_OBJECT (self, "send frame %p to level-%d inference", infer_frame,
          priv->infer->level);

      /* send input frame to inference thread */
      g_mutex_lock (&priv->infer->lock);
      g_queue_push_tail (priv->infer->batch_queue, infer_frame);
      g_mutex_unlock (&priv->infer->lock);
    }

    if (g_queue_get_length (priv->infer->batch_queue) >=
        priv->infer->batch_size) {
      g_mutex_lock (&priv->infer->lock);
      GST_LOG_OBJECT (self, "signal inference thread as queue size reached %u",
          g_queue_get_length (priv->infer->batch_queue));
      g_cond_signal (&priv->infer->cond);
      g_mutex_unlock (&priv->infer->lock);
    }
  }

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_vvas_xinfer_generate_output (GstBaseTransform *trans, GstBuffer **outbuf)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (trans);

  return self->priv->last_fret;
}

/**
 *  @fn static void gst_vvas_xfilter_set_property (GObject * object, guint prop_id, const GValue * value,
 *						   GParamSpec * pspec)
 *  @param [in] Handle - to GstVvas_XFilter typecasted to GObject
 *  @param [in] prop_id - Property ID value
 *  @param [in] value - GValue which holds property value set by user
 *  @param [in] pspec - Handle to metadata of a property with property ID prop_id
 *  @return None
 *  @brief This API stores values sent from the user in GstVvas_XFilter object members.
 *  @details This API is registered with GObjectClass by overriding GObjectClass::set_property
 *           function pointer and this will be invoked when developer sets properties on
 *           GstVvas_XFilter object. Based on property value type, corresponding
 *           g_value_get_xxx API will be called to get property value from GValue handle.
 */
static void
gst_vvas_xinfer_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (object);

  switch (prop_id) {
    case PROP_CONFIG_LOCATION:
      if (GST_STATE (self) != GST_STATE_NULL) {
        g_warning
            ("can't set inference json_file path when instance is NOT in NULL state");
        return;
      }
      if (self->config_file)
        g_free (self->config_file);
      self->config_file = g_value_dup_string (value);
      break;
    case PROP_BATCH_SUBMIT_TIMEOUT:
      self->batch_timeout = g_value_get_uint (value);
      break;
    case PROP_ENABLE_PROFILER:
      self->priv->infer_profiler.enabled = g_value_get_boolean (value);
      break;
    case PROP_PROFILER_FILE:
      if (self->priv->infer_profiler.log_file)
        g_free (self->priv->infer_profiler.log_file);
      self->priv->infer_profiler.log_file = g_value_dup_string (value);
      break;
    case PROP_PROFILER_LOG_INTERVAL:
      self->priv->infer_profiler.log_interval = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/**
 *  @fn static void gst_vvas_xfilter_get_property (GObject * object, guint prop_id, GValue * value,
 *					           GParamSpec * pspec)
 *  @param [in] Handle - to GstVvas_XFilter typecasted to GObject
 *  @param [in] prop_id - Property ID value
 *  @param [in] value - GValue which holds property value to get by user
 *  @param [in] pspec - Handle to metadata of a property with property ID prop_id
 *  @return None
 *  @brief This API gets currently configured value of a property from GstVvas_XFilter instance
 *  @details This API is registered with GObjectClass by overriding GObjectClass::get_property
 *	     function pointer and this will be invoked when developer gets properties on
 *	     GstVvas_XFilter object. Based on property value type, corresponding g_value_set_xxx API
 *	     will be called to set property value to GValue handle.
 */
static void
gst_vvas_xinfer_get_property (GObject *object, guint prop_id, GValue *value,
    GParamSpec *pspec)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (object);

  switch (prop_id) {
    case PROP_CONFIG_LOCATION:
      g_value_set_string (value, self->config_file);
      break;
    case PROP_BATCH_SUBMIT_TIMEOUT:
      g_value_set_uint (value, self->batch_timeout);
      break;
    case PROP_ENABLE_PROFILER:
      g_value_set_boolean (value, self->priv->infer_profiler.enabled);
      break;
    case PROP_PROFILER_FILE:
      g_value_set_string (value, self->priv->infer_profiler.log_file);
      break;
    case PROP_PROFILER_LOG_INTERVAL:
      g_value_set_uint (value, self->priv->infer_profiler.log_interval);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/**
 *  @fn static gboolean gst_vvas_xfilter_create (GstBaseTransform * trans)
 *  @param [in] - trans xinfer's parents instance handle which will be type casted to xinfer instance
 *  @return TRUE on success\n
 *          FALSE on failure
 *  @brief This API will be invoked during NULL_TO_READY transition to acquire resources.
 *  @details This API initializes xinfer member variables, reads json of ppe and infer, opens device handle
 *           and invokes initialization of INFER and PPE acceleration library.
 *        */
static gboolean
gst_vvas_xinfer_create (GstBaseTransform *trans)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (trans);
  GstVvas_XInferPrivate *priv = self->priv;
  gboolean bret = FALSE;
  json_t *root = NULL;
  json_error_t error;
  priv->instance_name = GST_ELEMENT_NAME (self);

  GST_INFO_OBJECT (self, "create (instance: %s)", priv->instance_name);

  priv->pre_proc = std::make_unique < PreProcessInfo > ();
  priv->infer = std::make_unique < InferInfo > ();
  priv->post_proc = std::make_unique < PostProcessInfo > ();

  priv->infer->attach_empty_meta = DEFAULT_ATTACH_EMPTY_METADATA;
  priv->infer->input_class_filters = NULL;
  priv->infer->num_input_class_filters = 0;

  priv->pre_proc->dev_idx = PPE_DEVICE_IDX;
  priv->in_vinfo = gst_video_info_new ();
  priv->do_init = TRUE;
  priv->stop = FALSE;
  priv->is_eos = FALSE;
  priv->is_pad_eos = FALSE;
  priv->pre_proc->enabled = FALSE;
  priv->infer->input_tensor_format = VVAS_VIDEO_FORMAT_UNKNOWN;
  priv->attach_tensors = FALSE;
  priv->core_log_level =
      vvas_get_core_log_level (gst_debug_category_get_threshold
      (gst_vvas_xinfer_debug));

  priv->pre_proc->is_quant_set = FALSE;
  priv->post_proc->is_dequant_set = FALSE;

  /* get root json object */
  root = json_load_file (self->config_file, JSON_DECODE_ANY, &error);
  if (!root) {
    GST_ERROR_OBJECT (self, "failed to load json file. reason %s", error.text);

    /* print to console */
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        ("failed to load json file. reason %s", error.text), (NULL));
    goto error;
  }

  GST_DEBUG_OBJECT (self, "Parsing preprocess-config");

  bret = read_ppe_config (self, root, priv->pre_proc.get ());
  if (!bret)
    goto error;

  GST_DEBUG_OBJECT (self, "Parsing Infer config");
  bret = read_infer_config (self, root, priv->infer.get ());
  if (!bret)
    goto error;

  bret = read_postprocess_config (self, root, priv->post_proc.get ());
  if (!bret)
    goto error;

  if (priv->infer->level > 1 && !priv->pre_proc->enabled) {
    GST_ERROR_OBJECT (self,
        "PPE is not available, when inference-level(%d) > 1",
        priv->infer->level);
    goto error;
  }

  if (priv->pre_proc->enabled) {
    memset (priv->pre_proc->core_handle->input, 0x0,
        sizeof (VvasVideoFrame *) * MAX_ROI);
    memset (priv->pre_proc->core_handle->output, 0x0,
        sizeof (VvasVideoFrame *) * MAX_ROI);
  }

  if (priv->infer_profiler.enabled) {
    vvas_infer_profiler_init (&priv->infer_profiler);
  }

  if (priv->do_init) {

    GST_DEBUG_OBJECT (self, "Doing inference init");

    if (!vvas_xinfer_infer_init (self))
      goto error;

    priv->infer->core_handle->init_done = TRUE;

    /* VART Light Runner quantization is done as part of pre-processing */
    priv->pre_proc->param.scale_r *=
        priv->infer->model_config.in_tensors[0].scale_coeff;
    priv->pre_proc->param.scale_g *=
        priv->infer->model_config.in_tensors[0].scale_coeff;
    priv->pre_proc->param.scale_b *=
        priv->infer->model_config.in_tensors[0].scale_coeff;

    if (priv->pre_proc->enabled) {
      priv->pre_proc->core_handle->init_done = FALSE;

      GST_DEBUG_OBJECT (self, "Doing PPE init");
      if (!vvas_xinfer_ppe_init (self))
        goto error;

      /* should be used before starting deinit */
      priv->pre_proc->core_handle->init_done = TRUE;
    }

    if (priv->post_proc->enabled) {
      if (!vvas_xinfer_postproc_init (self))
        goto error;
    }
    priv->do_init = FALSE;
  }

  GST_INFO_OBJECT (self, "%s to preprocess inference input frames",
      priv->pre_proc->enabled ? "need" : "not needed");

  priv->pre_proc->thread_state = VVAS_THREAD_NOT_CREATED;
  priv->infer->thread_state = VVAS_THREAD_NOT_CREATED;

  if (root)
    json_decref (root);

  GST_INFO_OBJECT (self, "start completed (instance: %s)", priv->instance_name);
  return TRUE;

error:
  if (priv->infer_profiler.enabled) {
    vvas_infer_profiler_deinit (&priv->infer_profiler);
  }

  if (root)
    json_decref (root);

  if (priv->pre_proc->enabled)
    vvas_xinfer_ppe_deinit (self);

  if (priv->post_proc->enabled)
    vvas_xinfer_postproc_deinit (self);

  vvas_xinfer_infer_deinit (self);
  gst_video_info_free (self->priv->in_vinfo);
  self->priv->in_vinfo = NULL;
  return FALSE;
}

/**
 *  @fn static gboolean gst_vvas_xinfer_query (GstBaseTransform * trans, GstPadDirection direction, GstQuery * query)
 *  @param [in] trans - xinfer's parents instance handle which will be type casted to xinfer instance
 *  @param [in] direction - The direction(sink/src) of a pad on which query received
 *  @param [in] query - Query received by xinfer instance on pad with direction
 *  @return TRUE if query handled successfully
 *          FALSE if query is not handled
 *  @brief Answers the capabilities query (i.e. GST_QUERY_CAPS) by populating it with vvas acceleration
 *         capabilities and invokes parent's query vmethod if a query is not handled by xinfer
 *  @ detail This also convert kcaps from infer kernel to GstCaps
 */
static gboolean
gst_vvas_xinfer_query (GstBaseTransform *trans,
    GstPadDirection direction, GstQuery *query)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (trans);
  GstVvas_XInferPrivate *priv = self->priv;
  gboolean ret = TRUE;
  VvasImageProcessCapabilities *caps = NULL;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:{
      /* Take kernel kcap and convert to gst caps */
      GstCaps *newcap, *allcaps, *filter = NULL;
      GstStructure *s;
      GValue list = { 0, };
      GValue aval = { 0, };
      const char *fourcc;
      GstCaps *peercaps = NULL;
      GstPad *peer = NULL;
      gint par_n = 0, par_d = 0;

      if (self->priv->do_init == TRUE || self->priv->stop)
        return FALSE;

      gst_query_parse_caps (query, &filter);
      GST_DEBUG_OBJECT (self, "Querying caps with filter = %" GST_PTR_FORMAT,
          filter);

      /* When we are queried about our caps requirement, we should also consider the
       * pixel aspect ratio required by the downstream, this is important when
       * are using a video sink, might not be required for fakesink/filesink */
      if (direction == GST_PAD_SINK) {
        GstStructure *structure;
        peer = gst_pad_get_peer (trans->srcpad);
        if (peer)
          peercaps = gst_pad_query_caps (peer, NULL);

        if (peercaps && gst_caps_get_size (peercaps)) {
          structure = gst_caps_get_structure (peercaps, 0);
          if (structure && gst_structure_has_field (structure,
                  "pixel-aspect-ratio")) {
            gst_structure_get_fraction (structure, "pixel-aspect-ratio", &par_n,
                &par_d);
          } else {
            par_n = 0;
            par_d = 0;
          }
          gst_caps_unref (peercaps);
        }
        if (peer) {
          gst_object_unref (peer);
        }
      }

      /* Same buffer for sink and src,
       * so the same caps for sink and src pads
       * and for all pads
       */

      allcaps = gst_caps_new_empty ();
      newcap = gst_caps_new_empty ();

      g_value_init (&list, GST_TYPE_LIST);
      g_value_init (&aval, G_TYPE_STRING);

      s = gst_structure_new ("video/x-raw", "height", G_TYPE_INT,
          priv->infer->model_config.model_height, NULL);
      gst_structure_set (s, "width", G_TYPE_INT,
          priv->infer->model_config.model_width, NULL);

      if (priv->pre_proc->enabled) {
        fourcc =
            gst_video_format_to_string (gst_coreutils_get_gst_fmt_from_vvas
            (priv->infer->model_format));
        g_value_set_string (&aval, fourcc);
      } else {
        fourcc = vvas_format_to_caps_str (priv->infer->input_tensor_format);
        g_value_set_string (&aval, fourcc);
        GST_DEBUG_OBJECT (self, "Added format %s", fourcc);
      }
      gst_value_list_append_value (&list, &aval);
      gst_structure_set_value (s, "format", &list);
      g_value_unset (&aval);
      g_value_unset (&list);

      /* If there is pixel aspect ratio from downstream
       * we should consider it */
      if (par_n) {
        gst_structure_set (s, "pixel-aspect-ratio", GST_TYPE_FRACTION,
            par_n, par_d, NULL);
      }

      gst_caps_append_structure (newcap, s);
      gst_caps_append (allcaps, newcap);

      GST_DEBUG_OBJECT (self,
          "Appending caps created by model config %" GST_PTR_FORMAT, allcaps);

      newcap = gst_caps_new_empty ();

      /* The Infer plugin will set the input stream CAPS range based on the Preprocessing library.
       * If Plugin will do preprocessing, it will query the HW/SW image processing library capabilities
       * and set that as the range.*/
      if (priv->pre_proc->enabled) {
        /* Get image process capabilities for default library */
        caps =
            vvas_image_process_get_capabilities (priv->pre_proc->
            core_handle->name);
        if (!caps) {
          gst_caps_unref (newcap);
          gst_caps_unref (allcaps);
          return FALSE;
        }
        gst_caps_unref (newcap);
        newcap = vvas_image_process_get_gstcaps_input (caps);
        free (caps);
      }

      if (!gst_caps_is_empty (newcap)) {
        GST_DEBUG_OBJECT (self,
            "Appending caps added by pre-processor %" GST_PTR_FORMAT, newcap);
        gst_caps_append (allcaps, newcap);
      } else {
        gst_caps_unref (newcap);
      }

      if (filter) {
        gchar *str = gst_caps_to_string (filter);
        gchar *s_str = gst_caps_to_string (allcaps);
        GST_DEBUG_OBJECT (self, "supported caps: %s, filter caps = %s", s_str,
            str);
        g_free (str);
        g_free (s_str);

        GstCaps *tmp = allcaps;
        allcaps =
            gst_caps_intersect_full (filter, tmp, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref (tmp);

        if (gst_caps_is_empty (allcaps)) {
          GST_ERROR_OBJECT (self, "can't support caps requested on sink pad");
        }
      }

      {
        gchar *str = gst_caps_to_string (allcaps);
        GST_INFO_OBJECT (self, "returning caps = %s", str);
        g_free (str);
      }

      gst_query_set_caps_result (query, allcaps);
      gst_caps_unref (allcaps);

      return TRUE;
    }

    case GST_QUERY_CUSTOM:{
      const GstStructure *s = gst_query_get_structure (query);
      GstStructure *st;
      if (gst_structure_has_name (s, "vvas-infer-quant-factor")) {
        GST_DEBUG_OBJECT (self, "Received vvas-infer-quant-factor query");
        query = gst_query_make_writable (query);
        st = gst_query_writable_structure (query);
        if (st) {
          /* When doing pre-processing, return default quantization factor.
           * xinfer is a video plugin and supports single-input-tensor models
           * only, so the 1st input tensor's scale is always the right one. */
          float quant_factor = priv->pre_proc->enabled ?
              1.0 : self->priv->infer->model_config.in_tensors[0].scale_coeff;
          gst_structure_set (st,
              "quantization_factor", G_TYPE_FLOAT, quant_factor, NULL);
        }
        return TRUE;
      } else if (gst_structure_has_name (s, "infer-batch-size")) {
        GST_DEBUG_OBJECT (self, "Received infer-batch-size query");
        query = gst_query_make_writable (query);
        st = gst_query_writable_structure (query);
        if (st) {
          gst_structure_set (st,
              "batch_size", G_TYPE_UINT, self->priv->infer->batch_size, NULL);
        }
        return TRUE;
      }
      break;
    }

    default:
      ret = TRUE;
      break;
  }

  GST_BASE_TRANSFORM_CLASS (parent_class)->query (trans, direction, query);
  return ret;
}

/**
 *  @fn static gboolean gst_vvas_xinfer_sink_event (GstBaseTransform * trans, GstEvent * event)
 *  @param [in] trans - xinfer's parents instance handle which will be type casted to xinfer instance
 *  @param [in] event - GstEvent received by xinfer instance on sink pad
 *  @return TRUE if event handled successfully
 *          FALSE if event is not handled
 *  @brief Handle the GstEvent and invokes parent's event vmethod if event is not handled by xinfer
 */
static gboolean
gst_vvas_xinfer_sink_event (GstBaseTransform *trans, GstEvent *event)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (trans);
  GST_VVAS_LOG_SCOPE (self);
  GstVvas_XInferPrivate *priv = self->priv;

  GST_LOG_OBJECT (self, "received sink event: %" GST_PTR_FORMAT, event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:{
      GST_INFO_OBJECT (self, "received EOS event");
      priv->is_eos = TRUE;

      if (priv->pre_proc->thread) {
        g_mutex_lock (&priv->pre_proc->lock);
        if (!priv->pre_proc->need_data && !priv->stop) {
          /* ppe inbuf is not consumed wait till thread consumes it */
          g_cond_wait (&priv->pre_proc->need_input, &priv->pre_proc->lock);
        }

        if (priv->stop) {
          /* return if stop is already generated */
          g_mutex_unlock (&priv->pre_proc->lock);
          gst_event_unref (event);
          return TRUE;
        }

        priv->pre_proc->frame->parent_buf = NULL;
        priv->pre_proc->frame->child_buf = NULL;
        priv->pre_proc->frame->parent_vinfo = NULL;
        priv->pre_proc->frame->child_vinfo = NULL;
        priv->pre_proc->frame->vvas_frame = NULL;
        priv->pre_proc->frame->skip_processing = TRUE;
        priv->pre_proc->frame->event = event;
        priv->pre_proc->need_data = FALSE;

        GST_INFO_OBJECT (self, "send event %p to preprocess thread", event);

        g_cond_signal (&priv->pre_proc->has_input);
        g_mutex_unlock (&priv->pre_proc->lock);
      } else {
        Vvas_XInferFrame *event_frame = NULL;

        event_frame = g_slice_new0 (Vvas_XInferFrame);
        event_frame->skip_processing = TRUE;
        event_frame->event = event;

        GST_INFO_OBJECT (self, "send event %p to inference thread",
            event_frame);

        g_mutex_lock (&self->priv->infer->lock);
        g_cond_signal (&self->priv->infer->cond);
        GST_INFO_OBJECT (self, "signalled infer thread to exit");
        /* send input frame to inference thread */
        g_queue_push_tail (priv->infer->batch_queue, event_frame);
        g_mutex_unlock (&self->priv->infer->lock);
      }

      /* stuck here untill PPE and INFER thread exits */
      if (priv->infer->thread) {
        GST_DEBUG_OBJECT (self, "waiting for inference thread to exit");
        g_thread_join (priv->infer->thread);
        GST_DEBUG_OBJECT (self, "inference thread exited");
        priv->infer->thread = NULL;
      }
      if (priv->pre_proc->thread) {
        GST_DEBUG_OBJECT (self, "waiting for ppe thread to exit");
        g_thread_join (priv->pre_proc->thread);
        GST_DEBUG_OBJECT (self, "ppe thread exited");
        priv->pre_proc->thread = NULL;
      }

      if (self->priv->post_proc->thread) {
        GST_DEBUG_OBJECT (self, "Waiting for Post Process thread to exit");
        g_thread_join (self->priv->post_proc->thread);
        self->priv->post_proc->thread = NULL;
        GST_DEBUG_OBJECT (self, "Post Process thread joined");
      }

      priv->is_eos = FALSE;
      break;
    }
    case GST_EVENT_CUSTOM_DOWNSTREAM:{
      /* Got Custom downstream event */
      const GstStructure *structure = gst_event_get_structure (event);
      guint pad_idx;
      Vvas_XInferFrame *event_frame;
      /* Get index of sender pad of this event */
      gst_structure_get_uint (structure, "pad-index", &pad_idx);

      if (!g_strcmp0 (gst_structure_get_name (structure), "pad-eos")) {
        /* Got custom Pad-EOS event */
        GST_DEBUG_OBJECT (self, "Got pad-eos event on pad %u", pad_idx);

        if (priv->pre_proc->thread) {
          g_mutex_lock (&priv->pre_proc->lock);
          if (!priv->pre_proc->need_data && !priv->stop) {
            /* ppe inbuf is not consumed wait till thread consumes it */
            g_cond_wait (&priv->pre_proc->need_input, &priv->pre_proc->lock);
          }
          g_mutex_unlock (&priv->pre_proc->lock);
        }

        event_frame = g_slice_new0 (Vvas_XInferFrame);
        event_frame->event = event;
        event_frame->skip_processing = TRUE;

        GST_INFO_OBJECT (self, "send event %p to inference thread",
            event_frame);

        g_mutex_lock (&self->priv->infer->lock);
        /* send input frame to inference thread */
        priv->is_pad_eos = TRUE;
        g_cond_signal (&self->priv->infer->cond);
        g_queue_push_tail (priv->infer->batch_queue, event_frame);
        g_mutex_unlock (&self->priv->infer->lock);
        return TRUE;
      }
      break;
    }
    case GST_EVENT_FLUSH_STOP:{
      GST_INFO_OBJECT (self, "freeing internal queues");
      /* Full flush requires draining the preproc / infer / postproc queues
       * (which hold Vvas_XInferFrame*, not GstBuffer*) under their respective
       * locks and resetting is_pad_eos / is_eos / batch counters. Not yet
       * implemented; multi-stream pipelines should avoid relying on flush. */
      break;
    }
    default:
      break;
  }
  GST_LOG_OBJECT (self, "pushing %" GST_PTR_FORMAT, event);
  return GST_BASE_TRANSFORM_CLASS (parent_class)->sink_event (trans, event);
}

/**
 *  @fn static gboolean gst_vvas_xinfer_propose_allocation (GstBaseTransform * trans, GstQuery * decide_query,
 *							    GstQuery * query)
 *  @param [in] trans - xinfer's parents instance handle which will be type casted to xinfer instance
 *  @param [in] decide_query - Query that was passed to the decide_allocation callback
 *  @param [inout] query - Query received on sink pad from upstream which needs to be updated xinfer requirements
 *  @return TRUE on success
 *          FALSE on failure
 *  @brief Proposes buffer allocation parameters for upstream element
 *  @details Pass the query to downstream and if there is no proposals from downstream then add xinfer proposal
 *           only if pre-process is enable, by setting Video buffer pool, VVAS allocator, alignment required and
 *           buffers required on query
*/
static gboolean
gst_vvas_xinfer_propose_allocation (GstBaseTransform *trans,
    GstQuery *decide_query, GstQuery *query)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (trans);
  GstVvas_XInferPrivate *priv = self->priv;
  GstCaps *caps;
  GstVideoInfo info;
  GstBufferPool *pool = NULL;
  GstAllocator *allocator = NULL;
  GstVideoAlignment align;
  guint size;

  /* call parent's propose allocation to send query downstream */
  GST_BASE_TRANSFORM_CLASS (parent_class)->propose_allocation (trans,
      decide_query, query);

  gst_query_parse_allocation (query, &caps, NULL);

  if (caps == NULL)
    return FALSE;

  if (!gst_video_info_from_caps (&info, caps))
    return FALSE;

  size = GST_VIDEO_INFO_SIZE (&info);

  /* downstream does not have any proposals or element */
  if (gst_query_get_n_allocation_pools (query) == 0) {
    GstStructure *pool_config;
    GstAllocationParams params = { GST_MEMORY_FLAG_PHYSICALLY_CONTIGUOUS, 0, 0,
      0
    };

    if (gst_query_get_n_allocation_params (query) > 0) {
      gst_query_parse_nth_allocation_param (query, 0, &allocator, &params);
    } else {
      if (self->priv->pre_proc->enabled && !self->priv->pre_proc->use_software) {
        /* HW preprocess enabled — create VVAS allocator on preprocess device.
         * USE_DMABUF_EXPORT enables DMA fd export so upstream (e.g. v4l2src)
         * can use io-mode=dmabuf-import for zero-copy capture. */
        allocator =
            gst_vvas_allocator_new (self->priv->pre_proc->dev_idx,
            self->priv->pre_proc->xclbin_loc, USE_DMABUF_EXPORT,
            priv->pre_proc->in_mem_bank);
      } else if (!self->priv->pre_proc->enabled &&
          priv->infer->runtime == MLRuntime::VART &&
          priv->infer->vart_info.inp_tensor_type == vart::TensorType::HW) {
        /* No preprocess, VART HW tensor — create VVAS allocator on infer
         * device so upstream can capture directly into XRT memory. */
        allocator =
            gst_vvas_allocator_new (XDNA_DEVICE_IDX, NULL, USE_DMABUF_EXPORT,
            DEFAULT_MBANK_IDX);
      } else {
        /* SW preprocess, ONNXRT, or VART CPU — system memory is fine */
        allocator = NULL;
      }
      gst_query_add_allocation_param (query, allocator, &params);
    }

    if (priv->pre_proc->enabled && !priv->pre_proc->use_software
        && GST_IS_VVAS_ALLOCATOR (allocator)) {
      /* HW preprocess: PPE stride alignment required */
      pool = gst_vvas_buffer_pool_new (PPE_STRIDE_ALIGN, 1);
    } else if (GST_IS_VVAS_ALLOCATOR (allocator)) {
      /* VART HW tensor without preprocess: infer device VVAS pool, no stride constraint */
      pool = gst_vvas_buffer_pool_new (1, 1);
    } else {
      pool = gst_video_buffer_pool_new ();
    }

    pool_config = gst_buffer_pool_get_config (pool);

    gst_buffer_pool_config_set_params (pool_config, caps, size,
        /* one extra for preprocessing */
        self->priv->infer->batch_size + 1, 0);

    gst_buffer_pool_config_add_option (pool_config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);

    if (priv->pre_proc->enabled && !priv->pre_proc->use_software) {
      gst_video_alignment_reset (&align);
      for (guint idx = 0; idx < GST_VIDEO_INFO_N_PLANES (&info); idx++) {
        align.stride_align[idx] = (PPE_STRIDE_ALIGN - 1);
      }
      gst_buffer_pool_config_add_option (pool_config,
          GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
      gst_buffer_pool_config_add_option (pool_config,
          GST_VVAS_BUFFER_POOL_OPTION_MEMSET);
      gst_buffer_pool_config_set_video_alignment (pool_config, &align);
    }

    if (allocator)
      gst_buffer_pool_config_set_allocator (pool_config, allocator, &params);

    if (!gst_buffer_pool_set_config (pool, pool_config))
      goto config_failed;

    GST_OBJECT_LOCK (self);
    gst_query_add_allocation_pool (query, pool, size,
        self->priv->infer->batch_size + 1, 0);
    GST_OBJECT_UNLOCK (self);

    GST_INFO_OBJECT (self, "allocated internal pool %" GST_PTR_FORMAT, pool);

    gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

    GST_DEBUG_OBJECT (self, "prepared query %" GST_PTR_FORMAT, query);

    if (allocator)
      gst_object_unref (allocator);
    if (pool)
      gst_object_unref (pool);
  } else {
    /* Downstream has proposal. Append current buffer requirment and
     * send to upstream.
     */
    guint min = 0;
    guint max = 0;

    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);

    /*
     * The downstream pool still backs xinfer's HW PPE input.  Apply the
     * stride alignment returned by the image-process library here as well as
     * in the xinfer-created-pool path above, otherwise upstream can negotiate
     * a pool that forces vvas_xinfer_prepare_ppe_input_frame() to copy.
     */
    if (priv->pre_proc->enabled && !priv->pre_proc->use_software && pool) {
      GstStructure *pool_config = gst_buffer_pool_get_config (pool);

      gst_video_alignment_reset (&align);
      for (guint idx = 0; idx < GST_VIDEO_INFO_N_PLANES (&info); idx++)
        align.stride_align[idx] = PPE_STRIDE_ALIGN - 1;

      gst_buffer_pool_config_add_option (pool_config,
          GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
      gst_buffer_pool_config_set_video_alignment (pool_config, &align);
      if (!gst_buffer_pool_set_config (pool, pool_config)) {
        GST_ERROR_OBJECT (self,
            "failed to configure downstream allocation pool for PPE stride");
        gst_object_unref (pool);
        return FALSE;
      }
    }

    min += self->priv->infer->batch_size + 1;

    /* max value 0 indicates unlimited buffers, so do not
     * modify the value if it is 0.
     */
    if (0 != max) {
      max += self->priv->infer->batch_size + 1;
    }

    /* update min and max buffers */
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);

    GST_INFO_OBJECT (self, "updated min buffers %u and max buffers = %u", min,
        max);

    gst_object_unref (pool);
  }

  return TRUE;

  /* ERRORS */
config_failed:
  {
    GST_ERROR_OBJECT (self, "failed to set config");
    if (allocator)
      gst_object_unref (allocator);
    gst_object_unref (pool);
    return FALSE;
  }
}

/** @def MIN_IMAGE_PROCESS_INPUT_WIDTH
 *  @brief Set minimum width requirement of Library
 */
#define MIN_IMAGE_PROCESS_INPUT_WIDTH 64

/** @def MIN_IMAGE_PROCESS_INPUT_HEIGHT
 *  @brief Set minimum height requirement of Library
 */
#define MIN_IMAGE_PROCESS_INPUT_HEIGHT 64

/**
 *  @fn static gboolean gst_vvas_xinfer_set_caps (GstBaseTransform * trans, GstCaps * incaps, GstCaps * outcaps)
 *  @param [in] trans - xinfer's parents instance handle which will be type casted to xinfer instance
 *  @param [in] incaps - Input capabilities configured on xinfer instance sink pad
 *  @param [in] outcaps - Output capabilities configured on xinfer instance source pad
 *  @return TRUE on success
 *          FALSE on failure
 *  @brief  Stores input and output capabilities in xinfer's private structure
 *  @detail Stores input caps in xinfer's private structure and create output caps for PPE. The PPE output
 *          caps is decided as per infer requirement and also PPE output buffer pool is created taking
 *          consideration of stride requirement of image process library.
 */
static gboolean
gst_vvas_xinfer_set_caps (GstBaseTransform *trans, GstCaps *incaps,
    GstCaps *outcaps)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (trans);
  gboolean bret = TRUE;
  GstVvas_XInferPrivate *priv = self->priv;
  GstStructure *structure;
  const gchar *format;
  gint32 max_width, stride_align;
  gfloat max_scale_factor;
  gint32 max_height;

  GST_INFO_OBJECT (self,
      "incaps = %" GST_PTR_FORMAT " and outcaps = %" GST_PTR_FORMAT, incaps,
      outcaps);

  if (!gst_video_info_from_caps (priv->in_vinfo, incaps)) {
    GST_ERROR_OBJECT (self, "Failed to parse input caps");
    return FALSE;
  }

  priv->infer->pref_width = priv->infer->model_config.model_width;
  priv->infer->pref_height = priv->infer->model_config.model_height;
  priv->infer->pref_format =
      gst_coreutils_get_gst_fmt_from_vvas (priv->infer->input_tensor_format);
  format = vvas_format_to_caps_str (priv->infer->input_tensor_format);

  GST_INFO_OBJECT (self,
      "inference preferred caps : width = %d, height = %d, format = %s",
      priv->infer->pref_width, priv->infer->pref_height, format);

  if (priv->pre_proc->enabled) {
    GstCaps *ppe_out_caps = NULL;
    gchar *caps_str = NULL;
    gint width, height;
    GstAllocator *allocator = NULL;
    GstAllocationParams params =
        { GST_MEMORY_FLAG_PHYSICALLY_CONTIGUOUS, 0, 0, 0 };
    gsize size;

    width = priv->infer->pref_width;
    height = priv->infer->pref_height;

    /* Changing width according to worst case scenario */
    stride_align = self->priv->pre_proc->caps->alignment_req.stride;
    max_scale_factor = ((gfloat) width) / MIN_IMAGE_PROCESS_INPUT_WIDTH;
    max_width = (gint32) ((((stride_align - 1) + MIN_IMAGE_PROCESS_INPUT_WIDTH +
                (self->priv->pre_proc->caps->alignment_req.width -
                    1)) * max_scale_factor) + 1.0);
    max_width =
        ALIGN (max_width, self->priv->pre_proc->caps->alignment_req.width);

    /*3 rows on top for handling croma and 1 at bottom for even number of height */
    max_scale_factor = ((gfloat) height) / MIN_IMAGE_PROCESS_INPUT_HEIGHT;
    max_height =
        (gint32) (((MIN_IMAGE_PROCESS_INPUT_HEIGHT + 4) * max_scale_factor) +
        1.0);
    max_height = ALIGN (max_height, 2);

    ppe_out_caps = gst_caps_new_simple ("video/x-raw",
        "width", G_TYPE_INT, width, "height", G_TYPE_INT, height,
        "format", G_TYPE_STRING, format, NULL);

    caps_str = gst_caps_to_string (ppe_out_caps);
    GST_INFO_OBJECT (self, "pre processing output caps %s", caps_str);
    g_free (caps_str);

    if (!gst_video_info_from_caps (priv->pre_proc->out_vinfo, ppe_out_caps)) {
      GST_ERROR_OBJECT (self, "Failed to parse ppe out caps");
      if (ppe_out_caps)
        gst_caps_unref (ppe_out_caps);
      return FALSE;
    }

    size_t pixel_size = 1;
    if (priv->infer->model_config.in_tensors[0].data_type ==
        VVAS_TENSOR_DATA_TYPE_FLOAT32) {
      pixel_size = sizeof (float);
    } else if (priv->infer->model_config.in_tensors[0].data_type ==
        VVAS_TENSOR_DATA_TYPE_BF16) {
      pixel_size = sizeof (uint16_t);
    } else if (priv->infer->model_config.in_tensors[0].data_type ==
        VVAS_TENSOR_DATA_TYPE_FP16) {
      pixel_size = sizeof (uint16_t);
    } else if (priv->infer->model_config.in_tensors[0].data_type ==
        VVAS_TENSOR_DATA_TYPE_INT8) {
      pixel_size = sizeof (int8_t);
    } else {
      GST_ERROR_OBJECT (self, "Unsupported tensor data type");
      gst_caps_unref (ppe_out_caps);
      return FALSE;
    }

    switch (gst_coreutils_get_gst_fmt_from_vvas (priv->infer->model_format)) {
      case GST_VIDEO_FORMAT_GRAY8:
        size = ALIGN (max_width * pixel_size, stride_align);
        break;
      case GST_VIDEO_FORMAT_NV12:
        size = ALIGN ((max_width + (max_width >> 1)), stride_align);
        break;
      case GST_VIDEO_FORMAT_BGR:
        size = ALIGN (max_width * 3 * pixel_size, stride_align);
        break;
      case GST_VIDEO_FORMAT_RGBA:
      case GST_VIDEO_FORMAT_BGRA:
      case GST_VIDEO_FORMAT_RGBx:
      case GST_VIDEO_FORMAT_BGRx:
        size = ALIGN (max_width * 4 * pixel_size, stride_align);
        break;
      default:
        size = ALIGN (max_width * 3 * pixel_size, stride_align);
        break;
    }

    size = size * max_height;

    if (priv->pre_proc->outpool) {
      if (gst_buffer_pool_is_active (priv->pre_proc->outpool)) {
        if (!gst_buffer_pool_set_active (priv->pre_proc->outpool, FALSE)) {
          GST_ERROR_OBJECT (self,
              "failed to deactivate preprocess output pool");
          GST_ELEMENT_ERROR (self, STREAM, FAILED,
              ("failed to deactivate pool."),
              ("failed to deactivate preprocess output pool"));
          gst_caps_unref (ppe_out_caps);
          return FALSE;
        }
      }
      gst_object_unref (priv->pre_proc->outpool);
      priv->pre_proc->outpool = NULL;
    }

    if (priv->pre_proc->use_software) {
      priv->pre_proc->outpool = gst_video_buffer_pool_new ();

      structure = gst_buffer_pool_get_config (priv->pre_proc->outpool);

      gst_buffer_pool_config_set_params (structure, ppe_out_caps, size,
          self->priv->infer->batch_size, 0);
      gst_buffer_pool_config_add_option (structure,
          GST_BUFFER_POOL_OPTION_VIDEO_META);

      GST_LOG_OBJECT (self, "allocated preprocess output pool %" GST_PTR_FORMAT,
          priv->pre_proc->outpool);

    } else {
      GstVideoAlignment align;
      GstVideoInfo ppe_out_info;
      if (!gst_video_info_from_caps (&ppe_out_info, ppe_out_caps)) {
        GST_ERROR_OBJECT (self, "Failed to parse input caps");
        gst_caps_unref (ppe_out_caps);
        return FALSE;
      }
      priv->pre_proc->outpool = gst_vvas_buffer_pool_new (PPE_STRIDE_ALIGN, 1);

      GST_DEBUG_OBJECT (self, "ppe_output pool: %p, out_mem_bank: %d",
          priv->pre_proc->outpool, priv->pre_proc->out_mem_bank);

      allocator = gst_vvas_allocator_new_and_set (self->priv->pre_proc->dev_idx,
          self->priv->pre_proc->xclbin_loc, USE_DMABUF,
          priv->pre_proc->out_mem_bank, priv->pre_proc->init_value);
      params.flags = GST_MEMORY_FLAG_PHYSICALLY_CONTIGUOUS;

      GST_LOG_OBJECT (self, "allocated preprocess output pool %" GST_PTR_FORMAT
          "output allocator %" GST_PTR_FORMAT, priv->pre_proc->outpool,
          allocator);

      structure = gst_buffer_pool_get_config (priv->pre_proc->outpool);

      gst_video_alignment_reset (&align);
      for (guint idx = 0; idx < GST_VIDEO_INFO_N_PLANES (&ppe_out_info); idx++) {
        align.stride_align[idx] = (PPE_STRIDE_ALIGN - 1);
      }

      gst_buffer_pool_config_add_option (structure,
          GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
      gst_buffer_pool_config_set_video_alignment (structure, &align);
      /* Add option to memset allocated buffers */
      gst_buffer_pool_config_add_option (structure,
          GST_VVAS_BUFFER_POOL_OPTION_MEMSET);

      GST_DEBUG_OBJECT (self, "Buffer pool caps: %" GST_PTR_FORMAT,
          ppe_out_caps);

      /* set the max size required as calculated above */
      gst_buffer_pool_config_set_params (structure, ppe_out_caps, size,
          self->priv->infer->batch_size, 0);
      gst_buffer_pool_config_add_option (structure,
          GST_BUFFER_POOL_OPTION_VIDEO_META);
      gst_buffer_pool_config_set_allocator (structure, allocator, &params);

      if (allocator)
        gst_object_unref (allocator);
    }

    if (ppe_out_caps)
      gst_caps_unref (ppe_out_caps);

    if (!gst_buffer_pool_set_config (priv->pre_proc->outpool, structure)) {
      GST_ERROR_OBJECT (self, "failed to configure pool");
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("failed to configure pool."),
          ("failed to configure preprocess output pool"));
      gst_object_unref (priv->pre_proc->outpool);
      priv->pre_proc->outpool = NULL;
      return FALSE;
    }

    if (!gst_buffer_pool_set_active (priv->pre_proc->outpool, TRUE)) {
      GST_ERROR_OBJECT (self, "failed to activate preprocess output pool");
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("failed to activate pool."),
          ("failed to activate preprocess output pool"));
      return FALSE;
    }
  } else {
    /* Pre-Processing is disabled, input width, height and format must be same as
     * model's expectation.
     */
    const gchar *infer_format = NULL;
    const gchar *in_format = NULL;
    GstStructure *structure;
    int in_width = 0, in_height = 0;

    infer_format = vvas_format_to_caps_str (priv->infer->input_tensor_format);
    structure = gst_caps_get_structure (incaps, 0);
    in_format = gst_structure_get_string (structure, "format");

    if (g_strcmp0 (infer_format, in_format)) {
      GST_ERROR_OBJECT (self, "input format %s is not acceptable for inference"
          ", expected format is %s", in_format, infer_format);
      return FALSE;
    }

    if (!gst_structure_get (structure,
            "width", G_TYPE_INT, &in_width,
            "height", G_TYPE_INT, &in_height, NULL)) {
      GST_ERROR_OBJECT (self, "couldn't get input width and height");
      return FALSE;
    }

    if (((guint) in_width != priv->infer->pref_width) ||
        ((guint) in_height != priv->infer->pref_height)) {
      GST_ERROR_OBJECT (self, "input width and height are not acceptable"
          " for inference, expected width: %d, height: %d",
          priv->infer->pref_width, priv->infer->pref_height);
      return FALSE;
    }
  }

  return bret;
}

/**
 *  @fn static gboolean gst_vvas_xinfer_start (GstBaseTransform * trans)
 *  @param [in] - trans xinfer's parents instance handle which will be type casted to xfilter instance
 *  @return TRUE on success
 *          FALSE on failure
 *  @brief This API will be invoked during READY_TO_PAUSED transition.
 *  @detail This function creates ppe and infer threads and initializes other members.
 */
static gboolean
gst_vvas_xinfer_start (GstBaseTransform *trans)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (trans);
  GstVvas_XInferPrivate *priv = self->priv;
  gchar *thread_name = NULL;

  priv->stop = FALSE;
  priv->last_fret = GST_FLOW_OK;

  g_mutex_init (&priv->infer->lock);
  g_cond_init (&priv->infer->cond);
  g_cond_init (&priv->infer->batch_full);

  g_mutex_init (&priv->infer->async_lock);
  g_cond_init (&priv->infer->async_cond);
  priv->infer->async_in_flight = 0;

  priv->infer->batch_queue = g_queue_new ();
  priv->infer->sub_buffers = g_queue_new ();

  priv->pre_proc->frame = g_slice_new0 (Vvas_XInferFrame);
  /* wait on event ppe.need_input or ppe.has_input as per ppe.need_data */
  priv->pre_proc->need_data = TRUE;
  priv->pre_proc->out_vinfo = gst_video_info_new ();
  priv->pre_proc->buf_queue = g_queue_new ();

  g_mutex_init (&priv->pre_proc->lock);
  g_cond_init (&priv->pre_proc->has_input);
  g_cond_init (&priv->pre_proc->need_input);

  if (priv->infer_profiler.enabled && priv->infer_profiler.log_interval > 0) {
    g_mutex_lock (&priv->infer_profiler.snap_lock);
    if (priv->infer_profiler.timeout_id)
      g_source_remove (priv->infer_profiler.timeout_id);
    priv->infer_profiler.timeout_id =
        g_timeout_add (priv->infer_profiler.log_interval * 1000,
        vvas_infer_profiler_tick_cb, &priv->infer_profiler);
    g_mutex_unlock (&priv->infer_profiler.snap_lock);
  }

  if (priv->pre_proc->enabled) {
    thread_name = g_strdup_printf ("ppe-thread");
    priv->pre_proc->thread =
        g_thread_new (thread_name, vvas_xinfer_ppe_loop, self);
    GST_DEBUG_OBJECT (self, "ppe thread: %s created", thread_name);
    g_free (thread_name);
  }

  thread_name = g_strdup_printf ("infer-thread");
  priv->infer->thread =
      g_thread_new (thread_name, vvas_xinfer_infer_loop, self);
  GST_DEBUG_OBJECT (self, "inference thread: %s created", thread_name);
  g_free (thread_name);

  if (priv->post_proc->enabled) {
    /* Post Processing Enabled, create post process thread */
    g_mutex_init (&priv->post_proc->lock);
    g_cond_init (&priv->post_proc->cond);

    priv->post_proc->queue = g_queue_new ();

    priv->post_proc->queue_length = priv->infer->max_queue;

    thread_name = g_strdup_printf ("postproc-thread");
    priv->post_proc->thread =
        g_thread_new (thread_name, vvas_xinfer_postprocess_loop, self);
    GST_DEBUG_OBJECT (self, "postprocess thread: %s created", thread_name);
    g_free (thread_name);
  }

  if (priv->infer_profiler.enabled) {
    g_mutex_lock (&priv->infer_profiler.snap_lock);
    priv->infer_profiler.start_time_us = vvas_profiler_now_us ();
    g_mutex_unlock (&priv->infer_profiler.snap_lock);
  }

  return TRUE;
}

/**
 * @fn static void vvas_xinfer_free_xinferfreame (gpointer data, gpointer user_data)
 * @param [in] data - pointer to Vvas_XInferFrame instance
 * @param [in] user_data - pointer to GstVvas_XInfer instance
 * @brief This function is used to free the Vvas_XInferFrame instance
 */
static void
vvas_xinfer_free_xinferframe (gpointer data, gpointer user_data)
{
  GstVvas_XInfer *self = static_cast < GstVvas_XInfer * >(user_data);
  GstVvas_XInferPrivate *priv = self->priv;
  Vvas_XInferFrame *frame = static_cast < Vvas_XInferFrame * >(data);

  /* Release tensor pool memory if the frame was processed by the infer
   * thread but not yet consumed by the post-process thread (e.g. on stop). */
  if (frame->tensors) {
    if (priv->post_proc->tensor_pool)
      priv->post_proc->tensor_pool->release_memories (*frame->tensors);
    delete frame->tensors;
    frame->tensors = nullptr;
  }

  if (frame->parent_vinfo)
    gst_video_info_free (frame->parent_vinfo);

  if (frame->vvas_frame) {
    vvas_video_frame_free (frame->vvas_frame);
  }

  if (frame->child_buf) {
    if (priv->infer->level > 1) {
      GstInferenceMeta *child_meta = NULL;
      GstInferencePrediction *parent_prediction = NULL;
      /* Clear the prediction of child buf */
      child_meta =
          (GstInferenceMeta *) gst_buffer_get_meta (frame->child_buf,
          gst_inference_meta_api_get_type ());
      if (child_meta) {
        parent_prediction = (GstInferencePrediction *)
            child_meta->prediction->prediction.node->parent->data;

        gst_inference_prediction_unref (parent_prediction);
        /* Adding a dummy prediction instance, which will get cleared
         * when buffer is cleaned */
        child_meta->prediction = gst_inference_prediction_new ();
      }
    }
    GST_INFO_OBJECT (self, "Deinit Unreffing child buf : %p", frame->child_buf);
    gst_buffer_unref (frame->child_buf);
  }
  if (frame->child_vinfo)
    gst_video_info_free (frame->child_vinfo);

  if (frame->last_parent_buf)
    gst_buffer_unref (frame->parent_buf);

  g_slice_free1 (sizeof (Vvas_XInferFrame), frame);
}

/**
 *  @fn static gboolean gst_vvas_xinfer_stop (GstBaseTransform * trans)
 *  @param [in] - trans xinfer's parents instance handle which will be type casted to xfilter instance
 *  @return TRUE on success
 *          FALSE on failure
 *  @brief This API will be invoked during PAUSED_TO_READY transition.
 *  @detail This function broadcast signals to PPE and INFER thread to exit and wait till
 *          both thread exit. All pending buffers and pool are freed up.
 */
static gboolean
gst_vvas_xinfer_stop (GstBaseTransform *trans)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (trans);
  GstVvas_XInferPrivate *priv = self->priv;

  GST_DEBUG_OBJECT (self, "sending stop to ppe and infer threads");
  self->priv->stop = TRUE;

  if (self->priv->post_proc->thread) {
    g_mutex_lock (&self->priv->post_proc->lock);
    g_cond_broadcast (&self->priv->post_proc->cond);
    GST_INFO_OBJECT (self, "signalled post process thread to exit");
    g_mutex_unlock (&self->priv->post_proc->lock);
  }

  if (self->priv->infer->thread) {
    g_mutex_lock (&self->priv->infer->lock);
    g_cond_broadcast (&self->priv->infer->cond);
    g_cond_broadcast (&self->priv->infer->batch_full);
    GST_INFO_OBJECT (self, "signalled infer thread to exit");
    g_mutex_unlock (&self->priv->infer->lock);

    /* Wake the infer thread if it is parked on the async drain barrier. */
    g_mutex_lock (&self->priv->infer->async_lock);
    g_cond_broadcast (&self->priv->infer->async_cond);
    g_mutex_unlock (&self->priv->infer->async_lock);
  }

  if (self->priv->pre_proc->thread) {
    g_mutex_lock (&self->priv->pre_proc->lock);
    self->priv->pre_proc->need_data = FALSE;
    g_cond_broadcast (&self->priv->pre_proc->has_input);
    g_cond_broadcast (&self->priv->pre_proc->need_input);
    GST_INFO_OBJECT (self, "signalled ppe thread to exit");
    g_mutex_unlock (&self->priv->pre_proc->lock);
  }

  if (self->priv->infer->thread) {
    GST_DEBUG_OBJECT (self, "waiting for inference thread to exit");
    g_thread_join (self->priv->infer->thread);
    GST_DEBUG_OBJECT (self, "inference thread exited");
    self->priv->infer->thread = NULL;
  }

  if (self->priv->post_proc->thread) {
    GST_DEBUG_OBJECT (self, "Waiting for Post Process thread to exit");
    g_thread_join (self->priv->post_proc->thread);
    self->priv->post_proc->thread = NULL;
    GST_DEBUG_OBJECT (self, "Post Process thread joined");
  }

  if (self->priv->pre_proc->thread) {
    GST_DEBUG_OBJECT (self, "waiting for ppe thread to exit");
    g_thread_join (self->priv->pre_proc->thread);
    GST_DEBUG_OBJECT (self, "ppe thread exited");
    self->priv->pre_proc->thread = NULL;
  }

  if (self->priv->infer_profiler.enabled) {
    vvas_infer_profiler_dump_json (&self->priv->infer_profiler,
        self->priv->instance_name);
    vvas_infer_profiler_deinit (&self->priv->infer_profiler);
  }

  g_mutex_lock (&priv->infer->lock);
  /* free all frames inside infer->batch_queue */
  if (priv->infer->batch_queue) {
    GST_INFO_OBJECT (self, "free infer batch queue of size %u",
        g_queue_get_length (priv->infer->batch_queue));

    g_queue_foreach (priv->infer->batch_queue,
        static_cast < GFunc > (vvas_xinfer_free_xinferframe), self);

    g_queue_free (priv->infer->batch_queue);
  }
  g_mutex_unlock (&priv->infer->lock);

  if (priv->post_proc->enabled) {
    g_mutex_lock (&priv->post_proc->lock);
    /* free all frames inside post process queue */
    if (priv->post_proc->queue) {
      GST_INFO_OBJECT (self, "free infer batch queue of size %u",
          g_queue_get_length (priv->post_proc->queue));

      g_queue_foreach (priv->post_proc->queue,
          static_cast < GFunc > (vvas_xinfer_free_xinferframe), self);

      g_queue_free (priv->post_proc->queue);
      priv->post_proc->queue = NULL;
    }
    g_mutex_unlock (&priv->post_proc->lock);
    g_mutex_clear (&priv->post_proc->lock);
    g_cond_clear (&priv->post_proc->cond);
  }

  g_mutex_clear (&priv->infer->lock);
  g_cond_clear (&priv->infer->cond);
  g_cond_clear (&priv->infer->batch_full);

  g_mutex_clear (&priv->infer->async_lock);
  g_cond_clear (&priv->infer->async_cond);

  /* buf inside infer->sub_buffers get freed when prediction
   * node get freed, so here just remove the queue */
  if (priv->infer->sub_buffers) {
    g_queue_free (priv->infer->sub_buffers);
  }

  if (priv->pre_proc->frame) {
    /* Free internal members of the PPE frame in case the PPE thread was
     * interrupted mid-frame (e.g. Ctrl+C) before it cleaned them up. */
    if (priv->pre_proc->frame->parent_vinfo) {
      gst_video_info_free (priv->pre_proc->frame->parent_vinfo);
      priv->pre_proc->frame->parent_vinfo = NULL;
    }
    if (priv->pre_proc->frame->child_vinfo) {
      gst_video_info_free (priv->pre_proc->frame->child_vinfo);
      priv->pre_proc->frame->child_vinfo = NULL;
    }
    if (priv->pre_proc->frame->vvas_frame) {
      vvas_video_frame_free (priv->pre_proc->frame->vvas_frame);
      priv->pre_proc->frame->vvas_frame = NULL;
    }
    g_slice_free1 (sizeof (Vvas_XInferFrame), priv->pre_proc->frame);
  }

  if (priv->pre_proc->outpool
      && gst_buffer_pool_is_active (priv->pre_proc->outpool)) {
    if (!gst_buffer_pool_set_active (priv->pre_proc->outpool, FALSE)) {
      GST_ERROR_OBJECT (self, "failed to deactivate preprocess output pool");
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("failed to deactivate pool."),
          ("failed to deactivate preprocess output pool"));
      return FALSE;
    }
  }

  if (priv->input_pool && gst_buffer_pool_is_active (priv->input_pool)) {
    if (!gst_buffer_pool_set_active (priv->input_pool, FALSE)) {
      GST_ERROR_OBJECT (self, "failed to deactivate PPE internal input pool");
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("failed to deactivate pool."),
          ("failed to deactivate PPE internal input pool"));
      return FALSE;
    }
  }

  if (self->priv->input_pool) {
    gst_object_unref (self->priv->input_pool);
    priv->input_pool = NULL;
  }

  if (priv->pre_proc->outpool) {
    gst_object_unref (priv->pre_proc->outpool);
    priv->pre_proc->outpool = NULL;
  }

  g_mutex_clear (&self->priv->pre_proc->lock);
  g_cond_clear (&self->priv->pre_proc->has_input);
  g_cond_clear (&self->priv->pre_proc->need_input);

  if (priv->pre_proc->buf_queue) {
    GstBuffer *buf;
    while ((buf =
            (GstBuffer *) g_queue_pop_head (priv->pre_proc->buf_queue)) !=
        NULL) {
      gst_buffer_unref (buf);
    }
    g_queue_free (priv->pre_proc->buf_queue);
    priv->pre_proc->buf_queue = NULL;
  }

  if (priv->pre_proc->out_vinfo) {
    gst_video_info_free (priv->pre_proc->out_vinfo);
    priv->pre_proc->out_vinfo = NULL;
  }

  return TRUE;
}

/**
 *  @fn static gboolean gst_vvas_xinfer_destroy (GstBaseTransform * trans)
 *  @param [in] - trans xinfer's parents instance handle which will be type casted to xfilter instance
 *  @return TRUE on success
 *          FALSE on failure
 *  @brief This API will be invoked during READY_TO_NULL transition to free up resources.
 *  @detail This function frees resources used for infer and preprocessing libraries.
 */
static gboolean
gst_vvas_xinfer_destroy (GstBaseTransform *trans)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (trans);
  GST_INFO_OBJECT (self, "destroy");

  gst_video_info_free (self->priv->in_vinfo);

#ifdef DUMP_INFER_INPUT
  if (self->priv->fp)
    fclose (self->priv->fp);
#endif
  vvas_xinfer_infer_deinit (self);

  if (self->priv->pre_proc->enabled)
    vvas_xinfer_ppe_deinit (self);

  if (self->priv->post_proc->enabled)
    vvas_xinfer_postproc_deinit (self);

  self->priv->do_init = TRUE;

  return TRUE;
}

/**
 *  @fn static void gst_vvas_xinfer_finalize (GObject * obj)
 *  @param [in] GObject - which will typecast to GstVvas_XInfer
 *  @return None
 *  @brief This API will be called during GstVvas_XInfer object's destruction phase.
 *         Close references to devices and free memories if any
 */
static void
gst_vvas_xinfer_finalize (GObject *obj)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (obj);

  if (self->config_file) {
    g_free (self->config_file);
    self->config_file = NULL;
  }

  if (self->priv) {
    self->priv->~GstVvas_XInferPrivate ();
  }

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static GstStateChangeReturn
gst_vvas_xinfer_change_state (GstElement *element, GstStateChange transition)
{
  GstVvas_XInfer *self = GST_VVAS_XINFER (element);
  GST_VVAS_LOG_SCOPE (self);
  GstStateChangeReturn ret;

  GST_DEBUG_OBJECT (self, "Got state change request: %s -> %s",
      gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
      gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:{
      if (!gst_vvas_xinfer_create (GST_BASE_TRANSFORM_CAST (element))) {
        GST_ERROR_OBJECT (self, "failed to do initialization");
        return GST_STATE_CHANGE_FAILURE;
      }
      break;
    }
    case GST_STATE_CHANGE_READY_TO_PAUSED:{
      if (!gst_vvas_xinfer_start (GST_BASE_TRANSFORM_CAST (element))) {
        GST_ERROR_OBJECT (self, "failed to allocate resources");
        return GST_STATE_CHANGE_FAILURE;
      }
      break;
    }
    case GST_STATE_CHANGE_PAUSED_TO_READY:{
      /* This is called before GstElement change_state as
       * there is a deadlock on GST_PAD_STREAM_LOCK used
       * to deactivate pads in this state transition.
       */
      if (!gst_vvas_xinfer_stop (GST_BASE_TRANSFORM_CAST (element))) {
        GST_ERROR_OBJECT (self, "failed to free resources");
        return GST_STATE_CHANGE_FAILURE;
      }

      break;
    }
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_NULL:{
      if (!gst_vvas_xinfer_destroy (GST_BASE_TRANSFORM_CAST (element))) {
        GST_ERROR_OBJECT (self, "failed to do de-initialization");
        return GST_STATE_CHANGE_FAILURE;
      }
      break;
    }
    default:
      break;
  }

  return ret;
}

/**
 *  @fn static void gst_vvas_xinfer_class_init (GstVvas_XInferClass * klass)
 *  @param [in] klass - Handle to GstVvas_XInferClass
 *  @return None
 *  @brief  Add properties and signals of GstVvas_XInfer to parent GObjectClass and ovverrides function
 *          pointers present in itself and/or its parent class structures
 *  @details This function publishes properties those can be set/get from application on
 *           GstVvas_XInfer object. And, while publishing a property it also declares type,
 *           range of acceptable values, default value, readability/writability and in which
 *           GStreamer state a property can be changed.
 */
static void
gst_vvas_xinfer_class_init (GstVvas_XInferClass *klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseTransformClass *transform_class;
  VvasImageProcessLibraryCapabilities *libs_caps = NULL;
  GstCaps *caps = NULL;

  gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);
  transform_class = GST_BASE_TRANSFORM_CLASS (klass);

  /* Get capabilities for all the image process libraries */
  libs_caps = vvas_image_process_get_all_capabilities ();
  if (!libs_caps || !libs_caps->num_libs) {
    GST_ERROR ("Couldn't get image process library capabilities");
  } else {
    caps = vvas_image_process_get_superset_caps_input (libs_caps, NULL);
  }

  /* Merge tensor formats into dynamic caps so they are always present
   * in pad template regardless of image processing library availability.
   * Tensor formats (BF16, FP16, FP32, RGBx, BGRx) are used for direct
   * inference when upstream provides model-ready data — these must not
   * be dropped when the image processing library scan succeeds.
   * Only add formats not already present in lib caps to avoid duplicates. */
  {
    GstCaps *tensor_caps =
        gst_caps_from_string ("video/x-raw, format=(string){RGBx, BGRx, "
        "RGBX_BF16_C4, BGRX_BF16_C4, RGBX_BF16_C8, "
        "RGB_BF16, BGR_BF16, "
        "RGB_BF16P, BGR_BF16P, "
        "RGBX_FP16_C4, BGRX_FP16_C4, RGBX_FP16_C8, RGBX8_C8, "
        "RGB_FP16, BGR_FP16, "
        "RGB_FP16P, BGR_FP16P, "
        "RGB_FLOAT, BGR_FLOAT, RGB_FLOATP, BGR_FLOATP, "
        "GRAY_BF16, GRAY_FP16, GRAY_FLOAT}, "
        "width=(int)[2,3840], height=(int)[2,2160], "
        "framerate=(fraction)[0/1,2147483647/1]");
    if (tensor_caps) {
      if (caps) {
        GstCaps *diff = gst_caps_subtract (tensor_caps, caps);
        if (diff && !gst_caps_is_empty (diff)) {
          gst_caps_append (caps, diff);
        } else if (diff) {
          gst_caps_unref (diff);
        }
        gst_caps_unref (tensor_caps);
      } else {
        caps = tensor_caps;
      }
    }
  }

  /* Free library capabilities */
  if (libs_caps) {
    for (uint8_t i = 0; i < libs_caps->num_libs; i++) {
      free (libs_caps->lib_caps[i]);
    }
    free (libs_caps->lib_caps);
    free (libs_caps);
  }

  gobject_class->set_property = gst_vvas_xinfer_set_property;
  gobject_class->get_property = gst_vvas_xinfer_get_property;
  gobject_class->finalize = gst_vvas_xinfer_finalize;
  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_vvas_xinfer_change_state);

  transform_class->set_caps = gst_vvas_xinfer_set_caps;
  transform_class->query = gst_vvas_xinfer_query;
  transform_class->sink_event = gst_vvas_xinfer_sink_event;
  transform_class->propose_allocation = gst_vvas_xinfer_propose_allocation;
  transform_class->submit_input_buffer = gst_vvas_xinfer_submit_input_buffer;
  transform_class->generate_output = gst_vvas_xinfer_generate_output;

  g_object_class_install_property (gobject_class, PROP_CONFIG_LOCATION,
      g_param_spec_string ("config-file",
          "Json config file path",
          "Location of the PPE/inference config file in json format "
          "(Note : Changable only in NULL state)", NULL,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_BATCH_SUBMIT_TIMEOUT,
      g_param_spec_uint ("batch-timeout",
          "timeout in milliseconds",
          "time (in milliseconds) to wait when batch is not full, before pushing batch of frames for inference."
          " By default infer waits indefinitely for batch to be formed", 0,
          UINT_MAX, DEFAULT_BATCH_SUBMIT_TIMEOUT,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_ENABLE_PROFILER,
      g_param_spec_boolean ("enable-profiler",
          "Enable profiling",
          "Enable or disable profiling for the inference process", FALSE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_PROFILER_LOG_INTERVAL,
      g_param_spec_uint ("profiler-log-interval",
          "Profiling log interval",
          "Interval (in seconds) for logging profiling information", 0,
          60, 0, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_PROFILER_FILE,
      g_param_spec_string ("profiler-file",
          "Profiler output file",
          "Path to the file where profiling information will be written in JSON format",
          NULL, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  gst_element_class_set_details_simple (gstelement_class,
      "VVAS Inference Plugin",
      "Inference/Video",
      "Performs Inference on Video", "AMD, Inc <https://www.amd.com>");

  if (caps) {
    GstPadTemplate *pad_templ;
    pad_templ =
        gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps);
    gst_element_class_add_pad_template (gstelement_class, pad_templ);

    pad_templ = gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps);
    gst_element_class_add_pad_template (gstelement_class, pad_templ);
    gst_caps_unref (caps);

  } else {
    gst_element_class_add_pad_template (gstelement_class,
        gst_static_pad_template_get (&src_template));
    gst_element_class_add_pad_template (gstelement_class,
        gst_static_pad_template_get (&sink_template));
  }

  /*
   * Will be emitted when kernel is successfully done.
   */
  vvas_signals[SIGNAL_VVAS] =
      g_signal_new ("vvas-kernel-done", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0,
      NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0, G_TYPE_NONE);

  GST_DEBUG_CATEGORY_INIT (gst_vvas_xinfer_debug, "vvas_xinfer", 0,
      "VVAS Inference plugin");
  GST_DEBUG_CATEGORY_GET (GST_CAT_PERFORMANCE, "GST_PERFORMANCE");

  _scale_quark = gst_video_meta_transform_scale_get_quark ();
  _copy_quark = g_quark_from_static_string ("gst-copy");

}

/**
 *  @fn static void gst_vvas_xinfer_init (GstVvas_XInfer * self)
 *  @param [in] self - Handle to GstVvas_XInfer instance
 *  @return None
 *  @brief  Initializes GstVvas_XInfer member variables to default.
 *          Also set pass-through and in_place mode for this filter by default
 */
static void
gst_vvas_xinfer_init (GstVvas_XInfer *self)
{
  GstBaseTransform *btrans = GST_BASE_TRANSFORM (self);
  GstVvas_XInferPrivate *priv = GST_VVAS_XINFER_PRIVATE (self);

  self->priv = new (priv) GstVvas_XInferPrivate {
  };

  priv->do_init = TRUE;
  priv->is_error = FALSE;
  self->batch_timeout = DEFAULT_BATCH_SUBMIT_TIMEOUT;
  priv->last_fret = GST_FLOW_OK;
#ifdef DUMP_INFER_INPUT
  priv->fp = NULL;
#endif
  memset (&priv->infer_profiler, 0, sizeof (VvasInferProfiler));
  gst_base_transform_set_in_place (GST_BASE_TRANSFORM (btrans), TRUE);
  gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (btrans), TRUE);
}

#ifndef PACKAGE
#define PACKAGE "vvas_xinfer"
#endif

/**
 *  @fn static gboolean plugin_init (GstPlugin * vvas_xinfer)
 *  @param [in] vvas_xinfer - Handle to plugin to register with GStreamer core
 *  @return TRUE on success
 *          FALSE on failure
 *  @brief Registers xinfer plugin with GStreamer core
 */
static gboolean
plugin_init (GstPlugin *vvas_xinfer)
{
  gst_vvas_log_bridge_install ();
  return gst_element_register (vvas_xinfer, "vvas_xinfer", GST_RANK_PRIMARY,
      GST_TYPE_VVAS_XINFER);
}

/**
 *  @def GST_PLUGIN_DEFINE
 *  @param [in] GST_VERSION_MAJOR - GStreamer major version with which xinfer is compiled
 *  @param [in] GST_VERSION_MINOR - GStreamer minor version with which xinfer is compiled
 *  @param [in] vvas_xinfer - Plugin name to be registered with GStreamer core
 *  @param [in] description - Purpose of the plugin
 *  @param [in] plugin_init - function pointer to the plugin_init method to be called to register xinfer
 *  @param [in] Version - of the plugin
 *  @param [in] Licence - of the plugin
 *  @param [in] Package - name
 *  @param [in] Package - Origin
 *  @return TRUE on success
 *          FALSE on failure
 *  @brief Entry point and meta data of a plugin to be exported to application
 */
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, vvas_xinfer,
    "GStreamer VVAS plug-in for inference", plugin_init, VVAS_API_VERSION,
    "MIT/X11", "AMD VVAS SDK", "https://www.amd.com/")
