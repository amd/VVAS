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

/*
 * Aggregates tile buffers that share one tile composition identity and
 * publishes the associated master buffer after all required tiles are ready.
 */

#ifndef __GST_VVAS_XTILECOMPOSITOR_H__
#define __GST_VVAS_XTILECOMPOSITOR_H__

#include <gst/base/gstaggregator.h>
#include <gst/gst.h>

G_BEGIN_DECLS
#define GST_TYPE_VVAS_XTILECOMPOSITOR_PAD                                      \
  (gst_vvas_xtilecompositor_pad_get_type ())
#define GST_VVAS_XTILECOMPOSITOR_PAD(obj)                                      \
  (G_TYPE_CHECK_INSTANCE_CAST (                                                \
      (obj), GST_TYPE_VVAS_XTILECOMPOSITOR_PAD, GstVvas_XTileCompositorPad))
#define GST_VVAS_XTILECOMPOSITOR_PAD_CAST(obj)                                 \
  ((GstVvas_XTileCompositorPad *)(obj))
#define GST_VVAS_XTILECOMPOSITOR_PAD_CLASS(klass)                              \
  (G_TYPE_CHECK_CLASS_CAST (                                                   \
      (klass), GST_TYPE_VVAS_XTILECOMPOSITOR_PAD, GstVvas_XTileCompositorPad))
#define GST_IS_VVAS_XTILECOMPOSITOR_PAD(obj)                                   \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_VVAS_XTILECOMPOSITOR_PAD))
#define GST_IS_VVAS_XTILECOMPOSITOR_PAD_CLASS(klass)                           \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_VVAS_XTILECOMPOSITOR_PAD))
typedef struct _GstVvas_XTileCompositorPad GstVvas_XTileCompositorPad;
typedef struct _GstVvas_XTileCompositorPadClass GstVvas_XTileCompositorPadClass;
typedef struct _GstVvas_XTileCompositorPrivate GstVvas_XTileCompositorPrivate;

#define GST_TYPE_VVAS_XTILECOMPOSITOR (gst_vvas_xtilecompositor_get_type ())
#define GST_VVAS_XTILECOMPOSITOR(obj)                                          \
  (G_TYPE_CHECK_INSTANCE_CAST (                                                \
      (obj), GST_TYPE_VVAS_XTILECOMPOSITOR, GstVvas_XTileCompositor))
#define GST_VVAS_XTILECOMPOSITOR_CAST(obj) ((GstVvas_XTileCompositor *)obj)
#define GST_VVAS_XTILECOMPOSITOR_CLASS(klass)                                  \
  (G_TYPE_CHECK_CLASS_CAST (                                                   \
      (klass), GST_TYPE_VVAS_XTILECOMPOSITOR, GstVvas_XTileCompositorClass))
#define GST_IS_VVAS_XTILECOMPOSITOR(obj)                                       \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_VVAS_XTILECOMPOSITOR))
#define GST_IS_VVAS_XTILECOMPOSITOR_CLASS(klass)                               \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_VVAS_XTILECOMPOSITOR))

struct _GstVvas_XTileCompositorPad
{
  GstAggregatorPad aggregator_pad;
  GstBufferPool *spool;
};

typedef struct _GstVvas_XTileCompositorPadClass
{
  GstAggregatorPadClass parent;
} GstVvas_XTileCompositorPadClass;

typedef struct _GstVvas_XTileCompositor
{
  GstAggregator aggregator;
  GstVvas_XTileCompositorPrivate *priv;
  GstPad *srcpad;
} GstVvas_XTileCompositor;

typedef struct _GstVvas_XTileCompositorClass
{
  GstAggregatorClass parent;
} GstVvas_XTileCompositorClass;

GType gst_vvas_xtilecompositor_pad_get_type (void);
GType gst_vvas_xtilecompositor_get_type (void);

G_END_DECLS
#endif /* __GST_VVAS_XTILECOMPOSITOR_H__ */
