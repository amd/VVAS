/*
 * Copyright (C) 2022 Xilinx, Inc.
 * Copyright (C) 2022 - 2025 Advanced Micro Devices, Inc.
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

#include "gstvvasoverlaymeta.h"
#include <stdio.h>
#include <math.h>

GType
gst_vvas_overlay_meta_api_get_type (void)
{
  static GType type = 0;
  static const gchar *tags[] =
      { GST_META_TAG_VIDEO_STR, GST_META_TAG_VIDEO_SIZE_STR,
    GST_META_TAG_VIDEO_ORIENTATION_STR, NULL
  };

  if (g_once_init_enter (&type)) {
    GType _type = gst_meta_api_type_register ("GstVvasOverlayMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }

  return type;
}

static gboolean
gst_vvas_overlay_meta_init (GstMeta * meta, gpointer params, GstBuffer * buffer)
{
  GstVvasOverlayMeta *vvasmeta = (GstVvasOverlayMeta *) meta;

  vvas_overlay_shape_info_init (&vvasmeta->shape_info);
  return TRUE;
}

static gboolean
gst_vvas_overlay_meta_free (GstMeta * meta, GstBuffer * buffer)
{
  GstVvasOverlayMeta *vvasmeta = (GstVvasOverlayMeta *) meta;

  vvas_overlay_shape_info_free (&vvasmeta->shape_info);
  return TRUE;
}

static gboolean
transform_overlay_meta (GstVvasOverlayMeta * dmeta, gpointer data)
{
  GstVideoMetaTransform *trans;
  gdouble hfactor = 1, vfactor = 1;
  gint fw, fh, tw, th;
  VvasList *head, *pt_head = NULL;

  trans = (GstVideoMetaTransform *) data;
  fw = GST_VIDEO_INFO_WIDTH (trans->in_info);
  tw = GST_VIDEO_INFO_WIDTH (trans->out_info);
  fh = GST_VIDEO_INFO_HEIGHT (trans->in_info);
  th = GST_VIDEO_INFO_HEIGHT (trans->out_info);

  if (0 == fw || 0 == fh) {
    return FALSE;
  }

  hfactor = tw * 1.0 / fw;
  vfactor = th * 1.0 / fh;

  GST_LOG ("Xform from %dx%d -> %dx%d, hfactor: %lf, vfactor: %lf",
      fw, fh, tw, th, hfactor, vfactor);

  if (dmeta->shape_info.num_rects) {
    head = dmeta->shape_info.rect_params;
    while (head) {
      VvasOverlayRectParams *rect_params = (VvasOverlayRectParams *) head->data;
      rect_params->points.x *= hfactor;
      rect_params->points.y *= vfactor;
      rect_params->width *= hfactor;
      rect_params->height *= vfactor;
      head = head->next;
    }
  }

  if (dmeta->shape_info.num_text) {
    head = dmeta->shape_info.text_params;
    while (head) {
      VvasOverlayTextParams *text_params = (VvasOverlayTextParams *) head->data;
      text_params->points.x *= hfactor;
      text_params->points.y *= vfactor;
      head = head->next;
    }
  }


  if (dmeta->shape_info.num_lines) {
    head = dmeta->shape_info.line_params;
    while (head) {
      VvasOverlayLineParams *line_params = (VvasOverlayLineParams *) head->data;
      line_params->start_pt.x *= hfactor;
      line_params->start_pt.y *= vfactor;
      line_params->end_pt.x *= hfactor;
      line_params->end_pt.y *= vfactor;
      head = head->next;
    }
  }

  if (dmeta->shape_info.num_arrows) {
    head = dmeta->shape_info.arrow_params;
    while (head) {
      VvasOverlayArrowParams *arrow_params =
          (VvasOverlayArrowParams *) head->data;
      arrow_params->start_pt.x *= hfactor;
      arrow_params->start_pt.y *= vfactor;
      arrow_params->end_pt.x *= hfactor;
      arrow_params->end_pt.y *= vfactor;
      head = head->next;
    }
  }

  if (dmeta->shape_info.num_circles) {
    double from_area, to_area;

    from_area = fw * fh;
    to_area = tw * th;
    head = dmeta->shape_info.circle_params;
    while (head) {
      VvasOverlayCircleParams *circle_params =
          (VvasOverlayCircleParams *) head->data;
      double from_circle_area, to_circle_area, to_radius;

      from_circle_area = M_PI * (circle_params->radius * circle_params->radius);
      to_circle_area = (to_area * from_circle_area) / from_area;
      to_radius = to_circle_area / M_PI;
      to_radius = sqrt (to_radius);
      GST_LOG ("radius %d -> %lf", circle_params->radius, to_radius);

      circle_params->center_pt.x *= hfactor;
      circle_params->center_pt.y *= vfactor;
      circle_params->radius = to_radius;
      head = head->next;
    }
  }

  head = dmeta->shape_info.polygn_params;
  while (head) {
    VvasOverlayPolygonParams *polygn_params =
        (VvasOverlayPolygonParams *) head->data;
    pt_head = polygn_params->poly_pts;
    while (pt_head) {
      VvasOverlayCoordinates *poly_pts =
          (VvasOverlayCoordinates *) pt_head->data;
      poly_pts->x *= hfactor;
      poly_pts->y *= vfactor;
      pt_head = pt_head->next;
    }
    head = head->next;
  }
  return TRUE;
}

static gboolean
add_and_copy_overlay_meta (GstBuffer * dest, GstVvasOverlayMeta * smeta)
{
  GstVvasOverlayMeta *dmeta;
  dmeta = gst_buffer_add_vvas_overlay_meta (dest);
  if (!dmeta) {
    GST_ERROR ("Unable to add meta to buffer");
    return FALSE;
  }

  vvas_overlay_shape_info_copy (&dmeta->shape_info, &smeta->shape_info);
  return TRUE;
}

static gint32
clamp_overlay_coordinate (gint64 value, guint limit)
{
  if (!limit || value < 0)
    return 0;
  if ((guint64) value >= limit)
    return (gint32) limit - 1;
  return (gint32) value;
}

static void
translate_overlay_point (VvasOverlayCoordinates * point, guint origin_x,
    guint origin_y, guint tile_width, guint tile_height, guint master_width,
    guint master_height)
{
  point->x = clamp_overlay_coordinate (
      (gint64) origin_x +
      clamp_overlay_coordinate (point->x, tile_width), master_width);
  point->y = clamp_overlay_coordinate (
      (gint64) origin_y +
      clamp_overlay_coordinate (point->y, tile_height), master_height);
}

static void
translate_overlay_rect (VvasOverlayRectParams * rect, guint origin_x,
    guint origin_y, guint tile_width, guint tile_height, guint master_width,
    guint master_height)
{
  gint64 left = CLAMP ((gint64) rect->points.x, 0, (gint64) tile_width);
  gint64 top = CLAMP ((gint64) rect->points.y, 0, (gint64) tile_height);
  gint64 right = CLAMP ((gint64) rect->points.x + rect->width, 0,
      (gint64) tile_width);
  gint64 bottom = CLAMP ((gint64) rect->points.y + rect->height, 0,
      (gint64) tile_height);

  left = CLAMP ((gint64) origin_x + left, 0, (gint64) master_width);
  top = CLAMP ((gint64) origin_y + top, 0, (gint64) master_height);
  right = CLAMP ((gint64) origin_x + right, 0, (gint64) master_width);
  bottom = CLAMP ((gint64) origin_y + bottom, 0, (gint64) master_height);
  rect->points.x = (gint32) MIN (left,
      master_width ? (gint64) master_width - 1 : 0);
  rect->points.y = (gint32) MIN (top,
      master_height ? (gint64) master_height - 1 : 0);
  rect->width = right > left ? (guint32) (right - left) : 0;
  rect->height = bottom > top ? (guint32) (bottom - top) : 0;
}

static void
translate_overlay_shape_info (VvasOverlayShapeInfo * shape_info,
    guint origin_x, guint origin_y, guint tile_width, guint tile_height,
    guint master_width, guint master_height)
{
  VvasList *head;

  for (head = shape_info->rect_params; head; head = head->next)
    translate_overlay_rect ((VvasOverlayRectParams *) head->data, origin_x,
        origin_y, tile_width, tile_height, master_width, master_height);

  for (head = shape_info->text_params; head; head = head->next) {
    VvasOverlayTextParams *text = (VvasOverlayTextParams *) head->data;

    translate_overlay_point (&text->points, origin_x, origin_y, tile_width,
        tile_height, master_width, master_height);
  }
  for (head = shape_info->line_params; head; head = head->next) {
    VvasOverlayLineParams *line = (VvasOverlayLineParams *) head->data;

    translate_overlay_point (&line->start_pt, origin_x, origin_y, tile_width,
        tile_height, master_width, master_height);
    translate_overlay_point (&line->end_pt, origin_x, origin_y, tile_width,
        tile_height, master_width, master_height);
  }
  for (head = shape_info->arrow_params; head; head = head->next) {
    VvasOverlayArrowParams *arrow = (VvasOverlayArrowParams *) head->data;

    translate_overlay_point (&arrow->start_pt, origin_x, origin_y, tile_width,
        tile_height, master_width, master_height);
    translate_overlay_point (&arrow->end_pt, origin_x, origin_y, tile_width,
        tile_height, master_width, master_height);
  }
  for (head = shape_info->circle_params; head; head = head->next) {
    VvasOverlayCircleParams *circle = (VvasOverlayCircleParams *) head->data;
    guint local_x = clamp_overlay_coordinate (circle->center_pt.x, tile_width);
    guint local_y = clamp_overlay_coordinate (circle->center_pt.y, tile_height);
    guint max_radius = MIN (MIN (local_x,
            tile_width ? tile_width - 1 - local_x : 0),
        MIN (local_y, tile_height ? tile_height - 1 - local_y : 0));

    circle->radius = MIN (circle->radius, max_radius);
    translate_overlay_point (&circle->center_pt, origin_x, origin_y,
        tile_width, tile_height, master_width, master_height);
  }
  for (head = shape_info->polygn_params; head; head = head->next) {
    VvasOverlayPolygonParams *polygon = (VvasOverlayPolygonParams *) head->data;
    VvasList *point;

    for (point = polygon->poly_pts; point; point = point->next)
      translate_overlay_point ((VvasOverlayCoordinates *) point->data,
          origin_x, origin_y, tile_width, tile_height, master_width,
          master_height);
  }
  for (head = shape_info->mask_params; head; head = head->next) {
    VvasOverlayMaskParams *mask = (VvasOverlayMaskParams *) head->data;

    translate_overlay_rect (&mask->mask_roi, origin_x, origin_y, tile_width,
        tile_height, master_width, master_height);
  }
}

gboolean
gst_vvas_overlay_meta_append_translated (GstVvasOverlayMeta * dest,
    const GstVvasOverlayMeta * src, guint origin_x, guint origin_y,
    guint tile_width, guint tile_height, guint master_width,
    guint master_height)
{
  VvasOverlayShapeInfo copied;

  g_return_val_if_fail (dest != NULL, FALSE);
  g_return_val_if_fail (src != NULL, FALSE);
  if (!tile_width || !tile_height || !master_width || !master_height)
    return FALSE;

  vvas_overlay_shape_info_init (&copied);
  vvas_overlay_shape_info_copy (&copied,
      (VvasOverlayShapeInfo *) & src->shape_info);
  translate_overlay_shape_info (&copied, origin_x, origin_y, tile_width,
      tile_height, master_width, master_height);

  dest->shape_info.rect_params =
      vvas_list_concat (dest->shape_info.rect_params, copied.rect_params);
  dest->shape_info.text_params =
      vvas_list_concat (dest->shape_info.text_params, copied.text_params);
  dest->shape_info.line_params =
      vvas_list_concat (dest->shape_info.line_params, copied.line_params);
  dest->shape_info.arrow_params =
      vvas_list_concat (dest->shape_info.arrow_params, copied.arrow_params);
  dest->shape_info.circle_params =
      vvas_list_concat (dest->shape_info.circle_params, copied.circle_params);
  dest->shape_info.polygn_params =
      vvas_list_concat (dest->shape_info.polygn_params, copied.polygn_params);
  dest->shape_info.mask_params =
      vvas_list_concat (dest->shape_info.mask_params, copied.mask_params);
  dest->shape_info.num_rects += copied.num_rects;
  dest->shape_info.num_text += copied.num_text;
  dest->shape_info.num_lines += copied.num_lines;
  dest->shape_info.num_arrows += copied.num_arrows;
  dest->shape_info.num_circles += copied.num_circles;
  dest->shape_info.num_polys += copied.num_polys;
  dest->shape_info.num_masks += copied.num_masks;

  memset (&copied, 0, sizeof (copied));
  return TRUE;
}

static gboolean
gst_vvas_overlay_meta_transform (GstBuffer * dest, GstMeta * meta,
    GstBuffer * buffer, GQuark type, gpointer data)
{
  GstVvasOverlayMeta *dmeta, *smeta;
  gboolean ret = FALSE;

  smeta = (GstVvasOverlayMeta *) meta;

  if (GST_META_TRANSFORM_IS_COPY (type)) {
    ret = add_and_copy_overlay_meta (dest, smeta);
  } else if (GST_VIDEO_META_TRANSFORM_IS_SCALE (type)) {
    dmeta = gst_buffer_get_vvas_overlay_meta (dest);
    if (!dmeta) {
      if (!add_and_copy_overlay_meta (dest, smeta)) {
        return ret;
      }
      dmeta = gst_buffer_get_vvas_overlay_meta (dest);
    }
    ret = transform_overlay_meta (dmeta, data);
  }
  return ret;
}

const GstMetaInfo *
gst_vvas_overlay_meta_get_info (void)
{
  static const GstMetaInfo *vvas_overlay_meta_info = NULL;

  if (g_once_init_enter ((GstMetaInfo **) & vvas_overlay_meta_info)) {
    const GstMetaInfo *meta =
        gst_meta_register (GST_VVAS_OVERLAY_META_API_TYPE, "GstVvasOverlayMeta",
        sizeof (GstVvasOverlayMeta),
        (GstMetaInitFunction) gst_vvas_overlay_meta_init,
        (GstMetaFreeFunction) gst_vvas_overlay_meta_free,
        gst_vvas_overlay_meta_transform);
    g_once_init_leave ((GstMetaInfo **) & vvas_overlay_meta_info,
        (GstMetaInfo *) meta);
  }
  return vvas_overlay_meta_info;
}
