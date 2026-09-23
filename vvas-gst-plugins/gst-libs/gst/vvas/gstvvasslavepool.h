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

#ifndef __GST_VVAS_SLAVE_POOL_H__
#define __GST_VVAS_SLAVE_POOL_H__

#include <gst/gst.h>
#include <gst/video/gstvideopool.h>
#include <gst/vvas/gstvvasmasterpool.h>

G_BEGIN_DECLS

typedef struct _GstVvasSlavePool GstVvasSlavePool;
typedef struct _GstVvasSlavePoolClass GstVvasSlavePoolClass;
typedef struct _GstVvasSlavePoolPrivate GstVvasSlavePoolPrivate;

#define GST_TYPE_VVAS_SLAVE_POOL (gst_vvas_slave_pool_get_type ())
#define GST_IS_VVAS_SLAVE_POOL(obj)                                            \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_VVAS_SLAVE_POOL))
#define GST_VVAS_SLAVE_POOL(obj)                                               \
  (G_TYPE_CHECK_INSTANCE_CAST (                                                \
      (obj), GST_TYPE_VVAS_SLAVE_POOL, GstVvasSlavePool))
#define GST_VVAS_SLAVE_POOL_CAST(obj) ((GstVvasSlavePool *)(obj))

struct _GstVvasSlavePool
{
  GstVideoBufferPool parent;
  GstVvasSlavePoolPrivate *priv;
};

struct _GstVvasSlavePoolClass
{
  GstVideoBufferPoolClass parent_class;
};

GST_EXPORT
GType gst_vvas_slave_pool_get_type (void) G_GNUC_CONST;

GST_EXPORT
GstBufferPool *gst_vvas_slave_pool_new (
    GstVvasTileCompositionCoordinator *coordinator,
    GstVvasMasterPool *master_pool,
    GstVvasTileCompositionLayout *layout,
    guint tile_id);

G_END_DECLS
#endif /* __GST_VVAS_SLAVE_POOL_H__ */
