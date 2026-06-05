/*
 * GStreamer
 * Copyright (C) 2018-2020 RidgeRun <support@ridgerun.com>
 * Copyright (C) 2021 - 2022 Xilinx, Inc.
 * Copyright (C) 2023 - 2025 Advanced Micro Devices, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

/*
 * This is the modified version of RidgeRun code
 * (https://github.com/RidgeRun/gst-inference) to support Xilinx VVAS product
 * specific use cases
 */

#include "gstinferenceprediction.h"
#include <vvas_utils/vvas_infer_results.h>

static GType gst_inference_prediction_get_type (void);
GST_DEFINE_MINI_OBJECT_TYPE (GstInferencePrediction, gst_inference_prediction);

typedef struct _PredictionScaleData PredictionScaleData;
struct _PredictionScaleData
{
  GstVideoInfo *from;
  GstVideoInfo *to;
};

typedef struct _PredictionFindData PredictionFindData;
struct _PredictionFindData
{
  GstInferencePrediction *prediction;
  guint64 prediction_id;
};

static void gst_inference_prediction_free (GstInferencePrediction * self);
static GstInferencePrediction *prediction_copy (const GstInferencePrediction *
    self);
static void prediction_free (GstInferencePrediction * obj);
static gboolean prediction_find (GstInferencePrediction * obj,
    PredictionFindData * found);
static GstInferencePrediction *prediction_find_unlocked (GstInferencePrediction
    * self, guint64 id);
static void prediction_reset (GstInferencePrediction * self);
static gchar *prediction_to_string (GstInferencePrediction * self, gint level);
static gchar *prediction_children_to_string (GstInferencePrediction * self,
    gint level);
static GstInferencePrediction *prediction_scale (const GstInferencePrediction *
    self, GstVideoInfo * to, GstVideoInfo * from);
static void prediction_scale_ip (GstInferencePrediction * self,
    GstVideoInfo * to, GstVideoInfo * from);
static GSList *prediction_get_children_unlocked (GstInferencePrediction * self);
static gboolean prediction_merge (GstInferencePrediction * src,
    GstInferencePrediction * dst);
static void prediction_get_enabled (GstInferencePrediction * self,
    GList ** enabled);

static void classification_merge (GList * src, GList ** dst);
static gint classification_compare (gconstpointer a, gconstpointer b);

static void node_get_children (VvasTreeNode * node, gpointer data);
static gpointer node_copy (gconstpointer node, gpointer data);
static gboolean node_scale_ip (VvasTreeNode * node, gpointer data);
static gpointer node_scale (gconstpointer, gpointer data);
static gboolean node_assign (VvasTreeNode * node, gpointer data);
static gboolean node_find (VvasTreeNode * node, gpointer data);
static gboolean node_get_enabled (VvasTreeNode * node, gpointer data);

GstInferencePrediction *
gst_inference_prediction_new (void)
{
  GstInferencePrediction *self = g_slice_new (GstInferencePrediction);

  gst_mini_object_init (GST_MINI_OBJECT_CAST (self), 0,
      gst_inference_prediction_get_type (),
      (GstMiniObjectCopyFunction) gst_inference_prediction_copy, NULL,
      (GstMiniObjectFreeFunction) gst_inference_prediction_free);

  g_mutex_init (&self->mutex);

  self->prediction.node = NULL;
  self->sub_buffer = NULL;
  self->prediction.model_path = NULL;
  self->prediction.infer_result = NULL;
  self->prediction.tb_ref = NULL;
  self->prediction.tensors = NULL;
  self->prediction.priv.data = NULL;
  self->prediction.priv.copy = NULL;
  self->prediction.priv.free = NULL;
  self->reserved_1 = NULL;
  self->reserved_2 = NULL;
  self->reserved_3 = NULL;
  self->reserved_4 = NULL;
  self->reserved_5 = NULL;

  prediction_reset (self);

  return self;
}

GstInferencePrediction *
gst_inference_prediction_new_full (VvasBoundingBox *bbox)
{
  GstInferencePrediction *self = NULL;
  VvasInferDetection *detection = NULL;

  g_return_val_if_fail (bbox, NULL);

  self = gst_inference_prediction_new ();
  self->prediction.infer_result = vvas_infer_result_detection_create ();
  detection = (VvasInferDetection *) self->prediction.infer_result->data;

  GST_INFERENCE_PREDICTION_LOCK (self);
  detection->bbox = *bbox;
  GST_INFERENCE_PREDICTION_UNLOCK (self);

  return self;
}

GstInferencePrediction *
gst_inference_prediction_ref (GstInferencePrediction *self)
{
  g_return_val_if_fail (self, NULL);

  return (GstInferencePrediction *)
      gst_mini_object_ref (GST_MINI_OBJECT_CAST (self));
}

void
gst_inference_prediction_unref (GstInferencePrediction *self)
{
  g_return_if_fail (self);

  gst_mini_object_unref (GST_MINI_OBJECT_CAST (self));
}

void
gst_inference_prediction_append (GstInferencePrediction *self,
    GstInferencePrediction *child)
{
  g_return_if_fail (self);
  g_return_if_fail (child);

  GST_INFERENCE_PREDICTION_LOCK (self);
  GST_INFERENCE_PREDICTION_LOCK (child);
  vvas_treenode_append (self->prediction.node, child->prediction.node);
  GST_INFERENCE_PREDICTION_UNLOCK (child);
  GST_INFERENCE_PREDICTION_UNLOCK (self);
}

static void
copy_tensors_list (void *data, void *udata)
{
  TensorBuf *tb = (TensorBuf *) data;
  TensorBuf *dst_tb = NULL;
  VvasList **dst_tensors = (VvasList **) udata;

  tb->copy ((void **) &tb, (void **) &dst_tb);
  *dst_tensors = vvas_list_append (*dst_tensors, dst_tb);
}

static GstInferencePrediction *
prediction_copy (const GstInferencePrediction *self)
{
  GstInferencePrediction *other = NULL;

  g_return_val_if_fail (self, NULL);

  other = gst_inference_prediction_new ();

  other->prediction.prediction_id = self->prediction.prediction_id;
  other->prediction.enabled = self->prediction.enabled;
  other->prediction.width = self->prediction.width;
  other->prediction.height = self->prediction.height;

  if (self->sub_buffer != NULL)
    other->sub_buffer = gst_buffer_ref (self->sub_buffer);

  if (self->prediction.infer_result && self->prediction.infer_result->create &&
      self->prediction.infer_result->data) {
    other->prediction.infer_result = self->prediction.infer_result->create ();
    self->prediction.infer_result->copy (self->prediction.infer_result->data,
        &(other->prediction.infer_result->data));
  }

  if (self->prediction.tb_ref != NULL)
    other->prediction.tb_ref = tensor_buf_ref (self->prediction.tb_ref);

  if (self->prediction.tensors)
    vvas_list_foreach (self->prediction.tensors, copy_tensors_list,
        &other->prediction.tensors);

  if (self->prediction.model_path)
    other->prediction.model_path = g_strdup (self->prediction.model_path);

  other->prediction.priv.copy = self->prediction.priv.copy;
  other->prediction.priv.free = self->prediction.priv.free;
  if (self->prediction.priv.data && self->prediction.priv.copy) {
    self->prediction.priv.copy (self->prediction.priv.data,
        &(other->prediction.priv.data));
  }

  other->reserved_1 = self->reserved_1;
  other->reserved_2 = self->reserved_2;
  other->reserved_3 = self->reserved_3;
  other->reserved_4 = self->reserved_4;
  other->reserved_5 = self->reserved_5;

  return other;
}

static gpointer
node_copy (gconstpointer node, gpointer data)
{
  GstInferencePrediction *self = (GstInferencePrediction *) node;

  g_return_val_if_fail (node, NULL);

  return prediction_copy (self);
}

static gboolean
node_assign (VvasTreeNode *node, gpointer data)
{
  GstInferencePrediction *pred = (GstInferencePrediction *) node->data;

  g_return_val_if_fail (node, FALSE);

  if (pred->prediction.node) {
    vvas_treenode_destroy (pred->prediction.node);
    pred->prediction.node = NULL;
  }

  pred->prediction.node = node;

  return FALSE;
}

GstInferencePrediction *
gst_inference_prediction_copy (const GstInferencePrediction *self)
{
  VvasTreeNode *other = NULL;

  g_return_val_if_fail (self, NULL);

  GST_INFERENCE_PREDICTION_LOCK ((GstInferencePrediction *) self);

  /* Copy the binary tree */
  other = vvas_treenode_copy_deep (self->prediction.node, node_copy, NULL);

  /* Now finish assigning the nodes to the predictions */
  vvas_treenode_traverse (other, IN_ORDER, TRAVERSE_ALL, -1,
      (vvas_treenode_traverse_func) node_assign, NULL);

  GST_INFERENCE_PREDICTION_UNLOCK ((GstInferencePrediction *) self);

  return (GstInferencePrediction *) other->data;
}

static gchar *
prediction_children_to_string (GstInferencePrediction *self, gint level)
{
  GSList *subpreds = NULL;
  GSList *iter = NULL;
  GString *string = NULL;

  g_return_val_if_fail (self, NULL);

  /* Build the child predictions using a GString */
  string = g_string_new (NULL);

  subpreds = prediction_get_children_unlocked (self);

  for (iter = subpreds; iter != NULL; iter = g_slist_next (iter)) {
    GstInferencePrediction *pred = (GstInferencePrediction *) iter->data;
    gchar *child = prediction_to_string (pred, level + 1);

    g_string_append_printf (string, "%s, ", child);
    g_free (child);
  }

  g_slist_free (subpreds);

  return g_string_free (string, FALSE);
}

static gchar *
prediction_to_string (GstInferencePrediction *self, gint level)
{
  gint indent = level * 2;
  gchar *children = NULL;
  gchar *vvas_prediction = NULL;
  gchar *gst_prediction = NULL;

  g_return_val_if_fail (self, NULL);

  vvas_prediction =
      vvas_inferprediction_to_string_with_level (&self->prediction, level + 1);
  children = prediction_children_to_string (self, level + 1);

  if (strlen (children)) {
    gst_prediction = g_strdup_printf ("{\n"
        "%*s  sub-buffer : %p,\n"
        "%*s  prediction : %s,\n"
        "%*s  child-predictions : [\n"
        "%*s    %s\n"
        "%*s  ]\n"
        "%*s}",
        indent, "", self->sub_buffer,
        indent, "", vvas_prediction,
        indent, "", indent, "", children, indent, "", indent, "");
  } else {
    gst_prediction = g_strdup_printf ("{\n"
        "%*s  sub-buffer : %p,\n"
        "%*s  prediction : %s,\n"
        "%*s}",
        indent, "", self->sub_buffer, indent, "", vvas_prediction, indent, "");
  }
  g_free (vvas_prediction);
  g_free (children);

  return gst_prediction;
}

gchar *
gst_inference_prediction_to_string (GstInferencePrediction *self)
{
  gchar *serial = NULL;

  g_return_val_if_fail (self, NULL);

  GST_INFERENCE_PREDICTION_LOCK (self);
  serial = prediction_to_string (self, 0);
  GST_INFERENCE_PREDICTION_UNLOCK (self);

  return serial;
}

static void
node_get_children (VvasTreeNode *node, gpointer data)
{
  GSList **children = (GSList **) data;
  GstInferencePrediction *prediction;

  g_return_if_fail (node);
  g_return_if_fail (children);

  prediction = (GstInferencePrediction *) node->data;

  *children = g_slist_append (*children, prediction);
}

static GSList *
prediction_get_children_unlocked (GstInferencePrediction *self)
{
  GSList *children = NULL;

  g_return_val_if_fail (self, NULL);

  if (self->prediction.node) {
    vvas_treenode_traverse_child (self->prediction.node, TRAVERSE_ALL,
        node_get_children, &children);
  }

  return children;
}

GSList *
gst_inference_prediction_get_children (GstInferencePrediction *self)
{
  GSList *children = NULL;

  GST_INFERENCE_PREDICTION_LOCK (self);
  children = prediction_get_children_unlocked (self);
  GST_INFERENCE_PREDICTION_UNLOCK (self);

  return children;
}

static void
prediction_reset (GstInferencePrediction *self)
{

  g_return_if_fail (self);

  self->prediction.prediction_id = vvas_inferprediction_get_prediction_id ();
  self->prediction.enabled = TRUE;
  self->prediction.scaled_to_root = FALSE;

  /* Free all children */
  prediction_free (self);

  self->prediction.node = vvas_treenode_new (self);
}

static void
free_tensors_list (void *data, void *udata)
{
  TensorBuf *tb = (TensorBuf *) data;
  tb->free ((void **) &tb);
}

static void
prediction_free (GstInferencePrediction *self)
{
  GSList *children = prediction_get_children_unlocked (self);

  /* Free all children recursively */
  g_slist_free_full (children, (GDestroyNotify) gst_inference_prediction_unref);

  if (self->prediction.node) {
    vvas_treenode_destroy (self->prediction.node);
    self->prediction.node = NULL;
  }

  if (self->sub_buffer != NULL)
    gst_buffer_unref (self->sub_buffer);

  /* Check and free the VvasInferResult and its corresponding inference
   * result structure */
  if (self->prediction.infer_result && self->prediction.infer_result->free) {
    self->prediction.infer_result->free (self->prediction.infer_result);
    self->prediction.infer_result = NULL;
  }

  if (self->prediction.tb_ref) {
    tensor_buf_unref (self->prediction.tb_ref);
    self->prediction.tb_ref = NULL;
  }

  if (self->prediction.tensors) {
    vvas_list_foreach (self->prediction.tensors, free_tensors_list, NULL);
    vvas_list_free (self->prediction.tensors);
    self->prediction.tensors = NULL;
  }

  if (self->prediction.model_path)
    g_free (self->prediction.model_path);

  if (self->prediction.priv.data && self->prediction.priv.free) {
    self->prediction.priv.free (self->prediction.priv.data);
    self->prediction.priv.data = NULL;
  }

  self->sub_buffer = NULL;
  self->prediction.model_path = NULL;
  self->reserved_1 = NULL;
  self->reserved_2 = NULL;
  self->reserved_3 = NULL;
  self->reserved_4 = NULL;
  self->reserved_5 = NULL;
}

static void
gst_inference_prediction_free (GstInferencePrediction *self)
{
  g_return_if_fail (self);

  prediction_free (self);

  g_mutex_clear (&self->mutex);
  g_slice_free (GstInferencePrediction, self);
}

void
gst_inference_prediction_append_classification (GstInferencePrediction *self,
    VvasInferClassification *c)
{
  VvasList *classification = NULL;

  g_return_if_fail (self);
  g_return_if_fail (c);

  classification = (VvasList *) self->prediction.infer_result->data;

  GST_INFERENCE_PREDICTION_LOCK (self);
  classification = (VvasList *) g_list_append ((GList *) classification, c);
  GST_INFERENCE_PREDICTION_UNLOCK (self);
}

/* Clip a detection's bbox to the destination frame bounds.
 *
 * Mirrors the clamp block in vvas_xinfer's update_child_bbox()
 * (see sys/infer/gstvvas_xinfer.cpp, "check if updated coordinates are
 * extended beyond original image"), extended with negative-origin and
 * non-positive-extent guards so that any out-of-frame detections produced
 * by a per-model transform get normalised before downstream elements
 * (metaconvert / overlay) consume them.
 */
static inline void
clip_detection_to_frame (VvasInferDetection *det, GstVideoInfo *to)
{
  const gint w = GST_VIDEO_INFO_WIDTH (to);
  const gint h = GST_VIDEO_INFO_HEIGHT (to);

  if ((gint) det->bbox.x < 0) {
    det->bbox.width += (gint) det->bbox.x;
    det->bbox.x = 0;
  }
  if ((gint) det->bbox.y < 0) {
    det->bbox.height += (gint) det->bbox.y;
    det->bbox.y = 0;
  }
  if ((gint) (det->bbox.x + det->bbox.width) > w)
    det->bbox.width = w - (gint) det->bbox.x;
  if ((gint) (det->bbox.y + det->bbox.height) > h)
    det->bbox.height = h - (gint) det->bbox.y;

  if ((gint) det->bbox.width < 0)
    det->bbox.width = 0;
  if ((gint) det->bbox.height < 0)
    det->bbox.height = 0;
}

static GstInferencePrediction *
prediction_scale (const GstInferencePrediction *self, GstVideoInfo *to,
    GstVideoInfo *from)
{
  GstInferencePrediction *dest = NULL;
  VvasInferScaleInfo scale_info;

  g_return_val_if_fail (self, NULL);
  g_return_val_if_fail (to, NULL);
  g_return_val_if_fail (from, NULL);

  scale_info.from_width = GST_VIDEO_INFO_WIDTH (from);
  scale_info.from_height = GST_VIDEO_INFO_HEIGHT (from);
  scale_info.to_width = GST_VIDEO_INFO_WIDTH (to);
  scale_info.to_height = GST_VIDEO_INFO_HEIGHT (to);
  scale_info.x = 0;
  scale_info.y = 0;
  scale_info.xOffset = 0;
  scale_info.yOffset = 0;

  dest = prediction_copy (self);

  if (dest->prediction.infer_result && dest->prediction.infer_result->transform) {
    dest->prediction.infer_result->transform (dest->prediction.infer_result->
        data, &scale_info);

    if (dest->prediction.infer_result->infer_result_type ==
        VVAS_INFER_RESULT_DETECTION && dest->prediction.infer_result->data) {
      clip_detection_to_frame ((VvasInferDetection *)
          dest->prediction.infer_result->data, to);
    }
  }

  dest->prediction.scaled_to_root = TRUE;

  return dest;
}

static void
prediction_scale_ip (GstInferencePrediction *self, GstVideoInfo *to,
    GstVideoInfo *from)
{
  VvasInferScaleInfo scale_info;

  g_return_if_fail (self);
  g_return_if_fail (to);
  g_return_if_fail (from);

  scale_info.from_width = GST_VIDEO_INFO_WIDTH (from);
  scale_info.from_height = GST_VIDEO_INFO_HEIGHT (from);
  scale_info.to_width = GST_VIDEO_INFO_WIDTH (to);
  scale_info.to_height = GST_VIDEO_INFO_HEIGHT (to);
  scale_info.x = 0;
  scale_info.y = 0;
  scale_info.xOffset = 0;
  scale_info.yOffset = 0;

  if (self->prediction.infer_result && self->prediction.infer_result->transform) {
    self->prediction.infer_result->transform (self->prediction.infer_result->
        data, &scale_info);

    if (self->prediction.infer_result->infer_result_type ==
        VVAS_INFER_RESULT_DETECTION && self->prediction.infer_result->data) {
      clip_detection_to_frame ((VvasInferDetection *)
          self->prediction.infer_result->data, to);
    }
  }

  self->prediction.scaled_to_root = TRUE;
}

static gboolean
node_scale_ip (VvasTreeNode *node, gpointer data)
{
  GstInferencePrediction *self = (GstInferencePrediction *) node->data;
  PredictionScaleData *sdata = (PredictionScaleData *) data;

  prediction_scale_ip (self, sdata->to, sdata->from);

  return FALSE;
}

static gpointer
node_scale (gconstpointer node, gpointer data)
{
  const GstInferencePrediction *self = (GstInferencePrediction *) node;
  PredictionScaleData *sdata = (PredictionScaleData *) data;

  return prediction_scale (self, sdata->to, sdata->from);
}

void
gst_inference_prediction_scale_ip (GstInferencePrediction *self,
    GstVideoInfo *to, GstVideoInfo *from)
{
  PredictionScaleData data = {.from = from,.to = to };

  g_return_if_fail (self);
  g_return_if_fail (to);
  g_return_if_fail (from);

  GST_INFERENCE_PREDICTION_LOCK (self);

  vvas_treenode_traverse (self->prediction.node, IN_ORDER, TRAVERSE_ALL, -1,
      (vvas_treenode_traverse_func) node_scale_ip, &data);

  GST_INFERENCE_PREDICTION_UNLOCK (self);
}

GstInferencePrediction *
gst_inference_prediction_scale (GstInferencePrediction *self,
    GstVideoInfo *to, GstVideoInfo *from)
{
  VvasTreeNode *other = NULL;
  PredictionScaleData data = {.from = from,.to = to };

  g_return_val_if_fail (self, NULL);
  g_return_val_if_fail (to, NULL);
  g_return_val_if_fail (from, NULL);

  GST_INFERENCE_PREDICTION_LOCK (self);

  other =
      vvas_treenode_copy_deep (self->prediction.node,
      (vvas_treenode_copy_func) node_scale, &data);

  /* Now finish assigning the nodes to the predictions */
  vvas_treenode_traverse (other, IN_ORDER, TRAVERSE_ALL, -1,
      (vvas_treenode_traverse_func) node_assign, NULL);

  GST_INFERENCE_PREDICTION_UNLOCK (self);

  return (GstInferencePrediction *) other->data;
}

static gboolean
prediction_find (GstInferencePrediction *obj, PredictionFindData *found)
{
  gboolean ret = FALSE;

  g_return_val_if_fail (obj, TRUE);
  g_return_val_if_fail (found, TRUE);

  if (obj->prediction.prediction_id == found->prediction_id) {
    found->prediction = gst_inference_prediction_ref (obj);
    ret = TRUE;
  }

  return ret;
}

static gboolean
node_find (VvasTreeNode *node, gpointer data)
{
  PredictionFindData *found = (PredictionFindData *) data;
  GstInferencePrediction *current = (GstInferencePrediction *) node->data;

  g_return_val_if_fail (found, TRUE);

  return prediction_find (current, found);
}

static GstInferencePrediction *
prediction_find_unlocked (GstInferencePrediction *self, guint64 id)
{
  PredictionFindData found = { 0, id };

  g_return_val_if_fail (self, NULL);

  vvas_treenode_traverse (self->prediction.node, IN_ORDER, TRAVERSE_ALL, -1,
      (vvas_treenode_traverse_func) node_find, &found);

  return found.prediction;
}

GstInferencePrediction *
gst_inference_prediction_find (GstInferencePrediction *self, guint64 id)
{
  GstInferencePrediction *found = NULL;

  g_return_val_if_fail (self, NULL);

  GST_INFERENCE_PREDICTION_LOCK (self);

  found = prediction_find_unlocked (self, id);

  GST_INFERENCE_PREDICTION_UNLOCK (self);

  return found;
}

static gint
classification_compare (gconstpointer a, gconstpointer b)
{
  VvasInferClassification *ca = (VvasInferClassification *) a;
  VvasInferClassification *cb = (VvasInferClassification *) b;

  return (ca->classification_id == cb->classification_id ? 0 : 1);
}

static void
classification_merge (GList *src, GList **dst)
{
  GList *iter = NULL;

  g_return_if_fail (dst);

  /* For each classification in the src, see if it exists in the dst */
  for (iter = src; iter; iter = g_list_next (iter)) {
    GList *exists =
        g_list_find_custom (*dst, iter->data, classification_compare);

    /* Copy and append it to the dst if it doesn't exist */
    if (!exists) {
      VvasInferClassification *child =
          vvas_inferclassification_copy (iter->data);
      *dst = g_list_append (*dst, child);
    }
  }
}

static gboolean
prediction_merge (GstInferencePrediction *src, GstInferencePrediction *dst)
{
  GSList *src_children = prediction_get_children_unlocked (src);
  GSList *iter = NULL;
  GSList *new_children = NULL;
  gboolean new_added = FALSE;

  g_return_val_if_fail (src, FALSE);
  g_return_val_if_fail (dst, FALSE);
  g_return_val_if_fail (src->prediction.prediction_id ==
      dst->prediction.prediction_id, FALSE);

  /* Two things might've happened:
   * 1) A new class was added
   * 2) A new subprediction was added
   */

  /* Handle 1) here */
  if ((src->prediction.infer_result) &&
      (VVAS_INFER_RESULT_CLASSIFICATION ==
          src->prediction.infer_result->infer_result_type)) {
    classification_merge ((GList *) src->prediction.infer_result->data,
        (GList **) & dst->prediction.infer_result->data);
  }

  /* Handle 2) here */
  for (iter = src_children; iter; iter = g_slist_next (iter)) {
    GstInferencePrediction *current = (GstInferencePrediction *) iter->data;

    GstInferencePrediction *found =
        prediction_find_unlocked (dst, current->prediction.prediction_id);

    /* No matching prediction, save it to append it later */
    if (!found) {
      new_children = g_slist_append (new_children, current);
      continue;
    }

    /* Recurse into the children */
    new_added = prediction_merge (current, found);

    gst_inference_prediction_unref (found);
  }

  /* Finally append all the new children to dst. Do it after all
     children have been processed */
  for (iter = new_children; iter; iter = g_slist_next (iter)) {
    GstInferencePrediction *prediction =
        gst_inference_prediction_copy ((GstInferencePrediction *) iter->data);
    /* Lock */
    GST_INFERENCE_PREDICTION_LOCK (dst);
    GST_INFERENCE_PREDICTION_LOCK (prediction);
    vvas_treenode_append (dst->prediction.node, prediction->prediction.node);
    GST_INFERENCE_PREDICTION_UNLOCK (dst);
    GST_INFERENCE_PREDICTION_UNLOCK (prediction);
  }

  new_added |= new_children ? TRUE : FALSE;

  g_slist_free (src_children);
  g_slist_free_full (new_children,
      (GDestroyNotify) gst_inference_prediction_unref);

  return new_added;
}

gboolean
gst_inference_prediction_merge (GstInferencePrediction *src,
    GstInferencePrediction *dst)
{
  gboolean needs_scale = FALSE;

  g_return_val_if_fail (src, FALSE);
  g_return_val_if_fail (dst, FALSE);

  if (src == dst) {
    return needs_scale;
  }

  GST_INFERENCE_PREDICTION_LOCK (src);

  needs_scale = prediction_merge (src, dst);

  GST_INFERENCE_PREDICTION_UNLOCK (src);

  return needs_scale;
}

static void
prediction_get_enabled (GstInferencePrediction *self, GList **found)
{
  g_return_if_fail (self);
  g_return_if_fail (found);

  if (self->prediction.enabled) {
    *found = g_list_append (*found, self);
  }
}

static gboolean
node_get_enabled (VvasTreeNode *node, gpointer data)
{
  GstInferencePrediction *self = NULL;
  GList **found = (GList **) data;

  g_return_val_if_fail (node, TRUE);
  g_return_val_if_fail (found, TRUE);

  self = (GstInferencePrediction *) node->data;

  prediction_get_enabled (self, found);

  return FALSE;
}

GList *
gst_inference_prediction_get_enabled (GstInferencePrediction *self)
{
  GList *found = NULL;

  g_return_val_if_fail (self, NULL);

  vvas_treenode_traverse (self->prediction.node, IN_ORDER, TRAVERSE_ALL, -1,
      (vvas_treenode_traverse_func) node_get_enabled, &found);

  return found;
}
