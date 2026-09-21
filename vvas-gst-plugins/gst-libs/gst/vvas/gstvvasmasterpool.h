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

#ifndef __GST_VVAS_MASTER_POOL_H__
#define __GST_VVAS_MASTER_POOL_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/vvas/gstvvastilecompositioncoordinator.h>

G_BEGIN_DECLS

typedef struct _GstVvasMasterPool GstVvasMasterPool;
typedef struct _GstVvasMasterPoolClass GstVvasMasterPoolClass;
typedef struct _GstVvasMasterPoolPrivate GstVvasMasterPoolPrivate;

typedef struct _GstVvasMasterSlotSnapshot
{
  gboolean available;
  gboolean quarantined;
  gboolean reserved;
  gboolean transferred;
} GstVvasMasterSlotSnapshot;

#define GST_TYPE_VVAS_MASTER_POOL (gst_vvas_master_pool_get_type ())
#define GST_IS_VVAS_MASTER_POOL(obj)                                           \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_VVAS_MASTER_POOL))
#define GST_VVAS_MASTER_POOL(obj)                                              \
  (G_TYPE_CHECK_INSTANCE_CAST (                                                \
      (obj), GST_TYPE_VVAS_MASTER_POOL, GstVvasMasterPool))
#define GST_VVAS_MASTER_POOL_CAST(obj) ((GstVvasMasterPool *)(obj))

struct _GstVvasMasterPool
{
  GstVideoBufferPool parent;
  GstVvasMasterPoolPrivate *priv;
};

struct _GstVvasMasterPoolClass
{
  GstVideoBufferPoolClass parent_class;
};

GST_EXPORT
GType gst_vvas_master_pool_get_type (void) G_GNUC_CONST;

GST_EXPORT
GstBufferPool *gst_vvas_master_pool_new (
    GstVvasTileCompositionLayout *layout, guint max_slots);

GST_EXPORT
gboolean gst_vvas_master_pool_reserve_slot (
    GstVvasMasterPool *pool, const GstVvasTileCompositionIdentity *identity);

GST_EXPORT
void gst_vvas_master_pool_unreserve_slot (
    GstVvasMasterPool *pool, const GstVvasTileCompositionIdentity *identity);

GST_EXPORT
GstBuffer *gst_vvas_master_pool_ref_slot_buffer (
    GstVvasMasterPool *pool, const GstVvasTileCompositionIdentity *identity);

GST_EXPORT
GstBuffer *gst_vvas_master_pool_take_slot_buffer (
    GstVvasMasterPool *pool, const GstVvasTileCompositionIdentity *identity);

GST_EXPORT
GstVvasTileCompositionLayout *gst_vvas_master_pool_ref_layout (
    GstVvasMasterPool *pool);

GST_EXPORT
gboolean gst_vvas_master_pool_get_slot_snapshot (GstVvasMasterPool *pool,
    guint slot_id,
    GstVvasMasterSlotSnapshot *snapshot);

G_END_DECLS
#endif /* __GST_VVAS_MASTER_POOL_H__ */
