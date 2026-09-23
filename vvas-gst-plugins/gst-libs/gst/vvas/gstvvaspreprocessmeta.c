/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * Licensed under the MIT License.
 */

#include <string.h>

#include "gstvvaspreprocessmeta.h"

GType
gst_vvas_preprocess_meta_api_get_type (void)
{
  static GType type = 0;
  static const gchar *tags[] = { GST_META_TAG_VIDEO_STR, NULL };

  if (g_once_init_enter (&type)) {
    GType registered_type =
        gst_meta_api_type_register ("GstVvasPreprocessMetaAPI", tags);
    g_once_init_leave (&type, registered_type);
  }

  return type;
}

static gboolean
gst_vvas_preprocess_meta_init (GstMeta *meta, gpointer params,
    GstBuffer *buffer)
{
  GstVvasPreprocessMeta *preprocess_meta = (GstVvasPreprocessMeta *) meta;

  memset (&preprocess_meta->geometry, 0, sizeof (preprocess_meta->geometry));
  preprocess_meta->geometry.abi_version = GST_VVAS_PREPROCESS_META_ABI_VERSION;
  preprocess_meta->geometry.operation = GST_VVAS_PREPROCESS_OPERATION_UNKNOWN;

  return TRUE;
}

static gboolean
gst_vvas_preprocess_meta_transform (GstBuffer *dest, GstMeta *meta,
    GstBuffer *buffer, GQuark type, gpointer data)
{
  GstVvasPreprocessMeta *source_meta = (GstVvasPreprocessMeta *) meta;

  if (GST_META_TRANSFORM_IS_COPY (type)) {
    return gst_buffer_add_vvas_preprocess_meta (dest,
        &source_meta->geometry) != NULL;
  }

  GST_ERROR ("unsupported transform type: %s", g_quark_to_string (type));
  return FALSE;
}

const GstMetaInfo *
gst_vvas_preprocess_meta_get_info (void)
{
  static const GstMetaInfo *preprocess_meta_info = NULL;

  if (g_once_init_enter ((GstMetaInfo **) & preprocess_meta_info)) {
    const GstMetaInfo *meta_info =
        gst_meta_register (GST_VVAS_PREPROCESS_META_API_TYPE,
        "GstVvasPreprocessMeta",
        sizeof (GstVvasPreprocessMeta),
        gst_vvas_preprocess_meta_init, NULL,
        gst_vvas_preprocess_meta_transform);
    g_once_init_leave ((GstMetaInfo **) & preprocess_meta_info,
        (GstMetaInfo *) meta_info);
  }

  return preprocess_meta_info;
}

GstVvasPreprocessMeta *
gst_buffer_add_vvas_preprocess_meta (GstBuffer *buffer,
    const GstVvasPreprocessGeometry *geometry)
{
  GstVvasPreprocessMeta *preprocess_meta;

  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);

  preprocess_meta = (GstVvasPreprocessMeta *) gst_buffer_add_meta (buffer,
      GST_VVAS_PREPROCESS_META_INFO, NULL);
  if (!preprocess_meta)
    return NULL;

  if (geometry)
    preprocess_meta->geometry = *geometry;

  return preprocess_meta;
}
