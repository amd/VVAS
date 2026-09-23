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

#include "gstvvastilecompositionleasemeta.h"

GType
gst_vvas_tile_composition_lease_meta_api_get_type (void)
{
  static GType type = 0;
  static const gchar *tags[] = {
    GST_META_TAG_MEMORY_STR,
    GST_META_TAG_MEMORY_REFERENCE_STR,
    NULL
  };

  if (g_once_init_enter (&type)) {
    GType registered =
        gst_meta_api_type_register ("GstVvasTileCompositionLeaseMetaAPI", tags);

    g_once_init_leave (&type, registered);
  }
  return type;
}

static gboolean
gst_vvas_tile_composition_lease_meta_init (GstMeta * meta, gpointer params,
    GstBuffer * buffer)
{
  GstVvasTileCompositionLeaseMeta *lease_meta =
      (GstVvasTileCompositionLeaseMeta *) meta;

  (void) params;
  (void) buffer;
  lease_meta->lease = NULL;
  return TRUE;
}

static void
gst_vvas_tile_composition_lease_meta_free (GstMeta * meta, GstBuffer * buffer)
{
  GstVvasTileCompositionLeaseMeta *lease_meta =
      (GstVvasTileCompositionLeaseMeta *) meta;

  (void) buffer;
  if (lease_meta->lease) {
    gst_vvas_tile_composition_lease_unref (lease_meta->lease);
    lease_meta->lease = NULL;
  }
}

static gboolean
gst_vvas_tile_composition_buffers_share_all_memory (GstBuffer * first,
    GstBuffer * second)
{
  guint n_memory;
  guint i;

  n_memory = gst_buffer_n_memory (first);
  if (!n_memory || gst_buffer_n_memory (second) != n_memory)
    return FALSE;

  for (i = 0; i < n_memory; i++) {
    if (gst_buffer_peek_memory (first, i) != gst_buffer_peek_memory (second, i))
      return FALSE;
  }
  return TRUE;
}

static gboolean
gst_vvas_tile_composition_lease_meta_transform (GstBuffer * dest,
    GstMeta * meta, GstBuffer * src, GQuark type, gpointer data)
{
  GstVvasTileCompositionLeaseMeta *lease_meta =
      (GstVvasTileCompositionLeaseMeta *) meta;
  GstMetaTransformCopy *copy;

  if (!GST_META_TRANSFORM_IS_COPY (type) || !data)
    return TRUE;

  copy = (GstMetaTransformCopy *) data;
  if (copy->region
      || !gst_vvas_tile_composition_buffers_share_all_memory (src, dest))
    return TRUE;
  if (gst_buffer_get_vvas_tile_composition_lease_meta (dest))
    return TRUE;

  return gst_buffer_add_vvas_tile_composition_lease_meta (dest,
      lease_meta->lease) != NULL;
}

const GstMetaInfo *
gst_vvas_tile_composition_lease_meta_get_info (void)
{
  static const GstMetaInfo *info = NULL;

  if (g_once_init_enter ((GstMetaInfo **) & info)) {
    const GstMetaInfo *registered =
        gst_meta_register (GST_VVAS_TILE_COMPOSITION_LEASE_META_API_TYPE,
        "GstVvasTileCompositionLeaseMeta",
        sizeof (GstVvasTileCompositionLeaseMeta),
        gst_vvas_tile_composition_lease_meta_init,
        gst_vvas_tile_composition_lease_meta_free,
        gst_vvas_tile_composition_lease_meta_transform);

    g_once_init_leave ((GstMetaInfo **) & info, (GstMetaInfo *) registered);
  }
  return info;
}

GstVvasTileCompositionLeaseMeta *
gst_buffer_add_vvas_tile_composition_lease_meta (GstBuffer * buffer,
    GstVvasTileCompositionLease * lease)
{
  GstVvasTileCompositionLeaseMeta *meta;

  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);
  g_return_val_if_fail (lease != NULL, NULL);
  if (gst_buffer_get_vvas_tile_composition_lease_meta (buffer))
    return NULL;

  meta = (GstVvasTileCompositionLeaseMeta *) gst_buffer_add_meta (buffer,
      GST_VVAS_TILE_COMPOSITION_LEASE_META_INFO, NULL);
  if (!meta)
    return NULL;
  meta->lease = gst_vvas_tile_composition_lease_ref (lease);
  return meta;
}

GstVvasTileCompositionLeaseMeta *
gst_buffer_get_vvas_tile_composition_lease_meta (GstBuffer * buffer)
{
  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);
  return (GstVvasTileCompositionLeaseMeta *) gst_buffer_get_meta (buffer,
      GST_VVAS_TILE_COMPOSITION_LEASE_META_API_TYPE);
}
