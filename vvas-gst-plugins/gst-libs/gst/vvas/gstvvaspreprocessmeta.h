/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * Licensed under the MIT License.
 */

#ifndef __GST_VVAS_PREPROCESS_META_H__
#define __GST_VVAS_PREPROCESS_META_H__

#include <gst/gst.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

#define GST_VVAS_PREPROCESS_META_ABI_VERSION 1
#define GST_VVAS_PREPROCESS_META_API_TYPE \
  (gst_vvas_preprocess_meta_api_get_type ())
#define GST_VVAS_PREPROCESS_META_INFO \
  (gst_vvas_preprocess_meta_get_info ())

typedef enum {
  GST_VVAS_PREPROCESS_OPERATION_UNKNOWN = 0,
  GST_VVAS_PREPROCESS_OPERATION_STRETCH,
  GST_VVAS_PREPROCESS_OPERATION_LETTERBOX,
  GST_VVAS_PREPROCESS_OPERATION_PANSCAN,
} GstVvasPreprocessOperation;

typedef struct _GstVvasPreprocessRect GstVvasPreprocessRect;
typedef struct _GstVvasPreprocessGeometry GstVvasPreprocessGeometry;
typedef struct _GstVvasPreprocessMeta GstVvasPreprocessMeta;

struct _GstVvasPreprocessRect {
  gint x;
  gint y;
  guint width;
  guint height;
};

/*
 * Effective geometry used to produce a tensor frame. source_rect and
 * destination_rect are captured after image-process alignment and add_frame().
 * The inverse map is:
 *   source = (tensor - destination_rect.origin) *
 *            source_rect.size / destination_rect.size +
 *            source_rect.origin
 */
struct _GstVvasPreprocessGeometry {
  guint32 abi_version;
  GstVvasPreprocessOperation operation;
  guint source_frame_width;
  guint source_frame_height;
  guint destination_frame_width;
  guint destination_frame_height;
  GstVvasPreprocessRect source_rect;
  GstVvasPreprocessRect destination_rect;
};

struct _GstVvasPreprocessMeta {
  GstMeta meta;
  GstVvasPreprocessGeometry geometry;
};

GST_EXPORT
GType gst_vvas_preprocess_meta_api_get_type (void);

GST_EXPORT
const GstMetaInfo *gst_vvas_preprocess_meta_get_info (void);

GST_EXPORT
GstVvasPreprocessMeta *gst_buffer_add_vvas_preprocess_meta (
    GstBuffer *buffer, const GstVvasPreprocessGeometry *geometry);

#define gst_buffer_get_vvas_preprocess_meta(b) \
  ((GstVvasPreprocessMeta *) gst_buffer_get_meta ((b), \
      GST_VVAS_PREPROCESS_META_API_TYPE))

G_END_DECLS

#endif /* __GST_VVAS_PREPROCESS_META_H__ */
