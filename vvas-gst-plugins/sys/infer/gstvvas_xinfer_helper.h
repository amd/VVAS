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

#ifndef __GST_VVAS_XINFER_HELPER_H__
#define __GST_VVAS_XINFER_HELPER_H__

#include <string>
#include <cctype>
#include <vector>
#include <atomic>
#include <filesystem>
#include <optional>

#include <gst/gst.h>
#include <gst/vvas/gstvvasallocator.h>
#include <gst/vvas/gstvvasbufferpool.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/vvas/gstvvasutils.h>
#include <gst/vvas/gstinferencemeta.h>
#include <gst/vvas/gstvvascoreutils.h>

#include <vvas/vvas_kernel.h>
#include <vvas_core/vvas_log.h>
#include <vvas_core/vvas_context.h>
#include <vvas_core/vvas_common.h>
#include <vvas_core/vvas_image_process.h>
#include <vvas_core/vvas_postprocess.h>
#include <vvas_utils/vvas_preprocess_api.h>
#include <vvas/vvas_structure.h>

#include <onnxruntime/core/session/onnxruntime_cxx_api.h>

#include "vart/vart_npu_tensor.hpp"
#include "vart/vart_runner_factory.hpp"

#include "utils/vvas_tensor_pool.hpp"

#define MAX_ROI 40

#define SCALE_EPSILON 1e-6f

/** @def DEFAULT_MBANK_IDX
 *  @brief Default memory bank index for external DMA buffers (non-VVAS).
 *         XDNA device has a single memory bank at index 0.
 */
#define DEFAULT_MBANK_IDX 3

/**
 * @brief Validates if scale value is positive and usable
 * @param scale The scale value to validate
 * @return true if scale is valid and positive, false otherwise
 */
inline bool is_valid_positive_scale(float scale) {
  return !std::isnan(scale) && !std::isinf(scale) && (scale > SCALE_EPSILON);
}

struct _roi
{
  uint32_t y_cord;
  uint32_t x_cord;
  uint32_t height;
  uint32_t width;
};

struct vvas_ms_roi
{
  uint32_t nobj;
  struct _roi roi[MAX_ROI];
};

struct QuantizationData
{
  float scale_factor;
  float zero_point;
};

/* Inference profiling statistics */
/** @struct VvasProfilerStats
 *  @brief  Holds profiling statistics for different stages of inference
 */
/**
 * @struct VvasInferProfiler
 * @brief Structure to hold profiling statistics and configuration for inference pipeline.
 *
 * This structure maintains detailed profiling information for each stage of the inference pipeline,
 * including pre-processing, inference, and post-processing. It also tracks timing information,
 * frame counts, and configuration parameters to facilitate performance analysis and debugging.
 *
 * Members:
 * - pre_proc: Profiling statistics for the pre-processing stage.
 * - infer: Profiling statistics for the inference stage.
 * - post_proc: Profiling statistics for the post-processing stage.
 * - start_time_us: Timestamp marking the start of the pipeline (in microseconds).
 * - end_time_us: Timestamp marking the end of the pipeline (in microseconds).
 * - last_pre_proc: Profiling statistics for the last processed frames in an interval (pre-processing).
 * - last_infer: Profiling statistics for the last processed frames in an interval (inference).
 * - last_post_proc: Profiling statistics for the last processed frames in an interval (post-processing).
 * - total_frames: Total number of frames processed.
 * - snap_lock: Mutex to protect access to profiling data.
 * - log_interval: Interval for logging profiling statistics.
 * - timeout_id: Identifier for timeout events.
 * - enable_profiling: Flag to enable or disable profiling.
 * - backend_runtime: Name of the backend runtime used for inference.
 */
typedef struct {
  VvasProfilerStats pre_proc;
  VvasProfilerStats infer;
  VvasProfilerStats post_proc;
  gint64 start_time_us;
  gint64 end_time_us;
  VvasProfilerStats last_pre_proc;
  VvasProfilerStats last_infer;
  VvasProfilerStats last_post_proc;
  guint64 total_frames;
  GMutex   snap_lock;
  guint    log_interval;
  gchar   *log_file;
  guint    timeout_id;
  gboolean enabled;
  char     backend_runtime [32];
} VvasInferProfiler;

/** @enum VvasThreadState
 *  @brief  Contains different states of kernel threads
 */
typedef enum
{
  VVAS_THREAD_NOT_CREATED,
  VVAS_THREAD_RUNNING,
  VVAS_THREAD_EXITED,
} VvasThreadState;

/** @struct Vvas_XInfer
 *  @brief  Holds members specific to VVAS image process library
 */
struct VvasCoreModule
{
  /** Name of the library */
  gchar *name;
  /** Array of input frame */
  VvasVideoFrame *input[MAX_ROI];
  /** Array of output frame */
  VvasVideoFrame *output[MAX_ROI];
  /** Stipulate core library is ready for processing */
  gboolean init_done;
  /** Handle to core library */
  void *handle;
};

/** @struct _Vvas_XInferFrame
 *  @brief  Contains video frame information
 */
struct Vvas_XInferFrame
{
  /** gstreamer parent buffer */
  GstBuffer *parent_buf;
  /** parent node video info */
  GstVideoInfo *parent_vinfo;
  /** useful to avoid pushing duplicate parent buffers in inference_level > 1 */
  gboolean last_parent_buf;
  /** gstreamer child buffer */
  GstBuffer *child_buf;
  /** child node video info */
  GstVideoInfo *child_vinfo;
  /** internally-allocated XRT-BO-backed buffer used when upstream
   *  delivered SW memory but the VART input tensor type is HW. NULL
   *  in the common zero-copy path. Owned by this struct. */
  GstBuffer *internal_inbuf;
  /** vvas frame info */
  VvasVideoFrame *vvas_frame;
  /** Skip frame if previous parent node does not have metadata */
  gboolean skip_processing;
  /** Received events */
  GstEvent *event;
  /** vvas input frame roi data*/
  vvas_ms_roi input_roi;
  /** vvas output frame roi data*/
  vvas_ms_roi output_roi;
  /** Use input and output roi info */
  gboolean use_roi_data;
  /** Use to check if scale is required or not in inference_level > 1 */
  gboolean is_ppe_required[MAX_ROI];
  /* tensor data */
  vector <VvasMemory *> *tensors;
};

/** @struct InferJobContext
 *  @brief  Holds all state required to finalize a single inference submission.
 *
 *  One context represents one inference call (one batch / one infer_frames
 *  group). For asynchronous VART inference it is heap-allocated, captured by
 *  the execute_async callback, and deleted in the finalize function once
 *  results have been cleaned up and queued to the post-process thread. For
 *  synchronous inference (VART use_async=false or ONNX) it is used inline and
 *  deleted right after the finalize call.
 *
 *  @c group and @c batch are sized to hold exactly their populated frames (no
 *  nullptr padding), so their @c size() is authoritative.
 */
struct InferJobContext
{
  /** Full infer_frames group for this submission, including skip/event frames
   *  riding along. These are forwarded to post-process in order. */
  std::vector<Vvas_XInferFrame *> group;
  /** Inferred subset of @c group (the frames actually sent to the runner). */
  std::vector<Vvas_XInferFrame *> batch;
  /** Whether HW (zero-copy) input tensors were used. */
  bool hw_input;
  /** Whether HW (zero-copy) output tensors were used. */
  bool hw_output;
  /** Mapped input frames (kept alive until the callback; non-HW input only). */
  std::vector<VvasVideoFrameMapInfo> mapped_inputs;
  /** Mapped output tensor memories (kept alive until the callback; non-HW output only). */
  std::vector<std::vector<VvasMemoryMapInfo>> mapped_outputs;
  /** VART input tensors (kept alive until the callback). */
  std::optional<std::vector<std::vector<vart::NpuTensor>>> vart_in;
  /** VART output tensors (kept alive until the callback). */
  std::optional<std::vector<std::vector<vart::NpuTensor>>> vart_out;
  /** Profiler start timestamp (microseconds); 0 when profiling disabled. */
  guint64 t0_us;
};

struct PreProcessInfo
{
  /* Is pre processing enabled?*/
  bool enabled;
  /** pre-process device index */
  gint dev_idx;
  /** Handle to pre-process device with dev-idx */
  vvasDeviceHandle dev_handle;
  /** Handle to hold pre-process's VVAS Acceleration Library information */
  VvasCoreModule *core_handle;
  /** UUID of xclbin */
  uuid_t xclbinId;
  /** To protect ppe context */
  GMutex lock;
  /** condition represents PPE has input to process */
  GCond has_input;
  /** condition represents PPE need input to process */
  GCond need_input;
  /** Holds handle to PPE thread */
  GThread *thread;
  /** Location of the xclbin to programmed on device */
  gchar *xclbin_loc;
  /** PPE video frame information */
  Vvas_XInferFrame *frame;
  /** PPE need data to process */
  gboolean need_data;
  /** Holds PPE output buffer video info, which is input to infer */
  GstVideoInfo *out_vinfo;
  /** PPE output buffer pool */
  GstBufferPool *outpool;
  /** number of bbox/sub_buffer at inference level */
  guint nframes_in_level;
  /** State of PPE thread */
  atomic<VvasThreadState> thread_state;
  /** Flag to inform if the PPE in use sofware */
  gboolean use_software;
  /** VVAS Core Global context */
  VvasContext *vvas_ctx;
  /** PPE input memory bank */
  gint in_mem_bank;
  /** PPE output memory bank */
  gint out_mem_bank;
  /** PPE output buffer queue */
  GQueue *buf_queue;
  /** roi data for pre-processing */
  vvas_ms_roi roi;
  /** Vvas Image Process caps */
  VvasImageProcessCapabilities *caps;
  /** Vvas Interpolation mode set by User*/
  VvasImageProcessInterpolationMode user_interpolation_mode;
  /** Interpolation mode property set */
  gboolean is_interpolation_mode_set;
  /** Vvas Image Process library initialization config */
  VvasImageProcessInitConfig init_config;
  /** Vvas Image Process library put parameters */
  VvasImageProcessOutParam out_param;
  /** Vvas Image Process param */
  VvasImageProcessParam param;
  /** PP Initial buffer value*/
  gint init_value;
  /* Quantization parameters for pre-process input tensors */
  QuantizationData quant_data;
  /* gboolean to indicate if quantization data is set by user */
  gboolean is_quant_set;
};

enum class OnnxRuntimeEP
{
  CPU,
  VITIS_AI,
  UNKNOWN
};

struct ORTVitisAIConfig
{
  /* Flag to enable/disable AI Analyzer profiling */
  gboolean ai_analyzer_profiling;
  /* Flag to enable/disable AI Analyzer visualization */
  gboolean ai_analyzer_visualization;
  /* Path to Vitis-AI EP config file */
  std::string file_path;
  /* Path to Vitis-AI compiled model cache directory */
  std::string cache_dir;
  /* VAIML compiled model directory cache key (directory name)*/
  std::string cache_key;
};

struct OnnxRTInfo
{
  /*Is this data successfully read from inference json file*/
  bool valid {false};
  /* Path to ONNX Model */
  std::string model_path;
  /* memory layout of the input tensor */
  std::string input_tensor_layout;
  /* Optional ONNX output layout override; unset leaves memory_layout empty */
  std::string output_tensor_layout;
  /* Onnx Session context */
  std::unique_ptr<Ort::Session> session = nullptr;
  /* ONNX Runtime environment */
  std::unique_ptr<Ort::Env> env = nullptr;
  /*Which execution provide is used cpu or vitis_ai*/
  OnnxRuntimeEP ep;
  /*configuration for vitisai, used if onnx_ep is VITIS_AI */
  ORTVitisAIConfig vai_conf;
  /* Flag to enable/disable ONNX Runtime profiling */
  gboolean enable_profiling;
  /* Path to save ONNX Runtime profiling data */
  std::string profiling_file_path;
};

struct VartInfo
{
  /*Is this data successfully read from inference json file*/
  bool valid {false};

  /*Path to compiled vart model directory*/
  std::string model_path;
  /* Flag to enable/disable AI Analyzer profiling */
  bool ai_analyzer_profiling;

  std::shared_ptr<vart::Runner> runner {nullptr};

  /* Whether to use CPU or HW tensors for ML input/output */
  /* Using HW will enable zero copy flow in VART */
  vart::TensorType inp_tensor_type;
  vart::TensorType out_tensor_type;

  /* xclbin to create vvas_context which will be used to 
  * allocate ML output memory if out_tensor_type = HW 
  */
  std::string xclbin_loc;

  /* Which memory bank to allocate ML output memory from */
  int32_t mbank_idx;

  /* Whether to use HW in shared or exclusive mode */
  bool aie_columns_sharing {true};
  bool is_columns_sharing_option_provided {false};
  /* Start column for NPU overlay execution */
  uint32_t start_column;
  bool is_start_column_option_provided {false};

  /* Path to vitis_ai_config json file */
  std::string config_file_path;

  /* Use asynchronous inference (Runner::execute_async with callback).
   * When false, synchronous Runner::execute is used. Async-by-default:
   * this matches read_vart_config(), which defaults "use-async" to true
   * when the JSON key is absent. */
  bool use_async {true};
};

enum class MLRuntime
{
  AUTO,
  ONNXRT,
  VART,
  UNKNOWN
};

/* Contains information related to model configuration */
struct InferModelConf
{
  /* Width of model */
  uint32_t model_width;
  /* Height of model */
  uint32_t model_height;
  /* Batch size */
  uint32_t batch_size;
  /* number of input tensor required by model */
  size_t num_in_tensors;
  /* number of output tensor generated by model */
  size_t num_out_tensors;
  /* Input tensor info */
  VvasTensorInfo in_tensors[MAX_TENSORS];
  /* Output tensor info */
  VvasTensorInfo out_tensors[MAX_TENSORS];
  /* Vector of input tensors' names */
  std::vector<const char*> input_names;
  /* Vector of output tensors' names */
  std::vector<const char*> output_names;
  /* Vector of input tensors' shapes */
  std::vector<std::vector<int64_t>> input_shapes;
  /* Vector of output tensors' shapes */
  std::vector<std::vector<int64_t>> output_shapes;
  /* dynamic shape flag */
  gboolean dynamic_shape;
};

struct InferInfo
{
    /* Network/Model's format */
  VvasVideoFormat model_format;
  /* Input video format */
  VvasVideoFormat input_tensor_format;
  /* inference members */
  /** VvasContext for Infer */
  VvasContext *vvas_ctx;
  
  /*Configuration related to ML runtime (onnx or vart)*/
  MLRuntime runtime;
  OnnxRTInfo ort_info;
  VartInfo vart_info;

  /* Model configuration */
  InferModelConf model_config;
  /** Handle to infer's VVAS acceleration library */
  VvasCoreModule *core_handle;
  /** Cascade level of current instance */
  guint level;
  /** For secondary inference, input object's minimum width
   * required to perform inference. */
  guint input_obj_min_width;
  /** For secondary inference, input object's minimum height
   * required to perform inference. */
  guint input_obj_min_height;
  /** For secondary inference, input object's maximum width
   * required to perform inference. */
  guint input_obj_max_width;
  /** For secondary inference, input object's maximum height
   * required to perform inference. */
  guint input_obj_max_height;
  /** To protect infer context */
  GMutex lock;
  /** Condition represents infer_batch_queue has sufficient buffer to process */
  GCond cond;
  /** Condition represents infer_batch_queue is full */
  GCond batch_full;
  /** Holds handle to infer thread */
  GThread *thread;
  /** Required infer batch size */
  guint batch_size;
  /** Max size of infer queue which holds inputs */
  guint max_queue;
  /** Infer input frame queue */
  GQueue *batch_queue;
  /** Low latency mode, where we do not wait for full batch to get full */
  gboolean low_latency;
  /** Width supported by infer model */
  guint pref_width;
  /** Height supported by infer model */
  guint pref_height;
  /** Video format supported by model */
  GstVideoFormat pref_format;
  /** Queue to holds sub buffer from previous xinfer prediction */
  GQueue *sub_buffers;
  /** decides whether sub_buffers need to be attached with metadata or not */
  gboolean attach_ppebuf;
  /** State of Infer thread */
  atomic<VvasThreadState> thread_state;

  /* Async inference state (used only when vart_info.use_async is true).
   * async_in_flight counts submitted-but-not-yet-finalized async jobs.
   * It is NOT a submission cap (backpressure comes from the blocking
   * tensor pool); it is used only as a drain barrier so no-inference
   * groups (EOS/event/skip-only) and thread exit cannot overtake
   * in-flight async results. */
  std::atomic<guint> async_in_flight;
  GMutex async_lock;
  GCond async_cond;

  /* filtering members */
  GList *input_class_filters;
  int num_input_class_filters;

  /** Flag to decide attaching empty metadata structure when there is no infer */
  gboolean attach_empty_meta;
};

/* Contains all PostProcessing related variables */
struct PostProcessInfo {
  bool enabled;
  /** VvasContext for PostProcess */
  VvasContext * vvas_ctx;
  /** Post Process handle */
  VvasPostProcess *handle;
  /* String representation of post processing json configuration */
  char *json_string;
  /* Post Processing library path */
  char *library_path;
  /* Post Process Queue length*/
  gint queue_length;
  /** Holds handle to Post Process thread */
  GThread *thread;
  /** Post Process queue */
  GQueue *queue;
  /** Mutex variable to protect concurrent access to Post Process Queue */
  GMutex lock;
  /** Condition variable to wait and signal Post Processing thread */
  GCond cond;
  /* Post Process thread state */
  atomic<VvasThreadState> thread_state;
  /* Tensor pool to manage tensor memory */
  VvasTensorPool *tensor_pool;

  /* Dequantization parameters for postprocess output tensors.
   * Backward compatible with older configs which specify a single scale-factor/zero-point.
   * Newer configs may provide per-output-tensor lists.
   */
  std::vector<QuantizationData> dequant_data;
  /* gboolean to indicate if dequantization data is set by user */
  gboolean is_dequant_set;
};

/**
   @fn gboolean read_ppe_config (void* element, json_t *root, PreProcessInfo* pre_proc)
   @param [in] element - Handle to GstVvas_XInfer (used only for logging)
   @param [in] root - Reference to json config file
   @param [out] pre_proc - The PreProcessInfo structure to be populated
   @return TRUE when reads all mandatory parameter from json
           FALSE when not able to read mandatory parameter from json
  
   @brief This function reads ppe json file and populate pre-processing
          parameters
 */
gboolean
read_ppe_config (void* element, json_t* root, PreProcessInfo* pre_proc);

/**
   @fn gboolean read_infer_config (void* element, json_t *root, InferInfo* infer)
   @param [in] element - Handle to GstVvas_XInfer (used only for logging)
   @param [in] root - Reference to json config file
   @param [out] infer - The InferInfo structure to be populated
   @return TRUE when reads all mandatory parameter from json
           FALSE when not able to read mandatory parameter from json
  
   @brief This function reads json config file and populate infer private
          parameters
 */
gboolean
read_infer_config (void* element, json_t* root, InferInfo* infer);

/**
   @fn gboolean read_postprocess_config (void* self, json_t *root, PostProcessInfo* post_proc)
   @param [in] element - Handle to GstVvas_XInfer
   @param [in] root - Reference to json config file
   @param [out] post_proc - The PostProcessInfo structure to be populated
   @return TRUE when reads all mandatory parameter from json
           FALSE when not able to read mandatory parameter from json
  
   @brief This function reads json config file and populate post-processing
          parameters
  
 */
gboolean
read_postprocess_config (void* element, json_t* root, PostProcessInfo* post_proc);

/**
    @fn VvasVideoFormat get_tensor_format (VvasVideoFormat model_format,
                              std::string& layout, VvasTensorDataType data_type)
    @param [in] model_format - This indicates which format of data the model was trained with RGB or BGR.
    @param [in] layout - Layout of data in memory ('NCHW', 'NHWC', ...)
    @param [in] data_type - data_type for tensors.
    @return A VvasVideoFormat corresponding to the data that will be used by ML model
            VVAS_VIDE_FORMAT_UNKNOWN in case an invalid/unsupported combination of format, layour, type
            was given.

 */
VvasVideoFormat
get_tensor_format (VvasVideoFormat model_format, std::string& layout, VvasTensorDataType data_type);

/**
    @fn VvasVideoFormat get_tensor_format (VvasVideoFormat model_format,
                              std::string& layout, VvasTensorDataType data_type)
    @param [in] model_format - This indicates which format of data the model was trained with RGB or BGR.
    @param [in] layout - Layout of data in memory ('NCHW', 'NHWC', ...)
    @param [in] data_type - data_type for tensors.
    @return A VvasVideoFormat corresponding to the data that will be used by ML model
            VVAS_VIDE_FORMAT_UNKNOWN in case an invalid/unsupported combination of format, layour, type
            was given.

    @brief this is an overlaod of the above function with layout from vart.
 */
VvasVideoFormat
get_tensor_format (VvasVideoFormat model_format, vart::MemoryLayout layout, VvasTensorDataType data_type);

/**
 * @brief Infer a preprocess-compatible layout for a GENERIC input tensor.
 *
 * - 4D tensor with shape[1] in {3,4} -> NCHW
 * - 4D tensor with shape[3] in {3,4} -> NHWC
 *
 * @param tensor          Input tensor metadata from the VART runner.
 * @param inferred_layout Resolved layout on success.
 * @param inferred_width  Resolved width on success.
 * @param inferred_height Resolved height on success.
 * @param error_reason    Failure reason when false is returned.
 * @return TRUE when the shape can be mapped for preprocessing.
 */
gboolean
infer_generic_preprocess_layout (const vart::NpuTensorInfo& tensor,
    vart::MemoryLayout& inferred_layout,
    uint32_t& inferred_width,
    uint32_t& inferred_height,
    std::string& error_reason);


/**
 *  @fn std::vector<int64_t> get_fixed_shape (const std::vector<int64_t>& shapes, guint user_batch_size)
 *  @param [in] shapes - The original shape of the tensor
 *  @param [in] user_batch_size - The batch size provided by user via infer-config.
 *  @return A vector containing the fixed shape with the updated batch size
 */
std::vector<int64_t>
get_fixed_shape (const std::vector<int64_t>& shapes, guint user_batch_size);

/**
 *  @fn std::optional<std::string> get_runtime_target (const std::string& vitis_config_file)
 *  @param [in] vitis_config_file - The path to the Vitis config file
 *  @return An optional string containing the runtime target if found, std::nullopt otherwise
 */
std::optional<std::string>
get_runtime_target (const std::string& vitis_config_file);


/**
 * @fn void vvas_infer_profiler_init (VvasInferProfiler *p)
 * @param [in] p - Pointer to VvasInferProfiler structure to initialize
 * @return None
 *
 * @brief Initializes the inference profiler structure and sets up
 *        profiling statistics for pre-processing, inference, and post-processing stages.
 */
void vvas_infer_profiler_init (VvasInferProfiler *p);

/**
 * @fn void vvas_infer_profiler_dump_json (VvasInferProfiler *p, gchar *instance_name)
 * @param [in] p - Pointer to VvasInferProfiler structure containing profiling data
 * @param [in] instance_name - Name of the inference instance for identification
 * @return None
 *
 * @brief Dumps the collected profiling statistics to a JSON file for
 *        analysis and debugging purposes.
 */
void vvas_infer_profiler_dump_json (VvasInferProfiler *p, gchar *instance_name);

/**
 * @fn gboolean vvas_infer_profiler_tick_cb (gpointer user_data)
 * @param [in] user_data - User data pointer (typically VvasInferProfiler structure)
 * @return TRUE to continue periodic callbacks, FALSE to stop
 *
 * @brief Callback function triggered at regular intervals to capture
 *        profiling snapshots and log performance statistics.
 */
gboolean vvas_infer_profiler_tick_cb (gpointer user_data);

/**
 * @fn void vvas_infer_profiler_deinit (VvasInferProfiler *p)
 * @param [in] p - Pointer to VvasInferProfiler structure to deinitialize
 * @return None
 *
 * @brief Cleans up and releases resources associated with the inference
 *        profiler, including mutex locks and allocated memory.
 */
void vvas_infer_profiler_deinit (VvasInferProfiler *p);


#endif // __GST_VVAS_XINFER_HELPER_H__
