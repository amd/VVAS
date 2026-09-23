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

#ifndef __GST_VVAS_TILE_COMPOSITION_LEASE_META_H__
#define __GST_VVAS_TILE_COMPOSITION_LEASE_META_H__

#include <gst/gst.h>
#include <gst/vvas/gstvvastilecompositioncoordinator.h>

G_BEGIN_DECLS
#define GST_VVAS_TILE_COMPOSITION_LEASE_META_API_TYPE                          \
  (gst_vvas_tile_composition_lease_meta_api_get_type ())
#define GST_VVAS_TILE_COMPOSITION_LEASE_META_INFO                              \
  (gst_vvas_tile_composition_lease_meta_get_info ())
typedef struct _GstVvasTileCompositionLeaseMeta GstVvasTileCompositionLeaseMeta;

struct _GstVvasTileCompositionLeaseMeta
{
  GstMeta meta;
  GstVvasTileCompositionLease *lease;
};

GST_EXPORT
GType gst_vvas_tile_composition_lease_meta_api_get_type (void);

GST_EXPORT
const GstMetaInfo *gst_vvas_tile_composition_lease_meta_get_info (
    void);

GST_EXPORT
GstVvasTileCompositionLeaseMeta *
gst_buffer_add_vvas_tile_composition_lease_meta (
    GstBuffer *buffer, GstVvasTileCompositionLease *lease);

GST_EXPORT
GstVvasTileCompositionLeaseMeta *
gst_buffer_get_vvas_tile_composition_lease_meta (GstBuffer *buffer);

G_END_DECLS
#endif /* __GST_VVAS_TILE_COMPOSITION_LEASE_META_H__ */
