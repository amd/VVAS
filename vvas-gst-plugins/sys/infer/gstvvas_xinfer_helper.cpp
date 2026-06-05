/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
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

#include "gstvvas_xinfer_helper.h"

/** @def DEFAULT_MAINTAIN_ASPECT_RATIO
 * @brief Default flag to enable or disable aspect ratio
 */
#define DEFAULT_MAINTAIN_ASPECT_RATIO  FALSE

/** @def DEFAULT_SYMMETRIC_PADDING
 * @brief Default flag to enable or disable aspect ratio
 */
#define DEFAULT_SYMMETRIC_PADDING  FALSE


/** @def DEFAULT_INFER_LEVEL
 *  @brief Setting default inference level required for cascade use case
 */
#define DEFAULT_INFER_LEVEL 1

/** @def BATCH_SIZE_ZERO
 *  @brief Setting batch size for inference processing
 */
#define BATCH_SIZE_ZERO 0

/** @def DEFAULT_INPUTOBJ_MIN_WIDTH
 * @brief setting default minimum width to the infer input object
 */
#define DEFAULT_INPUTOBJ_MIN_WIDTH  16

/** @def DEFAULT_INPUTOBJ_MIN_HEIGHT
 * @brief setting default minimum height to the infer input object
 */
#define DEFAULT_INPUTOBJ_MIN_HEIGHT  16

/** @def DEFAULT_INPUTOBJ_MAX_WIDTH
 * @brief setting default maximum width to the infer input object
 */
#define DEFAULT_INPUTOBJ_MAX_WIDTH  3840

/** @def DEFAULT_INPUTOBJ_MAX_HEIGHT
 * @brief setting default maximum height to the infer input object
 */
#define DEFAULT_INPUTOBJ_MAX_HEIGHT  2160

/** @def INFER_MAX_BATCH_SIZE
 *  @brief Maximum batch size supported
 */
#define INFER_MAX_BATCH_SIZE      32

/**
 *  @brief Defines a extern GstDebugCategory global variable "gst_vvas_xinfer_debug"
*/
GST_DEBUG_CATEGORY_EXTERN (gst_vvas_xinfer_debug);
#define GST_CAT_DEFAULT gst_vvas_xinfer_debug

static std::vector<float>
parse_float_or_float_list(void* element,
                          json_t* obj,
                          const char* key)
{
  std::vector<float> out;
  if (!obj || !key) return out;

  json_t* v = json_object_get(obj, key);
  if (!v) return out;

  if (json_is_real(v)) {
    out.push_back((float)json_real_value(v));
    return out;
  }

  if (json_is_integer(v)) {
    out.push_back((float)json_integer_value(v));
    return out;
  }

  if (json_is_array(v)) {
    const size_t n = json_array_size(v);
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      json_t* e = json_array_get(v, i);
      if (json_is_real(e)) {
        out.push_back((float)json_real_value(e));
      } else if (json_is_integer(e)) {
        out.push_back((float)json_integer_value(e));
      } else {
        GST_ERROR_OBJECT(element, "%s[%zu] must be a number (float/int)", key, i);
        out.clear();
        return out;
      }
    }
    return out;
  }

  GST_ERROR_OBJECT(element, "%s must be a number or a list of numbers", key);
  return out;
}

void vvas_infer_profiler_init (VvasInferProfiler *p) {
  VVAS_PROFILER_STATS_INIT (&p->pre_proc);
  VVAS_PROFILER_STATS_INIT (&p->infer);
  VVAS_PROFILER_STATS_INIT (&p->post_proc);
  VVAS_PROFILER_STATS_INIT (&p->last_pre_proc);
  VVAS_PROFILER_STATS_INIT (&p->last_infer);
  VVAS_PROFILER_STATS_INIT (&p->last_post_proc);
  g_mutex_init (&p->snap_lock);
  p->timeout_id = 0;
}

void vvas_infer_profiler_deinit (VvasInferProfiler *p) {
  if (p->timeout_id) {
    g_source_remove (p->timeout_id);
    p->timeout_id = 0;
  }
  g_mutex_clear (&p->snap_lock);
  /* Free log_file before memset to avoid leaking the g_value_dup_string()
   * allocation when the profiler is deinitialized. */
  g_free (p->log_file);
  memset (p, 0, sizeof (VvasInferProfiler));
}

static void compute_avg_us (const VvasProfilerStats *s, guint64 *avg_us) {
  *avg_us = (s->count > 0) ? (s->sum_us / s->count) : 0;
}

gboolean vvas_infer_profiler_tick_cb (gpointer user_data) {
  VvasInferProfiler * p = (VvasInferProfiler *) user_data;
  VvasProfilerStats pre_proc = {0}, infer, post_proc;
  VvasProfilerStats last_pre_proc = {0}, last_infer, last_post_proc;
  guint64 pre_proc_avg=0, infer_avg=0, post_proc_avg=0;
  gboolean pre_proc_enabled;
  guint interval_ms;
  guint64 frames_now, frames_delta;

  g_mutex_lock (&p->snap_lock);
  pre_proc_enabled = p->pre_proc.enabled;
  if (pre_proc_enabled)
    pre_proc = p->pre_proc;
  infer = p->infer;
  post_proc = p->post_proc;
  last_pre_proc = p->last_pre_proc;
  last_infer = p->last_infer;
  last_post_proc = p->last_post_proc;
  p->last_pre_proc = pre_proc;
  p->last_infer = infer;
  p->last_post_proc = post_proc;
  frames_now = p->total_frames;
  interval_ms = p->log_interval * 1000;
  g_mutex_unlock (&p->snap_lock);

  if (pre_proc_enabled)
    compute_avg_us (&pre_proc, &pre_proc_avg);
  compute_avg_us (&infer, &infer_avg);
  compute_avg_us (&post_proc,  &post_proc_avg);

  frames_delta = post_proc.count - last_post_proc.count;

  double fps_inst = (interval_ms ? (frames_delta * 1000.0) / interval_ms : 0.0);
  double pre_avg_ms  = (double) pre_proc_avg  / 1000.0;
  double inf_avg_ms  = (double) infer_avg / 1000.0;
  double post_avg_ms = (double) post_proc_avg / 1000.0;

  double pre_min_ms  = (double) pre_proc.min_us  / 1000.0;
  double pre_max_ms  = (double) pre_proc.max_us  / 1000.0;
  double inf_min_ms  = (double) infer.min_us / 1000.0;
  double inf_max_ms  = (double) infer.max_us / 1000.0;
  double post_min_ms = (double) post_proc.min_us  / 1000.0;
  double post_max_ms = (double) post_proc.max_us  / 1000.0;

/* fps_avg should be computed as frames_now / elapsed_seconds; assume you have it */
  double fps_avg = frames_now * 1000000.0 / (vvas_profiler_now_us () - p->start_time_us);

  if (pre_proc_enabled) {
    GST_INFO ("\nVvas_xinfer Plugin Profiling data\n"
         " frames=%" G_GUINT64_FORMAT "  fps_inst=%.2f  fps_avg=%.2f\n"
         " Pre Process stage  (ms):  avg=%.3f  min=%.3f  max=%.3f\n"
         " Inference stage    (ms):  avg=%.3f  min=%.3f  max=%.3f\n"
         " Post Process stage (ms):  avg=%.3f  min=%.3f  max=%.3f",
         frames_now, fps_inst, fps_avg,
         pre_avg_ms, pre_min_ms, pre_max_ms,
         inf_avg_ms, inf_min_ms, inf_max_ms,
         post_avg_ms, post_min_ms, post_max_ms);
  } else {
    GST_INFO (
        " \nVvas_xinfer Plugin Profile\n"
        " frames=%" G_GUINT64_FORMAT "  fps_inst=%.2f  fps_avg=%.2f\n"
        " Pre Process stage      :  Disabled\n"
        " Inference stage    (ms):  avg=%.3f  min=%.3f  max=%.3f\n"
        " Post Process stage (ms):  avg=%.3f  min=%.3f  max=%.3f\n",
         frames_now, fps_inst, fps_avg,
         inf_avg_ms, inf_min_ms, inf_max_ms,
         post_avg_ms, post_min_ms, post_max_ms);
  }

  return TRUE;
}

void
vvas_infer_profiler_dump_json (VvasInferProfiler *p, gchar *instance_name)
{
  if (!p)
    return;

  /* snapshot under lock */
  g_mutex_lock (&p->snap_lock);

  guint64 frames = p->total_frames;
  gint64 start_us = p->start_time_us;
  gint64 end_us = p->end_time_us ? p->end_time_us : vvas_profiler_now_us ();

  guint64 pre_proc_sum = p->pre_proc.sum_us;
  guint64 pre_proc_min = p->pre_proc.min_us;
  guint64 pre_proc_max = p->pre_proc.max_us;
  guint64 pre_proc_count = p->pre_proc.count;
  guint64 pre_proc_init = p->pre_proc.init_us;
  guint64 pre_proc_deinit = p->pre_proc.deinit_us;

  guint64 infer_sum = p->infer.sum_us;
  guint64 infer_min = p->infer.min_us;
  guint64 infer_max = p->infer.max_us;
  guint64 infer_count = p->infer.count;
  guint64 infer_init = p->infer.init_us;
  guint64 infer_deinit = p->infer.deinit_us;

  guint64 post_proc_sum = p->post_proc.sum_us;
  guint64 post_proc_min = p->post_proc.min_us;
  guint64 post_proc_max = p->post_proc.max_us;
  guint64 post_proc_count = p->post_proc.count;
  guint64 post_proc_init = p->post_proc.init_us;
  guint64 post_proc_deinit = p->post_proc.deinit_us;

  const char *backend = (p->backend_runtime[0] != '\0') ? p->backend_runtime : "(unknown)";
  g_mutex_unlock (&p->snap_lock);

  /* compute derived values */
  double elapsed_s = (end_us > start_us) ? (double)(end_us - start_us) / 1e6 : 0.0;
  double fps_avg = (elapsed_s > 0.0) ? (double) frames / elapsed_s : 0.0;
  
  /* Determine the output filename */
  gchar fname[PATH_MAX];
  char util_str[16];

  if (p->log_file && p->log_file[0] != '\0') {
    /* Use user-provided profiler filename */
    g_snprintf (fname, PATH_MAX, "%s", p->log_file);
  } else {
    /* Generate timestamp-based filename: pid_instancename_YYYYMMDD_HHMMSS_milliseconds_microseconds */
    gint64 now_us = g_get_real_time ();
    GDateTime *dt = g_date_time_new_from_unix_local (now_us / G_GINT64_CONSTANT(1000000));
    gchar *ts = g_date_time_format (dt, "%Y%m%d_%H%M%S");
    g_date_time_unref (dt);

    pid_t pid = getpid ();
    gchar *instance_name_safe = g_strdup_printf ("%s", instance_name ? instance_name : "unknown");
    gint64 microseconds = now_us % 1000000;
    gint64 milliseconds = microseconds / 1000;
    gint64 remaining_micros = microseconds % 1000;
    g_snprintf (fname, PATH_MAX, "vvas_xinfer_profiler_data_%d_%s_%s%03ld%03ld.json",
        pid, instance_name_safe, ts, milliseconds, remaining_micros);
    g_free (instance_name_safe);
    g_free (ts);
  }

  /* build JSON using jansson */
  json_t *root = json_object ();
  json_object_set_new (root, "backend",  json_string (backend));
  /* Format to 3 decimal points as string */
  snprintf(util_str, sizeof(util_str), "%.3f", elapsed_s);
  json_object_set_new (root, "duration_sec", json_string (util_str));
  json_object_set_new (root, "frames", json_integer ((json_int_t) frames));
  /* Format to 3 decimal points as string */
  snprintf(util_str, sizeof(util_str), "%.3f", fps_avg);
  json_object_set_new (root, "fps_avg", json_string (util_str));

  /* perprocess-stage objects */
  if (p->pre_proc.enabled) {
    json_t *jpre = json_object ();
    json_object_set_new (jpre, "init_us", json_integer ((json_int_t) pre_proc_init));
    json_object_set_new (jpre, "deinit_us", json_integer ((json_int_t) pre_proc_deinit));
    json_object_set_new (jpre, "avg_us",  json_integer ((json_int_t) pre_proc_sum/pre_proc_count));
    json_object_set_new (jpre, "min_us",  json_integer ((json_int_t) pre_proc_min));
    json_object_set_new (jpre, "max_us",  json_integer ((json_int_t) pre_proc_max));
    json_object_set_new (jpre, "samples", json_integer ((json_int_t) pre_proc_count));
    json_object_set_new (root, "preprocess", jpre);
  } else {
    json_object_set_new (root, "preprocess", json_string ("disabled"));
  }

  json_t *jinf = json_object ();
  json_object_set_new (jinf, "init_us", json_integer ((json_int_t) infer_init));
  json_object_set_new (jinf, "deinit_us", json_integer ((json_int_t) infer_deinit));
  json_object_set_new (jinf, "avg_us",  json_integer ((json_int_t) infer_sum/infer_count));
  json_object_set_new (jinf, "min_us",  json_integer ((json_int_t) infer_min));
  json_object_set_new (jinf, "max_us",  json_integer ((json_int_t) infer_max));
  json_object_set_new (jinf, "samples", json_integer ((json_int_t) infer_count));
  json_object_set_new (root, "inference", jinf);

  json_t *jpost = json_object ();
  json_object_set_new (jpost, "init_us", json_integer ((json_int_t) post_proc_init));
  json_object_set_new (jpost, "deinit_us", json_integer ((json_int_t) post_proc_deinit));
  json_object_set_new (jpost, "avg_us",  json_integer ((json_int_t) post_proc_sum/post_proc_count));
  json_object_set_new (jpost, "min_us",  json_integer ((json_int_t) post_proc_min));
  json_object_set_new (jpost, "max_us",  json_integer ((json_int_t) post_proc_max));
  json_object_set_new (jpost, "samples", json_integer ((json_int_t) post_proc_count));
  json_object_set_new (root, "postprocess", jpost);

  /* write file (pretty-printed) */
  int flags = JSON_INDENT(2);
  if (json_dump_file (root, fname, flags) != 0) {
    GST_WARNING ("Failed to write profiler JSON to %s", fname);
  } else {
    GST_INFO ("Profiler JSON written to %s", fname);
  }

  json_decref (root);
}

static VvasVideoFormat
get_vvas_video_fmt (const char *name)
{
  if (!strncmp (name, "RGBA", 4))
    return VVAS_VIDEO_FORMAT_RGBA;
  else if (!strncmp (name, "BGRA", 4))
    return VVAS_VIDEO_FORMAT_BGRA;
  else if (!strncmp (name, "RGBX", 4))
    return VVAS_VIDEO_FORMAT_RGBx;
  else if (!strncmp (name, "BGRX", 4))
    return VVAS_VIDEO_FORMAT_BGRx;
  else if (!strncmp (name, "RGB", 3))
    return VVAS_VIDEO_FORMAT_RGB;
  else if (!strncmp (name, "BGR", 3))
    return VVAS_VIDEO_FORMAT_BGR;
  else if (!strncmp (name, "GRAY8", 5))
    return VVAS_VIDEO_FORMAT_GRAY8;
  else
    return VVAS_VIDEO_FORMAT_UNKNOWN;
}

inline std::string lowercase_str(const char* str)
{
  std::string result{ str };
  std::transform(result.begin(), result.end(), result.begin(), ::tolower);
  return result;
}

gboolean
read_ppe_config (void* element, json_t * root, PreProcessInfo* pre_proc)
{
  json_t *value, *config, *quant;
  size_t sz;

  if (!pre_proc){
    GST_ERROR_OBJECT (element, "PreProcessInfo is NULL");
    return FALSE;
  }

  config = json_object_get (root, "preprocess-config");
  if (!config || !json_is_object (config)) {
    GST_INFO_OBJECT (element, "preprocess-config not found");
    return TRUE;
  }

  sz = json_object_size (config);
  if (!sz) {
    GST_INFO_OBJECT (element, "preprocess-config found to be empty");
    return TRUE;
  }

  value = json_object_get (config, "software-ppe");
  if (value) {
    if (!json_is_boolean (value)) {
      GST_ERROR_OBJECT (element, "software-ppe is not a boolean type");
      goto error;
    } else {
      pre_proc->use_software = json_boolean_value (value);
    }
  } else {
    /* If not mentioned, then the ppe used will be accelerated IP */
    pre_proc->use_software = FALSE;
  }

  /* get xclbin location if hw ppe */
  if (!pre_proc->use_software) {
    value = json_object_get (config, "xclbin-location");
    if (json_is_string (value)) {
      pre_proc->xclbin_loc = g_strdup (json_string_value (value));
      GST_INFO_OBJECT (element, "xclbin location to download %s",
          pre_proc->xclbin_loc);
    } else {
      pre_proc->xclbin_loc = NULL;
      GST_ERROR_OBJECT (element, "xclbin path is not set");
      return FALSE;
    }
  }
#if defined(XLNX_PCIe_PLATFORM)
  value = json_object_get (config, "device-index");
  if (!json_is_integer (value)) {
    GST_ERROR_OBJECT (element,
        "device-index is not set in config file");
    goto error;
  }
  pre_proc->dev_idx = json_integer_value (value);
  GST_INFO_OBJECT (element, "preprocess device index %d", pre_proc->dev_idx);
#endif

  pre_proc->core_handle = (VvasCoreModule *) calloc (1, sizeof (VvasCoreModule));
  if (!pre_proc->core_handle) {
    GST_ERROR_OBJECT (element, "failed to allocate memory");
    goto error;
  }

  /* get image process library name */
  value = json_object_get (config, "library-name");
  if (value) {
    if (!json_is_string (value)) {
      GST_ERROR_OBJECT (element, "Library name is not of string type");
      goto error;
    }
    pre_proc->core_handle->name = g_strdup (json_string_value (value));
  } else {
    pre_proc->core_handle->name = NULL;
  }

  GST_INFO_OBJECT (element, "Preprocess Library: %s", pre_proc->core_handle->name);

  /* get image process library configuration string */
  value = json_object_get (config, "library-config");
  if (value) {
    if (!json_is_string (value)) {
      GST_ERROR_OBJECT (element, "Library config is not of string type");
      goto error;
    }
    pre_proc->init_config.lib_config = g_strdup (json_string_value (value));
  } else {
    pre_proc->init_config.lib_config = NULL;
  }

  value = json_object_get (config, "in-mem-bank");
  if (!value || !json_is_integer (value)) {
    pre_proc->in_mem_bank = DEFAULT_MEM_BANK;
    GST_DEBUG_OBJECT (element, "PPE In mem bank not set. taking default : %d",
        pre_proc->in_mem_bank);
  } else {
    pre_proc->in_mem_bank = json_integer_value (value);
    GST_INFO_OBJECT (element, "In mem bank : %d", pre_proc->in_mem_bank);
  }

  value = json_object_get (config, "out-mem-bank");
  if (!value || !json_is_integer (value)) {
    pre_proc->out_mem_bank = DEFAULT_MEM_BANK;
    GST_DEBUG_OBJECT (element, "PPE out mem bank not set. taking default : %d",
        pre_proc->out_mem_bank);
  } else {
    pre_proc->out_mem_bank = json_integer_value (value);
    GST_INFO_OBJECT (element, "out mem bank : %d", pre_proc->out_mem_bank);
  }

  value = json_object_get (config, "interpolation-mode");
  if (!value || !json_is_integer (value)) {
    pre_proc->is_interpolation_mode_set = FALSE;
    GST_DEBUG_OBJECT (element,
        "Interpolation mode is not set. First supported mode will be set.");
  } else {
    int flag = json_integer_value (value);
    pre_proc->is_interpolation_mode_set = TRUE;
    pre_proc->user_interpolation_mode = (VvasImageProcessInterpolationMode) flag;
    GST_INFO_OBJECT (element, "Interpolation mode is set: %d", flag);
  }

  value = json_object_get (config, "maintain-aspect-ratio");
  if (!value || !json_is_integer (value)) {
    pre_proc->param.maintain_aspect_ratio = DEFAULT_MAINTAIN_ASPECT_RATIO;
    GST_DEBUG_OBJECT (element,
        "Maintain aspect ratio is not set. Default not set.");
  } else {
    int flag = json_integer_value (value);
    pre_proc->param.maintain_aspect_ratio =
        flag ? TRUE : DEFAULT_MAINTAIN_ASPECT_RATIO;
    GST_INFO_OBJECT (element, "Maintain aspect ratio is %s",
        pre_proc->param.maintain_aspect_ratio ? "set" : "not set");
  }

  value = json_object_get (config, "symmetric-padding");
  if (!value || !json_is_integer (value)) {
    pre_proc->param.symmetric_padding = DEFAULT_SYMMETRIC_PADDING;
    GST_DEBUG_OBJECT (element, "Symmetric Padding is not set. Default not set.");
  } else {
    int flag = json_integer_value (value);
    pre_proc->param.symmetric_padding =
        flag ? TRUE : DEFAULT_MAINTAIN_ASPECT_RATIO;
    GST_INFO_OBJECT (element, "Symmetric Padding is %s", flag ? "set" : "not set");
  }

  value = json_object_get (config, "image-pad-value");
  if (!value || !json_is_integer (value)) {
    pre_proc->init_value = 0;
    GST_DEBUG_OBJECT (element, "Image Pad Value is not set. taking default : %d",
        pre_proc->init_value);
  } else {
    pre_proc->init_value = json_integer_value (value);
    GST_INFO_OBJECT (element, "Image Pad Value : %d", pre_proc->init_value);
  }

  value = json_object_get (config, "mean-r");
  if (!value || !json_is_real (value)) {
    GST_ERROR_OBJECT (element, "mean-r is not set");
    goto error;
  } else {
    pre_proc->param.mean_r = (float) json_real_value (value);
  }

  value = json_object_get (config, "mean-g");
  if (!value || !json_is_real (value)) {
    GST_ERROR_OBJECT (element, "mean-g is not set");
    goto error;
  } else {
    pre_proc->param.mean_g = (float) json_real_value (value);
  }

  value = json_object_get (config, "mean-b");
  if (!value || !json_is_real (value)) {
    GST_ERROR_OBJECT (element, "mean-b is not set.");
    goto error;
  } else {
    pre_proc->param.mean_b = (float) json_real_value (value);
  }

  value = json_object_get (config, "scale-r");
  if (!value || !json_is_real (value)) {
    GST_ERROR_OBJECT (element, "scale-r is not set.");
    goto error;
  } else {
    pre_proc->param.scale_r = (float) json_real_value (value);
  }

  value = json_object_get (config, "scale-g");
  if (!value || !json_is_real (value)) {
    GST_ERROR_OBJECT (element, "scale-g is not set.");
    goto error;
  } else {
    pre_proc->param.scale_g = (float) json_real_value (value);
  }

  value = json_object_get (config, "scale-b");
  if (!value || !json_is_real (value)) {
    GST_ERROR_OBJECT (element, "scale-b is not set.");
    goto error;
  } else {
    pre_proc->param.scale_b = (float) json_real_value (value);
  }

  pre_proc->quant_data.scale_factor = 1.0;
  pre_proc->quant_data.zero_point = 0.0;
  quant = json_object_get (config, "quantization");
  if (!quant || !json_is_object (quant)) {
    GST_INFO_OBJECT (element, "quantization information not found");
  }

  if (quant) {
    value = json_object_get (quant, "scale-factor");
    if (!value || !json_is_real (value)) {
      GST_DEBUG_OBJECT (element, "scale-factor is not set.");
    } else {
      pre_proc->quant_data.scale_factor = (float) json_real_value (value);
      if (is_valid_positive_scale(pre_proc->quant_data.scale_factor)) {
        pre_proc->is_quant_set = true;
      } else {
        GST_ERROR_OBJECT (element, "Invalid scale-factor %f. It should be a positive non-zero value.",
            pre_proc->quant_data.scale_factor);
        goto error;
      }
      GST_DEBUG_OBJECT (element, "scale-factor is %f", pre_proc->quant_data.scale_factor);
    }

    value = json_object_get (quant, "zero-point");
    if (!value || !json_is_real (value)) {
      GST_DEBUG_OBJECT (element, "zero-point is not set.");
    } else {
      pre_proc->quant_data.zero_point = (float) json_real_value (value);
      GST_DEBUG_OBJECT (element, "zero-point is %f", pre_proc->quant_data.zero_point);
    }
  }

  pre_proc->enabled = true;
  return TRUE;

error:
  /* Free any partially allocated resources before returning */
  if (pre_proc->init_config.lib_config) {
    g_free ((gpointer) pre_proc->init_config.lib_config);
    pre_proc->init_config.lib_config = NULL;
  }
  if (pre_proc->core_handle) {
    if (pre_proc->core_handle->name) {
      g_free (pre_proc->core_handle->name);
      pre_proc->core_handle->name = NULL;
    }
    free (pre_proc->core_handle);
    pre_proc->core_handle = NULL;
  }
  if (pre_proc->xclbin_loc) {
    g_free (pre_proc->xclbin_loc);
    pre_proc->xclbin_loc = NULL;
  }
  /* print to console */
  GST_ELEMENT_ERROR (element, RESOURCE, FAILED,
      ("pre-process file parse error"), (NULL));
  return FALSE;
}

static gboolean read_onnxrt_config(void* element, json_t* config, InferInfo* infer)
{
  json_t *value;
  json_t *onnx_config, *vitisai_config {nullptr};

  onnx_config = json_object_get (config, "onnxrt-config");
  if (!onnx_config || !json_is_object (onnx_config)) {
    GST_INFO_OBJECT (element, "onnxrt-config not found");
    return FALSE;
  }

  value = json_object_get (onnx_config, "onnx-model-path");
  if (!json_is_string (value)) {
    GST_ERROR_OBJECT (element, "onnx-model-path is not set");
    return FALSE;
  } else {
    infer->ort_info.model_path = json_string_value (value);
    GST_DEBUG_OBJECT (element, "onnx-model-path: %s", infer->ort_info.model_path.c_str());
  }

  value = json_object_get (onnx_config, "input-tensor-layout");
  if (!value || !json_is_string (value)) {
    GST_ERROR_OBJECT (element, "input-tensor-layout is not set");
    return FALSE;
  } else {
    /* Assign the input tensor layout, which specifies the arrangement of data (e.g., NCHW or NHWC)
     * in the input tensor for the inference model. This is crucial for ensuring compatibility
     * between the model and the input data format. 
     * NOTE:- This is only needed for onnxrt, for vart we get this data by querying input tensors.*/
    const char *layout_str = json_string_value(value);
    if (strcmp(layout_str, "NCHW") == 0 || strcmp(layout_str, "nchw") == 0) {
      infer->ort_info.input_tensor_layout = "NCHW";
    } else if (strcmp(layout_str, "NHWC") == 0 || strcmp(layout_str, "nhwc") == 0) {
      infer->ort_info.input_tensor_layout = "NHWC";
    } else {
      GST_ERROR_OBJECT(element, "input-tensor-layout must be 'NCHW' or 'nchw' or 'NHWC' or 'nhwc', got: %s",
          layout_str);
      return FALSE;
    }
    GST_DEBUG_OBJECT(element, "input-tensor-layout: %s",
      infer->ort_info.input_tensor_layout.c_str());
  }

  value = json_object_get (onnx_config, "enable-profiling");
  if (!value || !json_is_boolean (value)) {
    GST_DEBUG_OBJECT (element, "enable-profiling is not set");
    infer->ort_info.enable_profiling = false;
  } else {
    infer->ort_info.enable_profiling = json_boolean_value (value);
    GST_DEBUG_OBJECT (element, "enable-profiling is %s",
        infer->ort_info.enable_profiling ? "set" : "not set");
  }

  if (infer->ort_info.enable_profiling) {
    value = json_object_get (onnx_config, "profiling-file-path");
    if (!value || !json_is_string (value)) {
      GST_DEBUG_OBJECT (element, "profiling-file-path is not set");
      infer->ort_info.profiling_file_path = "";
    } else {
      infer->ort_info.profiling_file_path = json_string_value (value);
      GST_DEBUG_OBJECT (element, "profiling-file-path: %s",
          infer->ort_info.profiling_file_path.c_str());
    }
  }

  infer->ort_info.ep = OnnxRuntimeEP::UNKNOWN;
  value = json_object_get (onnx_config, "onnxrt-ep");
  if (!value || !json_is_string (value)) {
    infer->ort_info.ep = OnnxRuntimeEP::CPU;
    GST_INFO_OBJECT (element, "onnxrt-ep config is not set defaulting to CPU");
    return FALSE;
  } else {
    std::string tmp{ lowercase_str(json_string_value (value)) };
    if (tmp == "cpu") {
      infer->ort_info.ep = OnnxRuntimeEP::CPU;
    } else if (tmp == "vitisai") {
      infer->ort_info.ep = OnnxRuntimeEP::VITIS_AI;
    } else {
      GST_ERROR_OBJECT (element, "Invalid value [%s] specified for onnxrt-ep should be one of [%s, %s]",
           tmp.c_str(), "cpu", "vitisai");
      return FALSE;
    }
    GST_DEBUG_OBJECT (element, "onnxrt-ep config is set to %s", tmp.c_str());
  }

  if(infer->ort_info.ep == OnnxRuntimeEP::VITIS_AI) {
    vitisai_config = json_object_get (onnx_config, "vitisai-ep-config");
    if (!vitisai_config || !json_is_object (vitisai_config)) {
      GST_ERROR_OBJECT (element, "onnxrt-ep config set to vitis_ai but vitisai-ep-config not provided");
      return FALSE;
    }
  }

  if (vitisai_config) {
    value = json_object_get (vitisai_config, "ai-analyzer-profiling");
    if (!value || !json_is_boolean (value)) {
      GST_DEBUG_OBJECT (element, "ai-analyzer-profiling is not set");
      GST_DEBUG_OBJECT (element, "setting ai-analyzer-profiling to false as default");
      infer->ort_info.vai_conf.ai_analyzer_profiling = false;
    } else {
      infer->ort_info.vai_conf.ai_analyzer_profiling = json_boolean_value (value);
      GST_DEBUG_OBJECT (element, "ai-analyzer-profiling is %s",
          infer->ort_info.vai_conf.ai_analyzer_profiling ? "true" : "false");
    }

    value = json_object_get (vitisai_config, "ai-analyzer-visualization");
    if (!value || !json_is_boolean (value)) {
      GST_DEBUG_OBJECT (element, "ai-analyzer-visualization is not set");
      GST_DEBUG_OBJECT (element, "setting ai-analyzer-visualization to false as default");
      infer->ort_info.vai_conf.ai_analyzer_visualization = false;
    } else {
      infer->ort_info.vai_conf.ai_analyzer_visualization = json_boolean_value (value);
      GST_DEBUG_OBJECT (element, "ai-analyzer-visualization is %s",
        infer->ort_info.vai_conf.ai_analyzer_visualization ? "true" : "false");
    }

    value = json_object_get (vitisai_config, "config-file-path");
    if (!value || !json_is_string (value)) {
      GST_ERROR_OBJECT (element, "vitisai-ep-config :: config-file-path is not set");
      return FALSE;
    } else {
      infer->ort_info.vai_conf.file_path = json_string_value (value);
      GST_DEBUG_OBJECT (element, "vitisai-ep-config :: config-file-path: %s",
          infer->ort_info.vai_conf.file_path.c_str());
    }

    value = json_object_get (vitisai_config, "cache-dir");
    if (!value || !json_is_string (value)) {
      GST_WARNING_OBJECT (element, "cache-dir is not set, using default: /tmp/$USER/vaip/.cache");
      infer->ort_info.vai_conf.cache_dir = "";
    } else {
      infer->ort_info.vai_conf.cache_dir = json_string_value (value);
      GST_DEBUG_OBJECT (element, "cache-dir is set: %s",
          infer->ort_info.vai_conf.cache_dir.c_str());
      if (!std::filesystem::is_directory (infer->ort_info.vai_conf.cache_dir)) {
        GST_WARNING_OBJECT (element, "cache-dir %s does not exist,"
            " inference may run on CPU instead of hardware accelerator",
            infer->ort_info.vai_conf.cache_dir.c_str());
      }
    }

    value = json_object_get (vitisai_config, "cache-key");
    if (!value || !json_is_string (value)) {
      GST_WARNING_OBJECT (element, "cache-key is not set, using default hash value key");
      infer->ort_info.vai_conf.cache_key = "";
    } else {
      infer->ort_info.vai_conf.cache_key = json_string_value (value);
      GST_DEBUG_OBJECT (element, "cache-key is set: %s",
          infer->ort_info.vai_conf.cache_key.c_str());
      std::filesystem::path rai_path =
          std::filesystem::path (infer->ort_info.vai_conf.cache_dir) /
          infer->ort_info.vai_conf.cache_key /
          (infer->ort_info.vai_conf.cache_key + ".rai");
      if (!std::filesystem::exists (rai_path)) {
        GST_WARNING_OBJECT (element, "cache-key %s may be incorrect:"
            " expected compiled model not found at %s,"
            " verify cache-key in vitisai-ep-config,"
            " inference may run on CPU instead of hardware accelerator",
            infer->ort_info.vai_conf.cache_key.c_str(),
            rai_path.c_str());
      }
    }
  }
  return TRUE;
}

static gboolean 
read_vart_config (void* element, json_t* config, InferInfo* infer)
{
  json_t *value;
  json_t *vart_config{nullptr};

  vart_config = json_object_get (config, "vart-config");
  if (!vart_config || !json_is_object (vart_config)) {
    GST_INFO_OBJECT (element, "vart-config not found");
    return FALSE;
  }

  value = json_object_get (vart_config, "vart-model-path");
  if (!json_is_string (value)) {
    GST_ERROR_OBJECT (element, "vart-model-path is not set");
    return FALSE;
  } else {
    infer->vart_info.model_path = json_string_value (value);
    GST_DEBUG_OBJECT (element, "vart-model-path: %s", infer->vart_info.model_path.c_str());
  }

  value = json_object_get (vart_config, "ai-analyzer-profiling");
  if (!value || !json_is_boolean (value)) {
    GST_DEBUG_OBJECT (element, "ai-analyzer-profiling is not set");
    GST_DEBUG_OBJECT (element, "setting ai-analyzer-profiling to false as default");
    infer->vart_info.ai_analyzer_profiling = false;
  } else {
    infer->vart_info.ai_analyzer_profiling = json_boolean_value (value);
    GST_DEBUG_OBJECT (element, "ai-analyzer-profiling is %s",
          infer->vart_info.ai_analyzer_profiling ? "true" : "false");
  }

  infer->vart_info.inp_tensor_type = vart::TensorType::HW;
  infer->vart_info.out_tensor_type = vart::TensorType::HW;

  value = json_object_get (vart_config, "input-tensor-type");
  if (!value || !json_is_string (value)) {
    GST_INFO_OBJECT (element, "input-tensor-type is not set using default HW");
  } else {
    std::string tmp{ lowercase_str(json_string_value (value)) };
    if (tmp == "cpu") {
      infer->vart_info.inp_tensor_type = vart::TensorType::CPU;
    } else if (tmp == "hw") {
      infer->vart_info.inp_tensor_type = vart::TensorType::HW;
    } else {
      GST_ERROR_OBJECT (element, "Invalid value [%s] specified for input-tensor-type should be one of [%s, %s]",
           tmp.c_str(), "cpu", "hw");
      return FALSE;
    }
    GST_DEBUG_OBJECT (element, "input-tensor-type config is set to %s", tmp.c_str());
  }

  value = json_object_get (vart_config, "output-tensor-type");
  if (!value || !json_is_string (value)) {
    GST_INFO_OBJECT (element, "output-tensor-type is not set using default HW");
  } else {
    std::string tmp{ lowercase_str(json_string_value (value)) };
    if (tmp == "cpu") {
      infer->vart_info.out_tensor_type = vart::TensorType::CPU;
    } else if (tmp == "hw") {
      infer->vart_info.out_tensor_type = vart::TensorType::HW;
    } else {
      GST_ERROR_OBJECT (element, "Invalid value [%s] specified for output-tensor-type should be one of [%s, %s]",
           tmp.c_str(), "cpu", "hw");
      return FALSE;
    }
    GST_DEBUG_OBJECT (element, "output-tensor-type config is set to %s", tmp.c_str());
  }

  if(infer->vart_info.inp_tensor_type != infer->vart_info.out_tensor_type) {
    GST_ERROR_OBJECT (element, "input-tensor-type and output-tensor-type must be the same provided [%s, %s]", 
        infer->vart_info.inp_tensor_type == vart::TensorType::CPU ? "cpu" : "hw", 
        infer->vart_info.out_tensor_type == vart::TensorType::CPU ? "cpu" : "hw");
    return FALSE;
  }

  value = json_object_get (vart_config, "out-mem-bank");
  if (!value || !json_is_integer (value)) {
    int32_t idx = DEFAULT_MEM_BANK;
    if(infer->vart_info.out_tensor_type == vart::TensorType::HW){
      idx = DEFAULT_MBANK_IDX;
      GST_DEBUG_OBJECT (element, "Out-Mem-Bank not set for HW output-tensor-type. taking default : %d",
          idx);
    }
    infer->vart_info.mbank_idx = idx;
  } else {
    infer->vart_info.mbank_idx = json_integer_value (value);
    GST_INFO_OBJECT (element, "Out-Mem-Bank bank : %d", infer->vart_info.mbank_idx);
  }

  value = json_object_get (vart_config, "aie-columns-sharing");
  if (!value || !json_is_boolean (value)) {
    GST_DEBUG_OBJECT (element, "aie-columns-sharing is not set");
    infer->vart_info.is_columns_sharing_option_provided = false;
  } else {
    infer->vart_info.aie_columns_sharing = json_boolean_value (value);
    infer->vart_info.is_columns_sharing_option_provided = true;
    GST_DEBUG_OBJECT (element, "aie-columns-sharing: %s", infer->vart_info.aie_columns_sharing ? "true" : "false");
  }

  value = json_object_get (vart_config, "start-column");
  if (!value || !json_is_integer (value)) {
    GST_DEBUG_OBJECT (element, "start-column is not set");
    infer->vart_info.is_start_column_option_provided = false;
  } else {
    infer->vart_info.start_column = json_integer_value (value);
    infer->vart_info.is_start_column_option_provided = true;
    if (json_integer_value (value) < 0) {
      GST_WARNING_OBJECT (element, "start-column value is less than 0, not setting any start-column value");
      infer->vart_info.is_start_column_option_provided = false;
    }
    GST_DEBUG_OBJECT (element, "start-column: %d", infer->vart_info.start_column);
  }

  value = json_object_get (vart_config, "config-file-path");
  if (!value || !json_is_string (value)) {
    GST_DEBUG_OBJECT (element, "config-file-path is not set, setting config-file-path to empty as default");
    infer->vart_info.config_file_path = "";
  } else {
    infer->vart_info.config_file_path = (json_string_value (value));
    GST_DEBUG_OBJECT (element, "config-json-path: %s", infer->vart_info.config_file_path.c_str());
  }

  return TRUE;
}

gboolean
read_infer_config (void* element, json_t * root, InferInfo* infer)
{
  json_t *value, *config, *label;
  gboolean is_maxwidth_configured = FALSE;
  gboolean is_maxheight_configured = FALSE;
  gboolean ret;

  if (!infer) {
    GST_ERROR_OBJECT (element, "InferInfo is NULL");
    return FALSE;
  }

  /* Set default values for non mandatory parameter */
  infer->level = DEFAULT_INFER_LEVEL;
  infer->batch_size = BATCH_SIZE_ZERO;
  infer->low_latency = TRUE;
  infer->attach_ppebuf = FALSE;

  /* get vvas kernel lib internal configuration */
  config = json_object_get (root, "infer-config");
  if (!json_is_object (config)) {
    GST_ERROR_OBJECT (element, "config is not of object type");
    goto error;
  }
  GST_DEBUG_OBJECT (element, "infer config size = %lu", json_object_size (config));

  infer->runtime = MLRuntime::UNKNOWN;
  value = json_object_get (config, "inference-backend-runtime");
  if (!value || !json_is_string (value)) {
    GST_INFO_OBJECT (element, "inference-backend-runtime is not set, defaulting to onnxrt");
    infer->runtime = MLRuntime::ONNXRT;
  } else {
    std::string tmp { lowercase_str (json_string_value (value)) };
    if (tmp == "onnxrt") {
        infer->runtime = MLRuntime::ONNXRT;
    } else if (tmp == "vart") {
        infer->runtime = MLRuntime::VART;
    } else {
        GST_ERROR_OBJECT (element, "Invalid value [%s] provided for inference-backend-runtime"
            "should be one of [onnxrt, vart]", tmp.c_str());
        goto error;
    }
    GST_DEBUG_OBJECT (element, "inference-backend-runtime: %s", tmp.c_str());
  }

  if (infer->runtime == MLRuntime::ONNXRT || infer->runtime == MLRuntime::AUTO) {
    ret = read_onnxrt_config (element, config, infer);
    if(ret){
      infer->ort_info.valid = true;
    }
  }

  if (infer->runtime == MLRuntime::VART || infer->runtime == MLRuntime::AUTO) {
    ret = read_vart_config (element, config, infer);
    if(ret){
      infer->vart_info.valid = true;
    }
  }

  if (infer->runtime == MLRuntime::ONNXRT && !infer->ort_info.valid) {
    GST_ERROR_OBJECT (element, "ONNXRT configuration not found or invalid");
    goto error;
  }

  if (infer->runtime == MLRuntime::VART && !infer->vart_info.valid) {
    GST_ERROR_OBJECT (element, "VART configuration not found or invalid");
    goto error;
  }

  if (infer->runtime == MLRuntime::AUTO) {
    /* First Preference is onnx */
    if (infer->ort_info.valid) {
      infer->runtime = MLRuntime::ONNXRT;
    } else if (infer->vart_info.valid) {
      infer->runtime = MLRuntime::VART;
    } else {
      GST_ERROR_OBJECT (element, "AUTO configuration neither vart-config not onnxrt-config is valid!!");
      goto error;
    }
  }

  value = json_object_get (config, "model-format");
  if (!json_is_string (value)) {
    GST_ERROR_OBJECT (element, "model-format is not set");
    goto error;
  } else {
    infer->model_format = get_vvas_video_fmt (json_string_value (value));
    if (VVAS_VIDEO_FORMAT_UNKNOWN == infer->model_format) {
      GST_ERROR_OBJECT (element, "Unknown model-format %s : supported values are [RGB, BGR]", json_string_value (value));
      goto error;
    }
  }

  value = json_object_get (config, "batch-size");
  if (json_is_integer (value)) {
    infer->batch_size = json_integer_value (value);
    if (infer->batch_size > INFER_MAX_BATCH_SIZE) {
      GST_ERROR_OBJECT (element, "batch-size should not > %d",
          INFER_MAX_BATCH_SIZE);
      goto error;
    }
  }

  value = json_object_get (config, "inference-level");
  if (json_is_integer (value)) {
    infer->level = json_integer_value (value);
    if (infer->level < 1) {
      GST_ERROR_OBJECT (element,
          "inference-level %d can't be less than 1", infer->level);
      goto error;
    }
  }

  /* making batch size as default max-queue-size */
  infer->max_queue = infer->batch_size;

  value = json_object_get (config, "low-latency");
  if (value) {
    if (!json_is_boolean (value)) {
      GST_ERROR_OBJECT (element, "low-latency is not a boolean type");
      goto error;
    }

    infer->low_latency = json_boolean_value (value);
    GST_INFO_OBJECT (element, "setting low-latency to %d",
        infer->low_latency);
  }

  value = json_object_get (config, "inference-max-queue");
  if (!json_is_integer (value)) {
    GST_WARNING_OBJECT (element, "inference-max-queue is not set."
        "taking batch-size %d as default", infer->batch_size);
  } else {
    infer->max_queue = json_integer_value (value);
    if (infer->max_queue < infer->batch_size) {
      GST_WARNING_OBJECT (element, "inference-max-queue can't be less than "
          "batch-size. taking batch-size %d as default queue length",
          infer->batch_size);
      infer->max_queue = infer->batch_size;
    } else {
      GST_INFO_OBJECT (element, "setting inference-max-queue to %d",
          infer->max_queue);
    }
  }

  value = json_object_get (config, "attach-ppe-outbuf");
  if (value) {
    if (!json_is_boolean (value)) {
      GST_ERROR_OBJECT (element, "attach-ppe-outbuf is not a boolean type");
      goto error;
    }

    infer->attach_ppebuf = json_boolean_value (value);
    GST_INFO_OBJECT (element, "setting attach-ppe-outbuf to %d",
        infer->attach_ppebuf);
  }

  value = json_object_get (config, "input-obj-min-width");
  if (value) {
    if (!json_is_integer (value)) {
      GST_ERROR_OBJECT (element, "No input-obj-min-width in configuration");
      goto error;
    }
    infer->input_obj_min_width = json_integer_value (value);
    GST_INFO_OBJECT (element, "setting infer->input_obj_min_width to %d",
        infer->input_obj_min_width);
  } else {
    GST_INFO_OBJECT (element, "No input-obj-min-width in configuration "
        "setting input-obj-min-width with %d", DEFAULT_INPUTOBJ_MIN_WIDTH);
    infer->input_obj_min_width = DEFAULT_INPUTOBJ_MIN_WIDTH;
  }

  value = json_object_get (config, "input-obj-min-height");
  if (value) {
    if (!json_is_integer (value)) {
      GST_ERROR_OBJECT (element, "No input-obj-min-height in configuration");
      goto error;
    }
    infer->input_obj_min_height = json_integer_value (value);
    GST_INFO_OBJECT (element, "setting infer->input_obj_min_height to %d",
        infer->input_obj_min_height);
  } else {
    GST_INFO_OBJECT (element, "No input-obj-min-height in configuration "
        "setting input-obj-min-height with %d", DEFAULT_INPUTOBJ_MIN_HEIGHT);
    infer->input_obj_min_height = DEFAULT_INPUTOBJ_MIN_HEIGHT;
  }

  value = json_object_get (config, "input-obj-max-width");
  if (!value || !json_is_integer (value)) {
    GST_INFO_OBJECT (element, "No input-obj-max-width in configuration "
        "setting input-obj-max-width with %d", DEFAULT_INPUTOBJ_MAX_WIDTH);
    infer->input_obj_max_width = DEFAULT_INPUTOBJ_MAX_WIDTH;
  } else {
    infer->input_obj_max_width = json_integer_value (value);
    GST_INFO_OBJECT (element, "setting infer->input_obj_max_width to %d",
        infer->input_obj_max_width);
    is_maxwidth_configured = TRUE;
  }

  value = json_object_get (config, "input-obj-max-height");
  if (!value || !json_is_integer (value)) {
    GST_INFO_OBJECT (element, "No input-obj-max-height in configuration "
        "setting input-obj-max-height with %d", DEFAULT_INPUTOBJ_MAX_HEIGHT);
    infer->input_obj_max_height = DEFAULT_INPUTOBJ_MAX_HEIGHT;
  } else {
    infer->input_obj_max_height = json_integer_value (value);
    GST_INFO_OBJECT (element, "setting infer->input_obj_max_height to %d",
        infer->input_obj_max_height);
    is_maxheight_configured = TRUE;
  }

  /* validation for input object min width */
  if ((infer->input_obj_min_width < DEFAULT_INPUTOBJ_MIN_WIDTH) ||
      (infer->input_obj_min_width > DEFAULT_INPUTOBJ_MAX_WIDTH)) {
    GST_ERROR_OBJECT (element,
        "input-obj-min-width value is not within the range. It should "
        "be > %d and < %d", DEFAULT_INPUTOBJ_MIN_WIDTH,
        DEFAULT_INPUTOBJ_MAX_WIDTH);
    goto error;
  }

  /* validation for input object min height */
  if ((infer->input_obj_min_height < DEFAULT_INPUTOBJ_MIN_HEIGHT) ||
      (infer->input_obj_min_height > DEFAULT_INPUTOBJ_MAX_HEIGHT)) {
    GST_ERROR_OBJECT (element,
        "input-obj-min-height value is not within the range. It should "
        "be > %d and < %d", DEFAULT_INPUTOBJ_MIN_HEIGHT,
        DEFAULT_INPUTOBJ_MAX_HEIGHT);
    goto error;
  }

  /* validation for input object max width */
  if ((infer->input_obj_max_width < DEFAULT_INPUTOBJ_MIN_WIDTH) ||
      (infer->input_obj_max_width > DEFAULT_INPUTOBJ_MAX_WIDTH)) {
    if (is_maxwidth_configured) {
      GST_ERROR_OBJECT (element,
          "input-obj-max-width value is not within the range. It should "
          "be > %d and < %d", DEFAULT_INPUTOBJ_MIN_WIDTH,
          DEFAULT_INPUTOBJ_MAX_WIDTH);
      goto error;
    }
    GST_WARNING_OBJECT (element,
        "input-obj-max-width value is not within the range "
        "setting input-obj-max-width as %d", DEFAULT_INPUTOBJ_MAX_WIDTH);
    infer->input_obj_max_width = DEFAULT_INPUTOBJ_MAX_WIDTH;
  }

  /* validation for input object max height */
  if ((infer->input_obj_max_height < DEFAULT_INPUTOBJ_MIN_HEIGHT) ||
      (infer->input_obj_max_height > DEFAULT_INPUTOBJ_MAX_HEIGHT)) {
    if (is_maxheight_configured) {
      GST_ERROR_OBJECT (element,
          "input-obj-max-height value is not within the range. It should "
          "be > %d and < %d", DEFAULT_INPUTOBJ_MIN_HEIGHT,
          DEFAULT_INPUTOBJ_MAX_HEIGHT);
      goto error;
    }
    GST_WARNING_OBJECT (element,
        "input-obj-max-height value is not within the range "
        "setting input-obj-max-height as %d", DEFAULT_INPUTOBJ_MAX_HEIGHT);
    infer->input_obj_max_height = DEFAULT_INPUTOBJ_MAX_HEIGHT;
  }

  value = json_object_get (config, "input-class-filters");
  if (json_is_array (value) && infer->level > 1) {
    infer->num_input_class_filters = json_array_size (value);
    for (int i = 0; i < infer->num_input_class_filters; i++) {
      label = json_array_get (value, i);
      if (json_is_string (label)) {
        infer->input_class_filters =
            g_list_append (infer->input_class_filters,
            g_strdup ((char *) json_string_value (label)));
        GST_DEBUG_OBJECT (element, "Adding input filter label: %s",
            (char *) g_list_last (infer->input_class_filters)->data);
      } else {
        GST_DEBUG_OBJECT (element, "Input Filter label %d is not of string type",
            i + 1);
        infer->num_input_class_filters--;
      }
    }
  }

  value = json_object_get (config, "attach-empty-metadata");
  if (value) {
    if (!json_is_boolean (value)) {
      GST_ERROR_OBJECT (element, "attach-empty-metadata is not a boolean type");
      goto error;
    }

    infer->attach_empty_meta = json_boolean_value (value);
    GST_INFO_OBJECT (element, "setting attach-empty-metadata to %d",
        infer->attach_empty_meta);
  }

  GST_INFO_OBJECT (element, "inference-level = %d and batch-size = %d",
      infer->level, infer->batch_size);

  return TRUE;

error:
  /* print to console */
  GST_ELEMENT_ERROR (element, RESOURCE, FAILED,
      ("infer config file parse error"), (NULL));
  return FALSE;
}

gboolean
read_postprocess_config (void* element, json_t * root, PostProcessInfo* post_proc)
{
  json_t *value, *config, *dequant;

  if (!post_proc) {
    GST_ERROR_OBJECT (element, "PostProcessInfo is NULL");
    return FALSE;
  }

  config = json_object_get (root, "postprocess-config");
  if (!config || !json_is_object (config)) {
    GST_ERROR_OBJECT (element, "post-process config not found");
    return FALSE;
  }

  value = json_object_get (config, "library-path");
  if (json_is_string (value)) {
    post_proc->library_path = g_strdup (json_string_value (value));
    GST_DEBUG_OBJECT (element, "post-processing library %s",
        (char *) json_string_value (value));
    post_proc->enabled = TRUE;
  } else {
    GST_ERROR_OBJECT (element, "post-process library-path not set");
    return FALSE;
  }

  value = json_object_get (config, "library-config");
  if (!value || !json_is_object (value)) {
    GST_INFO_OBJECT (element, "post-process library-config not set");
    return FALSE;
  }

  post_proc->json_string = json_dumps (value, JSON_ENCODE_ANY);
  if (!post_proc->json_string) {
    GST_ERROR_OBJECT (element, "Failed to dump postprocess json to string");
    return FALSE;
  }

  post_proc->dequant_data.clear();
  dequant = json_object_get (config, "dequantization");
  if (!dequant || !json_is_object (dequant)) {
    GST_INFO_OBJECT (element, "quantization information not found");
  }

  if (dequant) {
    auto scales = parse_float_or_float_list(element, dequant, "scale-factor");
    auto zeros  = parse_float_or_float_list(element, dequant, "zero-point");

    // Assumptions:
    // - scale-factor must be present (scalar or array; scalar implies one output tensor)
    // - zero-point is optional; if absent => 0 for all outputs
    // - if zero-point is provided, it must match scale-factor count
    if (!scales.empty()) {
      for (auto& scale: scales) {
        if (is_valid_positive_scale(scale)) {
          continue;
        } else {
          GST_ERROR_OBJECT (element, "Invalid scale-factor value [%f] provided for dequantization."
             " It should be a positive number.",
              scale);
          return FALSE;
        }
      }
      post_proc->is_dequant_set = TRUE;
    }
    if (scales.empty()) {
      scales.push_back(1.0f);
      GST_DEBUG_OBJECT (element, "dequantization.scale-factor not set; defaulting to 1.0");
    }
    if (zeros.empty()) {
      zeros.assign(scales.size(), 0.0f);
    } else if (zeros.size() != scales.size()) {
      GST_ERROR_OBJECT (element,
        "dequantization config mismatch: scale-factor has %zu entries, zero-point has %zu entries",
        scales.size(), zeros.size());
      return FALSE;
    }

    post_proc->dequant_data.resize(scales.size());
    for (size_t i = 0; i < scales.size(); ++i) {
      post_proc->dequant_data[i].scale_factor = scales[i];
      post_proc->dequant_data[i].zero_point = zeros[i];
    }

    GST_DEBUG_OBJECT (element,
      "postprocess dequantization: %zu entries (scale-factor required, zero-point optional)",
      post_proc->dequant_data.size());
  }

  GST_DEBUG_OBJECT (element, "postprocess-config: %s",
      post_proc->json_string);

  return TRUE;
}

VvasVideoFormat
get_tensor_format (VvasVideoFormat model_format, std::string& layout, VvasTensorDataType data_type)
{
  if (model_format != VVAS_VIDEO_FORMAT_RGB && model_format != VVAS_VIDEO_FORMAT_BGR) {
    GST_ERROR ("Only RGB and BGR model format is supported");
    return VVAS_VIDEO_FORMAT_UNKNOWN;
  }

  if (layout != "NCHW" && layout != "NHWC" && layout != "HCWNC4" ) {
    GST_ERROR ("Unknown layout: %s", layout.c_str());
    return VVAS_VIDEO_FORMAT_UNKNOWN;
  }

  if (data_type != VVAS_TENSOR_DATA_TYPE_FLOAT32 &&
      data_type != VVAS_TENSOR_DATA_TYPE_INT8 && 
      data_type != VVAS_TENSOR_DATA_TYPE_BF16 && 
      data_type != VVAS_TENSOR_DATA_TYPE_FP16) {
    GST_ERROR ("Unsupported tensor data type: %d", data_type);
    return VVAS_VIDEO_FORMAT_UNKNOWN;
  }

  switch (model_format) {
    case VVAS_VIDEO_FORMAT_RGB:
    {
      switch (data_type) {
        case VVAS_TENSOR_DATA_TYPE_FLOAT32:
          return layout == "NCHW" ? VVAS_VIDEO_FORMAT_RGBP_FLOAT : VVAS_VIDEO_FORMAT_RGB_FLOAT;
        case VVAS_TENSOR_DATA_TYPE_INT8:
          if (layout == "HCWNC4") {
            return VVAS_VIDEO_FORMAT_RGBx;
          } else {
            return layout == "NCHW" ? VVAS_VIDEO_FORMAT_RGBP : VVAS_VIDEO_FORMAT_RGB;
          }
        case VVAS_TENSOR_DATA_TYPE_BF16:
          if (layout == "HCWNC4") return VVAS_VIDEO_FORMAT_RGBx_BF16;
          return layout == "NCHW" ? VVAS_VIDEO_FORMAT_RGBP_BF16 : VVAS_VIDEO_FORMAT_RGB_BF16;
        case VVAS_TENSOR_DATA_TYPE_FP16:
          if (layout == "HCWNC4") return VVAS_VIDEO_FORMAT_RGBx_FP16;
          return layout == "NCHW" ? VVAS_VIDEO_FORMAT_RGBP_FP16 : VVAS_VIDEO_FORMAT_RGB_FP16;
        default:
          return VVAS_VIDEO_FORMAT_UNKNOWN;
      }
    }
    case VVAS_VIDEO_FORMAT_BGR:
    {
      switch (data_type) {
        case VVAS_TENSOR_DATA_TYPE_FLOAT32:
          return layout == "NCHW" ? VVAS_VIDEO_FORMAT_UNKNOWN : VVAS_VIDEO_FORMAT_BGR_FLOAT;
        case VVAS_TENSOR_DATA_TYPE_INT8:
          return layout == "NCHW" ? VVAS_VIDEO_FORMAT_UNKNOWN : VVAS_VIDEO_FORMAT_BGR;
        case VVAS_TENSOR_DATA_TYPE_BF16:
          if (layout == "HCWNC4") return VVAS_VIDEO_FORMAT_BGRx_BF16;
          return layout == "NCHW" ? VVAS_VIDEO_FORMAT_BGRP_BF16 : VVAS_VIDEO_FORMAT_BGR_BF16;
        case VVAS_TENSOR_DATA_TYPE_FP16:
          if (layout == "HCWNC4") return VVAS_VIDEO_FORMAT_BGRx_FP16;
          return layout == "NCHW" ? VVAS_VIDEO_FORMAT_BGRP_FP16 : VVAS_VIDEO_FORMAT_BGR_FP16;
        default:
          return VVAS_VIDEO_FORMAT_UNKNOWN;
      }
    }
    default:
      break;
  }
  return VVAS_VIDEO_FORMAT_UNKNOWN;
}

VvasVideoFormat
get_tensor_format (VvasVideoFormat model_format, vart::MemoryLayout layout, VvasTensorDataType data_type)
{
  std::string layout_str;
  if (layout == vart::MemoryLayout::NCHW) {
    layout_str = "NCHW";
  } else if (layout == vart::MemoryLayout::NHWC) {
    layout_str = "NHWC";
  } else if (layout == vart::MemoryLayout::HCWNC4) {
    layout_str = "HCWNC4";
  } else {
    GST_ERROR ("Unknown layout");
    return VVAS_VIDEO_FORMAT_UNKNOWN;
  }
  return get_tensor_format(model_format, layout_str, data_type);
}

std::vector<int64_t>
get_fixed_shape (const std::vector<int64_t>& shapes, guint user_batch_size)
{
  std::vector<int64_t> fixed_shape { shapes };

  if(fixed_shape[0] != -1) {
    return fixed_shape;
  }

  if (user_batch_size == 0) {
    GST_CAT_DEBUG (GST_CAT_DEFAULT, "User batch size is 0, using default 1");
    user_batch_size = 1;
  }
  fixed_shape[0] = user_batch_size;
  return fixed_shape;
}

std::optional<std::string>
get_runtime_target (const std::string& vitis_config_file)
{
  json_t *root;
  json_error_t error;
  json_t *target_value;

  root = json_load_file(vitis_config_file.c_str(), 0, &error);
  if (!root) {
    GST_ERROR("Failed to parse JSON file %s: %s", 
              vitis_config_file.c_str(), error.text);
    return std::nullopt;
  }

  target_value = json_object_get(root, "target");
  if (!target_value || !json_is_string(target_value)) {
    GST_DEBUG("Key 'target' not found or not a string in config file: %s", 
              vitis_config_file.c_str());
    json_decref(root);
    return std::nullopt;
  }

  std::string target = json_string_value(target_value);
  json_decref(root);
  
  return target;
}
