/*
 * Copyright (C) 2022 Xilinx, Inc.  All rights reserved.
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <gst/gst.h>
#include <gst/vvas/gstvvasallocator.h>
#include <gst/vvas/gstvvasbufferpool.h>
#include <gst/allocators/gstdmabuf.h>
#include <time.h>
#include <vvas_core/vvas_device.h>
#ifdef XLNX_PCIe_PLATFORM
#include <experimental/xrt-next.h>
#else
#include <xrt/experimental/xrt-next.h>
#endif
#include <vvas/vvas_kernel.h>
#include <vvas_core/vvas_overlay.h>
#include "gstvvas_xoverlay.h"
#include <gst/vvas/gstvvasutils.h>
#include <gst/vvas/gstvvasoverlaymeta.h>
#include <gst/vvas/gstvvascoreutils.h>
#include <vvas_core/vvas_memory.h>
#include <vvas_core/vvas_memory_priv.h>

/** @def DEFAULT_KERNEL_NAME
 *  @brief Default kernel name of boundingbox IP
 */
#define DEFAULT_KERNEL_NAME "boundingbox_accel:{boundingbox_accel_1}"

/** @def DEFAULT_CLOCK_YOFFSET
 *  @brief Default y-offset of clock
 */
#define DEFAULT_CLOCK_YOFFSET 20

/** @def DEFAULT_CLOCK_FONTSCALE
 *  @brief Default fontscale of clock
 */
#define DEFAULT_CLOCK_FONTSCALE (0.5)

#ifdef XLNX_PCIe_PLATFORM
/**
 * @brief Default value of the device index in case user has not provided
 */
#define DEFAULT_DEVICE_INDEX -1
/**
 * @brief Default no dma buffer in PCIe
 */
#define USE_DMABUF 0
#else
/**
 * @brief On Embedded only one device i.e. device 0
 */
#define DEFAULT_DEVICE_INDEX 0
/**
 * @brief Default use dma buffer in Embedded
 */
#define USE_DMABUF 1
#endif

/** @def OVERLAY_FLOW_TIMEOUT
 *  @brief Default timeout for overlay bbox IP
 */
#define OVERLAY_BBOX_TIMEOUT 1000

/** @def MAX_BOXES
 *  @brief Maximum number of bounding boxes hardware can draw
 */
#define MAX_BOXES 5

/** @def DEFAULT_THICKNESS_LEVEL
 *  @brief default thickness of bounding boxes for hardware ip
 */
#define DEFAULT_THICKNESS_LEVEL 1

/**
 *  @brief Defines a static GstDebugCategory global variable "gst_vvas_xoverlay_debug"
 */
GST_DEBUG_CATEGORY_STATIC (gst_vvas_xoverlay_debug);

/** @def GST_CAT_DEFAULT
 *  @brief Setting gst_vvas_xoverlay_debug as default debug category for logging
 */
#define GST_CAT_DEFAULT gst_vvas_xoverlay_debug
/**
 *  @brief Defines a static GstDebugCategory global variable with name GST_CAT_PERFORMANCE for
 *         performance logging purpose
 */
GST_DEBUG_CATEGORY_STATIC (GST_CAT_PERFORMANCE);

/**
 * @brief Default width align for Edge
 */
#define WIDTH_ALIGN 1
/**
 * @brief Default height align for Edge
 */
#define HEIGHT_ALIGN 1

typedef struct _GstVvas_XOverlayPrivate GstVvas_XOverlayPrivate;

enum
{
  /** default */
  PROP_0,
  /** bool property to show clock on frame or not */
  PROP_DISPLAY_CLOCK,
  /** Font name from opencv to be used for clock display */
  PROP_CLOCK_FONT_NAME,
  /** color to be used for clock display */
  PROP_CLOCK_FONT_COLOR,
  /** Font scale to be used for clock display */
  PROP_CLOCK_FONT_SCALE,
  /** Column start point of clock display */
  PROP_CLOCK_X_OFFSET,
  /** Row start point of clock display */
  PROP_CLOCK_Y_OFFSET,
};

/**
 *  @brief Defines sink pad template
 */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{NV12, RGB, BGR, GRAY8, BGRA, RGBA}")));

/**
 *  @brief Defines source pad template
 */
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{NV12, RGB, BGR, GRAY8, BGRA, RGBA}")));


/** @struct vvas_bbox_acc_roi
 *  @brief  Holds number of bboxes and their info
 */
typedef struct _vvas_bbox_acc_roi
{
  /** Number of bbox to be draw */
  uint32_t nobj;
  /** bbox info as xrt_buffer */
  VvasMemory *roi;
} vvas_bbox_acc_roi;

/** @struct _GstVvas_XOverlayPrivate
 *  @brief  Holds private members related overlay
 */
struct _GstVvas_XOverlayPrivate
{
  /** Pointer to the info of input caps */
  GstVideoInfo *in_vinfo;
  /** flag to display clock or not  */
  gboolean display_clock;
  /** To store clock data as string */
  gchar clock_time_string[256];
  /** display clock font name from opencv */
  gint clock_font_name;
  /** display clock font scale */
  gfloat clock_font_scale;
  /** display clock font color */
  guint clock_font_color;
  /** display clock column start position in frame */
  gint clock_x_offset;
  /** display clock row start position in frame */
  gint clock_y_offset;
  /** To store bbox information for bbox hardware IP */
  vvas_bbox_acc_roi roi_data;
  /** VVAS context handle  */
  VvasContext *vvas_ctx;
  /** Overlay Context */
  VvasOverlay *vvas_overlay;

};

#define gst_vvas_xoverlay_parent_class parent_class

/** @brief  Glib's convenience macro for GstVvas_XOverlay type implementation.
 *  @details This macro does below tasks:\n
 *           - Declares a class initialization function with prefix gst_vvas_xoverlay
 */
G_DEFINE_TYPE_WITH_PRIVATE (GstVvas_XOverlay, gst_vvas_xoverlay,
    GST_TYPE_BASE_TRANSFORM);

/** @def GST_VVAS_XOVERLAY_PRIVATE(self)
 *  @brief Get instance of GstVvas_XOverlayPrivate structure
 */
#define GST_VVAS_XOVERLAY_PRIVATE(self) (GstVvas_XOverlayPrivate *) (gst_vvas_xoverlay_get_instance_private (self))

/* Functions declaration */
static void gst_vvas_xoverlay_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);

static void gst_vvas_xoverlay_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean
gst_vvas_xoverlay_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstVvas_XOverlay *self = GST_VVAS_XOVERLAY (trans);
  gboolean bret = TRUE;
  GstVvas_XOverlayPrivate *priv = self->priv;

  GST_INFO_OBJECT (self,
      "incaps = %" GST_PTR_FORMAT "and outcaps = %" GST_PTR_FORMAT, incaps,
      outcaps);

  /* Update in_vinfo from input caps */
  if (!gst_video_info_from_caps (priv->in_vinfo, incaps)) {
    GST_ERROR_OBJECT (self, "Failed to parse input caps");
    return FALSE;
  }

  return bret;
}

/**
 *  @fn gboolean gst_vvas_xoverlay_start (GstBaseTransform * trans)
 *  @param [in] trans - Pointer to GstBaseTransform object.
 *  @return TRUE on success \n
 *          FALSE on failure
 *  @brief  Opens device handle and creates context for XRT.
 *  @details This API is registered with GObjectClass by overriding GstBaseTransform::start function pointer and
 *          this will be called when element start processing. It opens device context and allocates memory.
 */
static gboolean
gst_vvas_xoverlay_start (GstBaseTransform * trans)
{
  GstVvas_XOverlay *self = GST_VVAS_XOVERLAY (trans);
  GstVvas_XOverlayPrivate *priv = self->priv;
  VvasReturnType vret;

  self->priv = priv;
  priv->in_vinfo = gst_video_info_new ();

  VvasLogLevel core_log_level =
      vvas_get_core_log_level (gst_debug_category_get_threshold
      (gst_vvas_xoverlay_debug));

  /* create a SW vvas context */
  priv->vvas_ctx = vvas_context_create (-1, NULL, core_log_level, &vret);
  if (vret != VVAS_RET_SUCCESS) {
    GST_ERROR_OBJECT (self, "Couldn't create VVAS context");
    return FALSE;
  }

  /* Create Core Overlay */
  priv->vvas_overlay = vvas_overlay_create (priv->vvas_ctx, core_log_level);
  if (!priv->vvas_overlay) {
    GST_ERROR_OBJECT (self, "Couldn't create Core Overlay");
    return FALSE;
  }

  GST_INFO_OBJECT (self, "start completed");
  return TRUE;
}

/**
 *  @fn gboolean gst_vvas_xoverlay_stop (GstBaseTransform * trans)
 *  @param [in] trans - Pointer to GstBaseTransform object.
 *  @return TRUE on success \n
 *          FALSE on failure
 *  @brief  Free up allocates memory and invokes vvas_xoverlay_deinit.
 *  @details This API is registered with GObjectClass by overriding GstBaseTransform::stop function pointer and
 *          this will be called when element stops processing.
 *          It invokes vvas_xoverlay_deinit to free up allocated memory.
 *
 */
static gboolean
gst_vvas_xoverlay_stop (GstBaseTransform * trans)
{
  GstVvas_XOverlay *self = GST_VVAS_XOVERLAY (trans);
  GstVvas_XOverlayPrivate *priv = self->priv;
  GST_DEBUG_OBJECT (self, "stopping");

  if (self->priv->in_vinfo) {
    gst_video_info_free (self->priv->in_vinfo);
    self->priv->in_vinfo = NULL;
  }

  if (priv->vvas_overlay) {
    vvas_overlay_destroy (priv->vvas_overlay);
    priv->vvas_overlay = NULL;
  }

  if (priv->vvas_ctx) {
    vvas_context_destroy (priv->vvas_ctx);
    priv->vvas_ctx = NULL;
  }

  return TRUE;
}

/**
 *  @fn static void gst_vvas_xoverlay_finalize (GObject * obj)
 *  @param [in] Handle to GstVvas_XOverlay typecast to GObject
 *  @return None
 *  @brief This API will be called during GstVvas_XOverlay object's destruction phase. Close references
 *         to devices and free memories if any
 *  @note After this API GstVvas_XOverlay object \p obj will be destroyed completely. So free all internal
 *        memories held by current object
 */
static void
gst_vvas_xoverlay_finalize (GObject * obj)
{
  GstVvas_XOverlay *self = GST_VVAS_XOVERLAY (obj);

  self->priv->display_clock = 0;

  G_OBJECT_CLASS (gst_vvas_xoverlay_parent_class)->finalize (obj);
}

/**
 *  @fn void prepare_bbox_data (GstVvas_XOverlay * self,
 *          GstVvasOverlayMeta * overlay_meta, GstVideoFormat gst_fmt, gint32 str_idx)
 *  @param [inout] self - Handle to GstVvas_XOverlay instance
 *  @param [in] overlay_meta - Pointer to metadata type GstVvasOverlayMeta
 *  @param [in] gst_fmt - Gstreamer based frame format
 *  @param [in] str_idx - Starting point in a array bboxes to draw on frame
 *  @return None
 *
 *  @brief  This API converts bounding box metadata from overlay meta to
 *          sequence data as per expectations of bbox accelerator.
 */
void
prepare_bbox_data (GstVvas_XOverlay * self,
    GstVvasOverlayMeta * overlay_meta, GstVideoFormat gst_fmt, guint32 str_idx)
{
  guint32 idx, end_idx, loop;
  GstVvas_XOverlayPrivate *priv = self->priv;
  VvasOverlayRectParams *rect_params;
  gint32 *roi;
  VvasList *head;
  VvasMemoryMapInfo roi_buf_info;
  VvasReturnType vret;

  /* map roi_buf to get user space address */
  vret = vvas_memory_map (priv->roi_data.roi,
             VVAS_DATA_MAP_WRITE, &roi_buf_info);
  if (vret != VVAS_RET_SUCCESS) {
    GST_ERROR_OBJECT (self,
        "ERROR: Failed to map the roi_buf for write vret = %d", vret);
    return;
  }
  roi = (gint32 *)roi_buf_info.data;

  /* check if number of bbox are less or more than MAX_BOXES */
  if ((str_idx + MAX_BOXES) > overlay_meta->shape_info.num_rects)
    end_idx = overlay_meta->shape_info.num_rects;
  else
    end_idx = str_idx + MAX_BOXES;

  priv->roi_data.nobj = 0;
  idx = 1;

  /* while loop to get VvasList pointer of nth element */
  head = overlay_meta->shape_info.rect_params;
  while (idx != str_idx && head) {
    idx++;
    head = head->next;
  }
  /* for loop to read bounding box info from starting at str_idx to end_idx */
  for (loop = str_idx; (loop < end_idx) && (head != NULL); loop++) {
    rect_params = (VvasOverlayRectParams *) head->data;
    roi[priv->roi_data.nobj * 5] = rect_params->points.x;
    roi[(priv->roi_data.nobj * 5) + 1] = rect_params->points.y;
    roi[(priv->roi_data.nobj * 5) + 2] = rect_params->width;
    roi[(priv->roi_data.nobj * 5) + 3] = rect_params->height;

    if (gst_fmt == GST_VIDEO_FORMAT_RGB) {
      roi[(priv->roi_data.nobj * 5) + 4] =
          (rect_params->rect_color.red & 0xFF) << 24;
      roi[(priv->roi_data.nobj * 5) + 4] |=
          (rect_params->rect_color.green & 0xFF) << 16;
      roi[(priv->roi_data.nobj * 5) + 4] |=
          (rect_params->rect_color.blue & 0xFF) << 8;
      roi[(priv->roi_data.nobj * 5) + 4] |=
          (rect_params->rect_color.alpha & 0xFF);
    } else {
      roi[(priv->roi_data.nobj * 5) + 4] =
          (rect_params->rect_color.blue & 0xFF) << 24;
      roi[(priv->roi_data.nobj * 5) + 4] |=
          (rect_params->rect_color.green & 0xFF) << 16;
      roi[(priv->roi_data.nobj * 5) + 4] |=
          (rect_params->rect_color.red & 0xFF) << 8;
      roi[(priv->roi_data.nobj * 5) + 4] |=
          (rect_params->rect_color.alpha & 0xFF);
    }
    head = head->next;
    priv->roi_data.nobj++;
  }
  vvas_memory_unmap (priv->roi_data.roi, &roi_buf_info);
}

/**
 *  @fn gboolean gst_vvas_xoverlay_generate_output (GstBaseTransform * base, GstBuffer ** outbuf)
 *  @param [in] base - Pointer to GstBaseTransform object.
 *  @param [out] outbuf - Pointer to output buffer of type GstBuffer.
 *  @return TRUE on success \n
 *          FALSE on failure
 *  @brief  This API to draw overlay metadata on frames.
 *  @details This API is registered with GObjectClass by overriding GstBaseTransform::generate_output
 *           function pointer and this will be called for every frame. Bases on overlay metadata it
 *           draws different geometric shapes, text and clock on frames.
 */
static GstFlowReturn
gst_vvas_xoverlay_generate_output (GstBaseTransform * trans,
    GstBuffer ** outbuf)
{
  GstVvas_XOverlay *self = GST_VVAS_XOVERLAY (trans);
  GstVvas_XOverlayPrivate *priv = self->priv;
  GstFlowReturn fret = GST_FLOW_OK;
  GstVvasOverlayMeta *overlay_meta;
  GstMapFlags map_flags;
  GstBuffer *inbuf = NULL;
  VvasOverlayFrameInfo *ovlinfo = NULL;
  VvasVideoFrame *vframe = NULL;

  inbuf = trans->queued_buf;
  trans->queued_buf = NULL;

  if (inbuf == NULL)
    return GST_FLOW_OK;

  GST_DEBUG_OBJECT (self, "received buffer %" GST_PTR_FORMAT, inbuf);

  /* Read overlay metadata from inbuf */
  overlay_meta = gst_buffer_get_vvas_overlay_meta (inbuf);

  /* If no overlay metadata return without further processing */
  if (!overlay_meta) {
    *outbuf = inbuf;
    GST_LOG_OBJECT (self, "unable to get overlaymeta from input buffer");
    return GST_FLOW_OK;
  }

  /*Allocate memory for ovlerlay */
  ovlinfo = (VvasOverlayFrameInfo *) calloc (1, sizeof (VvasOverlayFrameInfo));

  map_flags =
      (GstMapFlags) (GST_MAP_READ | GST_VIDEO_FRAME_MAP_FLAG_NO_REF |
      GST_MAP_WRITE);
  /* get vvasframe form gst buffer */
  vframe = vvas_videoframe_from_gstbuffer (priv->vvas_ctx, DEFAULT_MEM_BANK,
      inbuf, self->priv->in_vinfo, map_flags);
  if (NULL == vframe) {
    GST_ERROR_OBJECT (self, "Cannot convert input GstBuffer to VvasVideoFrame");
    fret = GST_FLOW_ERROR;
    goto error;
  }

  /* Update video frame */
  ovlinfo->frame_info = vframe;

  /* Update shape info */
  memcpy (&ovlinfo->shape_info, &overlay_meta->shape_info,
      sizeof (VvasOverlayShapeInfo));

  /* update clock data */
  ovlinfo->clk_info.display_clock = self->priv->display_clock;
  ovlinfo->clk_info.clock_font_name = self->priv->clock_font_name;
  ovlinfo->clk_info.clock_font_scale = self->priv->clock_font_scale;
  ovlinfo->clk_info.clock_font_color = self->priv->clock_font_color;
  ovlinfo->clk_info.clock_x_offset = self->priv->clock_x_offset;
  ovlinfo->clk_info.clock_y_offset = self->priv->clock_y_offset;

  /* draw requested pattern on the image */
  if (VVAS_RET_SUCCESS == vvas_overlay_process_frame (priv->vvas_overlay, ovlinfo)) {
    GST_DEBUG_OBJECT (self, "ovl process ret success");
  } else {
    /*do we need to update fret here ?  */
    GST_DEBUG_OBJECT (self, "ovl process failure");
  }

  vvas_video_frame_free (vframe);
  free (ovlinfo);

  *outbuf = inbuf;
  return fret;

error:
  if (inbuf)
    *outbuf = inbuf;
  if (ovlinfo)
    free (ovlinfo);
  return fret;
}

/**
 *  @fn static void gst_vvas_xoverlay_class_init (GstVvas_XOverlayClass * klass)
 *  @param [in]klass  - Handle to GstVvas_XOverlayClass
 *  @return None
 *  @brief  Add properties and signals of GstVvas_XOverlay to parent GObjectClass \n
 *          and overrides function pointers present in itself and/or its parent class structures
 *  @details This function publishes properties those can be set/get from application on GstVvas_XOverlay object.
 *           And, while publishing a property it also declares type, range of acceptable values, default value,
 *           readability/writability and in which GStreamer state a property can be changed.
 */
static void
gst_vvas_xoverlay_class_init (GstVvas_XOverlayClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseTransformClass *transform_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);
  transform_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->set_property = gst_vvas_xoverlay_set_property;
  gobject_class->get_property = gst_vvas_xoverlay_get_property;
  transform_class->set_caps = gst_vvas_xoverlay_set_caps;
  gobject_class->finalize = gst_vvas_xoverlay_finalize;

  transform_class->start = gst_vvas_xoverlay_start;
  transform_class->stop = gst_vvas_xoverlay_stop;
  transform_class->generate_output = gst_vvas_xoverlay_generate_output;

  g_object_class_install_property (gobject_class, PROP_DISPLAY_CLOCK,
      g_param_spec_boolean ("display-clock", "display clock flag",
          "flag to display time stamp on frames", 0,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_CLOCK_FONT_NAME,
      g_param_spec_uint ("clock-fontname", "clock display font number",
          "font number for displaying time stamp as given in opencv", 0,
          7, 0, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_CLOCK_FONT_SCALE,
      g_param_spec_float ("clock-fontscale", "clock display font size",
          "font size to be used for displaying time stamp on frames in pixels",
          0, 1.0, 0.5,
          (GParamFlags) (G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
              G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_CLOCK_FONT_COLOR,
      g_param_spec_uint ("clock-fontcolor", "clock display font color",
          "font color to be used for displaying time stamp on frames as rgba",
          0, 4294967295, 0,
          (GParamFlags) (G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
              G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_CLOCK_X_OFFSET,
      g_param_spec_uint ("clock-xoffset", "clock x offset location",
          "column location of displaying time stamp on frame", 0, G_MAXUINT,
          0, (GParamFlags) (G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
              G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_CLOCK_Y_OFFSET,
      g_param_spec_uint ("clock-yoffset", "clock y offset location",
          "row location of displaying time stamp on frame",
          0, G_MAXUINT, DEFAULT_CLOCK_YOFFSET,
          (GParamFlags) (G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
              G_PARAM_STATIC_STRINGS)));

  gst_element_class_set_details_simple (gstelement_class,
      "VVAS Generic Overlay Plugin",
      "Filter/Effect/Video",
      "Renders Overlay info using VVAS Core Overlay library",
      "AMD, Inc <https://www.amd.com>");
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_template));

  GST_DEBUG_CATEGORY_INIT (gst_vvas_xoverlay_debug, "vvas_xoverlay", 0,
      "VVAS optical flow plugin");
  GST_DEBUG_CATEGORY_GET (GST_CAT_PERFORMANCE, "GST_PERFORMANCE");
}

/**
 *  @fn static void gst_vvas_xoverlay_init (GstVvas_XOverlay * self)
 *  @param [in] self  - Handle to GstVvas_XOverlay instance
 *  @return None
 *  @brief  Initilizes GstVvas_XOverlay member variables to default values
 *
 */
static void
gst_vvas_xoverlay_init (GstVvas_XOverlay * self)
{
  GstBaseTransform *btrans = GST_BASE_TRANSFORM (self);
  GstVvas_XOverlayPrivate *priv = GST_VVAS_XOVERLAY_PRIVATE (self);

  self->priv = priv;
  self->priv->display_clock = 0;
  self->priv->clock_y_offset = DEFAULT_CLOCK_YOFFSET;
  self->priv->clock_font_scale = DEFAULT_CLOCK_FONTSCALE;
  gst_base_transform_set_in_place (GST_BASE_TRANSFORM (btrans), TRUE);
  gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (btrans), TRUE);
}

/**
 *  @fn static void gst_vvas_xoverlay_set_property (GObject * object, guint prop_id,
 *                                                  const GValue * value, GParamSpec * pspec)
 *  @param [in] object - Handle to GstVvas_XOverlay typecast to GObject
 *  @param [in] prop_id - Property ID as defined in enum
 *  @param [in] value - GValue which holds property value set by user
 *  @param [in] pspec - Handle to metadata of a property with property ID \p prop_id
 *  @return None
 *  @brief This API stores values sent from the user in GstVvas_XOverlay object members.
 *  @details This API is registered with GObjectClass by overriding GObjectClass::set_property function pointer and
 *           this will be invoked when developer sets properties on GstVvas_XOverlay object.
 *           Based on property value type, corresponding g_value_get_xxx API will be called to get 
 *           property value from GValue handle.
 */
static void
gst_vvas_xoverlay_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVvas_XOverlay *self = GST_VVAS_XOVERLAY (object);

  switch (prop_id) {
    case PROP_DISPLAY_CLOCK:
      self->priv->display_clock = g_value_get_boolean (value);
      break;
    case PROP_CLOCK_FONT_NAME:
      self->priv->clock_font_name = g_value_get_uint (value);
      break;
    case PROP_CLOCK_FONT_SCALE:
      self->priv->clock_font_scale = g_value_get_float (value);
      break;
    case PROP_CLOCK_FONT_COLOR:
      self->priv->clock_font_color = g_value_get_uint (value);
      break;
    case PROP_CLOCK_X_OFFSET:
      self->priv->clock_x_offset = g_value_get_uint (value);
      break;
    case PROP_CLOCK_Y_OFFSET:
      self->priv->clock_y_offset = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/**
 *  @fn static void gst_vvas_xoverlay_get_property (GObject * object, guint prop_id,
 *                                                  const GValue * value, GParamSpec * pspec)
 *  @param [in] object - Handle to GstVvas_XOverlay typecast to GObject
 *  @param [in] prop_id - Property ID as defined in properties enum
 *  @param [in] value - value GValue which holds property value set by user
 *  @param [in] pspec - Handle to metadata of a property with property ID \p prop_id
 *  @return None
 *  @brief This API gets values from GstVvas_XOverlay object members.
 *  @details This API is registered with GObjectClass by overriding GObjectClass::get_property function pointer and
 *           this will be invoked when developer want gets properties from GstVvas_XOverlay object.
 *           Based on property value type,corresponding g_value_set_xxx API will be called to set value of GValue type.
 */
static void
gst_vvas_xoverlay_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstVvas_XOverlay *self = GST_VVAS_XOVERLAY (object);

  switch (prop_id) {
    case PROP_DISPLAY_CLOCK:
      g_value_set_boolean (value, self->priv->display_clock);
      break;
    case PROP_CLOCK_FONT_NAME:
      g_value_set_uint (value, self->priv->clock_font_name);
      break;
    case PROP_CLOCK_FONT_SCALE:
      g_value_set_float (value, self->priv->clock_font_scale);
      break;
    case PROP_CLOCK_FONT_COLOR:
      g_value_set_uint (value, self->priv->clock_font_color);
      break;
    case PROP_CLOCK_X_OFFSET:
      g_value_set_uint (value, self->priv->clock_x_offset);
      break;
    case PROP_CLOCK_Y_OFFSET:
      g_value_set_uint (value, self->priv->clock_y_offset);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

#ifndef PACKAGE
#define PACKAGE "vvas_xoverlay"
#endif

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
plugin_init (GstPlugin * vvas_xoverlay)
{
  return gst_element_register (vvas_xoverlay, "vvas_xoverlay", GST_RANK_PRIMARY,
      GST_TYPE_VVAS_XOVERLAY);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    vvas_xoverlay,
    "GStreamer VVAS plug-in for overlaying display text and geometric shapes",
    plugin_init, VVAS_API_VERSION, "MIT/X11", "AMD VVAS SDK",
    "https://www.amd.com/")
