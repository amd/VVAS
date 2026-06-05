/*
 *
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

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include <vvas_core/vvas_common.h>
#include <vvas_core/vvas_log.h>
#include <vvas_core/vvas_postprocess.h>
#include <vvas_utils/vvas_utils.h>
#include <boost/lexical_cast.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <iostream>
#include <sstream>
#include <atomic>
#include <cinttypes>
#include <vector>
#include <algorithm>
#include <cmath>
#include <stdfloat>
#include <optional>
#include <limits>
#include <type_traits>
#include <cstring>
#include <fstream>
#include <cctype>

// Global logger/frame state for this postprocess module.
// NOTE: profiling is optional, but frame logging uses the frame counter regardless.
static VvasLogLevel g_yolo_pp_log_level = DEFAULT_VVAS_LOG_LEVEL;
static VvasLogger* g_yolo_pp_logger_handle = nullptr;
static std::atomic<uint64_t> g_yolo_pp_frame_counter {0};
/*
* WARNING this profiling code if used along with START_PROFILE
* and STOP_PROFILE macro's is only useful if there is a single
* instance of Yolo Post processing in the application, as the 
* profilers are stored in a static variable, do not expect sane
* results if enabled in a process with multiple instances of
* yolo processing.
* Enable only for development testing.
*/
//#define PROFILE

#ifdef PROFILE


#include <chrono>
#include <numeric>
class YoloProfiler {

    vector<std::chrono::high_resolution_clock::time_point> m_start_times;
    vector<std::chrono::high_resolution_clock::time_point> m_stop_times;
    vector<long> m_durations;
    string m_log;
    bool m_detailed;
public:
    YoloProfiler(const std::string &log, bool detailed=true): m_log{log}, m_detailed{detailed} {}
    YoloProfiler(YoloProfiler&& p) = default;
    YoloProfiler(const YoloProfiler& p) = default;

    void Start(){
        m_start_times.push_back(std::chrono::high_resolution_clock::now());
    }
    void Stop(){
        m_stop_times.push_back(std::chrono::high_resolution_clock::now());
        if(m_detailed){
            auto us = std::chrono::duration_cast<std::chrono::microseconds>
                    (m_stop_times.back() - m_start_times.back()).count();
            // Prefer *_OBJ logging so module-specific env var overrides (VVAS_CORE_DEBUG) are respected.
            if (g_yolo_pp_logger_handle) {
                vvas_logger_log_obj(LOG_LEVEL_NONE, g_yolo_pp_logger_handle, __FILENAME__,__func__, __LINE__, "%s took %ld us", m_log.c_str(), (long)us);
            } else {
                vvas_log(LOG_LEVEL_NONE, g_yolo_pp_log_level, __FILENAME__,__func__, __LINE__, "%s took %ld us", m_log.c_str(), (long)us);
            }
            m_durations.push_back(us);
        }
    }
    ~YoloProfiler(){
        if(!m_detailed){
            std::transform(m_start_times.begin(), m_start_times.end(),
            m_stop_times.begin(), std::back_inserter(m_durations),
            []( const std::chrono::high_resolution_clock::time_point& start,
                const std::chrono::high_resolution_clock::time_point& stop)
                {
                  return std::chrono::duration_cast<std::chrono::microseconds>(stop - start).count();
                });
        }

        if (!m_durations.empty()) {
            auto avg = std::accumulate(m_durations.begin(), m_durations.end(), 0L) / (long)m_durations.size();
            if (g_yolo_pp_logger_handle) {
                vvas_logger_log_obj(LOG_LEVEL_NONE, g_yolo_pp_logger_handle, __FILENAME__,__func__, __LINE__, "%s on average took %ld us", m_log.c_str(), avg);
            } else {
                vvas_log(LOG_LEVEL_NONE, g_yolo_pp_log_level, __FILENAME__,__func__, __LINE__, "%s on average took %ld us", m_log.c_str(), avg);
            }
        }

    }
};

    static std::map<string, YoloProfiler> profilers;
#define START_PROFILE(name, log) do {                                      \
    const std::string _key = (name);                                       \
    auto it = profilers.find(_key);                                        \
    if (it == profilers.end()) {                                           \
        it = profilers.emplace(_key, YoloProfiler{(log), false}).first;    \
    }                                                                      \
    it->second.Start();                                                    \
} while(0)

#define STOP_PROFILE(name) do {                                            \
    const std::string _key = (name);                                       \
    auto it = profilers.find(_key);                                        \
    if (it != profilers.end()) {                                           \
        it->second.Stop();                                                 \
    }                                                                      \
} while(0)
#else
    #define START_PROFILE(name, log) ;
    #define STOP_PROFILE(name) ;
#endif

using std::unique_ptr;
using std::vector;
using std::string;

constexpr float default_conf_thresh = 0.25f;
constexpr float default_iou_thresh = 0.45f;
constexpr int32_t default_max_detections = 300;
constexpr int32_t image_input_width = 640;
constexpr int32_t image_input_height = 640;


// Built-in fallback labels (COCO). If `class_label_file` is provided in JSON, we’ll load labels
// from that file (one class per line) instead of using this list.
const std::vector<std::string> coco_names = {
    "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat",
    "traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat","dog",
    "horse","sheep","cow","elephant","bear","zebra","giraffe","backpack","umbrella",
    "handbag","tie","suitcase","frisbee","skis","snowboard","sports ball","kite",
    "baseball bat","baseball glove","skateboard","surfboard","tennis racket","bottle",
    "wine glass","cup","fork","knife","spoon","bowl","banana","apple","sandwich","orange",
    "broccoli","carrot","hot dog","pizza","donut","cake","chair","couch","potted plant",
    "bed","dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone",
    "microwave","oven","toaster","sink","refrigerator","book","clock","vase","scissors",
    "teddy bear","hair drier","toothbrush"
};

struct Point {
    float x, y;
    Point(): x{0}, y{0} {}
    Point(float x, float y): x{x}, y{y} {}
};
struct BBox {
    Point tl, br;
    BBox(): tl{0, 0}, br{0, 0} {}
    BBox(const Point& tl, const Point& br): tl{tl}, br{br} {}
    BBox(float tlx, float tly, float brx, float bry): tl{tlx, tly}, br{brx, bry} {}
    BBox(const Point& center, float width, float height): tl{center.x - width/2, center.y - height/2}, br{center.x + width/2, center.y + height/2} {}
    Point getTL() const { return tl; }
    Point getBR() const { return br; }
    float getWidth() const { return br.x - tl.x; }
    float getHeight() const { return br.y - tl.y; }
    BBox addXOffset(float xoffset) const {
        return BBox(tl.x + xoffset, tl.y, br.x + xoffset, br.y);
    }
};

struct Detection {
    BBox box;
    float conf;
    int32_t class_id;
    Detection(): box{0, 0, 0, 0}, conf{0}, class_id{0} {}
    Detection(const BBox& box, float conf, int32_t class_id): box{box}, conf{conf}, class_id{class_id} {}
};

struct GridAndStride {
    int32_t gridx;
    int32_t gridy;
    int32_t stride;
};

struct YoloPostProcessParams{
  float conf_thresh {default_conf_thresh};
  float iou_thresh {default_iou_thresh};
  bool apply_sigmoid {false};
  bool class_agnostic {false};
  bool multi_label {false};
  bool box_grid_decode {false};
  bool has_objectness_score {true};
  uint32_t num_preds {0};
  uint32_t row_stride {0};
  bool transposed {false}; // output layout: false=[preds,attrs], true=[attrs,preds]
  std::vector<int32_t> decode_strides {8, 16, 32};
  std::vector<GridAndStride> grid_strides;
  int32_t preds_per_location {1};
  int32_t input_width {image_input_width};
  int32_t input_height {image_input_height};
  int32_t max_detections {default_max_detections};
  bool dequantize {false};
  float dequantize_scale {1.0f};
  float dequantize_offset {0.0f};
  std::string preset;
};

static inline std::string trim_copy(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

static std::vector<std::string>
read_class_labels_file(const std::string& path, VvasLogger* logger_handle)
{
    std::vector<std::string> labels;
    if (path.empty()) return labels;

    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_WARNING_OBJ(logger_handle,
                        "Failed to open class_label_file='%s' (falling back to built-in COCO labels)",
                        path.c_str());
        return labels;
    }

    std::string line;
    while (std::getline(f, line)) {
        // Handle Windows CRLF.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trim_copy(std::move(line));
        if (line.empty()) continue;
        if (!line.empty() && line[0] == '#') continue;
        labels.emplace_back(std::move(line));
    }

    if (labels.empty()) {
        LOG_WARNING_OBJ(logger_handle,
                     "class_label_file='%s' contained no labels (falling back to built-in COCO labels)",
                     path.c_str());
    } else {
        LOG_INFO_OBJ(logger_handle, "Loaded %zu class labels from '%s'", labels.size(), path.c_str());
    }
    return labels;
}

static std::vector<GridAndStride>
generate_grids_and_strides(int32_t input_w, int32_t input_h,
    int32_t preds_per_loc, const std::vector<int32_t>& strides)
{
    std::vector<GridAndStride> grid_strides;
    for (int32_t s : strides) {
        int32_t num_grid_w = input_w / s;
        int32_t num_grid_h = input_h / s;
        for (int32_t gy = 0; gy < num_grid_h; ++gy) {
            for (int32_t gx = 0; gx < num_grid_w; ++gx) {
                for(int32_t pr = 0; pr < preds_per_loc; ++pr){
                    grid_strides.push_back(GridAndStride{gx, gy, s});
                }
            }
        }
    }
    return grid_strides;
}

template<typename T>
static inline T sigmoid(const T& x)
{
    // Avoid ambiguous overload resolution for float16/bfloat16 by doing math in float.
    const float xf = static_cast<float>(x);
    if (xf >= 0.0f) {
        const float z = std::exp(-xf);
        return static_cast<T>(1.0f / (1.0f + z));
    } else {
        const float z = std::exp(xf);
        return static_cast<T>(z / (1.0f + z));
    }
}

static inline float iou_xyxy(const BBox& a, const BBox& b) {
    const float xx1 = std::max(a.getTL().x, b.getTL().x);
    const float yy1 = std::max(a.getTL().y, b.getTL().y);
    const float xx2 = std::min(a.getBR().x, b.getBR().x);
    const float yy2 = std::min(a.getBR().y, b.getBR().y);
    const float w = std::max(0.0f, xx2 - xx1);
    const float h = std::max(0.0f, yy2 - yy1);
    const float inter = w * h;
    const float areaA = std::max(0.0f, a.getBR().x - a.getTL().x) * std::max(0.0f, a.getBR().y - a.getTL().y);
    const float areaB = std::max(0.0f, b.getBR().x - b.getTL().x) * std::max(0.0f, b.getBR().y - b.getTL().y);
    return inter / (areaA + areaB - inter + 1e-7f);
}

// Single-pass NMS (offset trick handles class separation)
static std::vector<Detection>
 nms(const std::vector<Detection>& dets,
            float iou_thres,
            int32_t max_det)
{
    std::vector<Detection> out;
    if (dets.empty()) return out;

    // Sort indices by confidence descending
    std::vector<int32_t> idx(dets.size());
    for (size_t i = 0; i < dets.size(); ++i)
        idx[i] = static_cast<int>(i);

    std::sort(idx.begin(), idx.end(), [&](int32_t a, int32_t b) {
        return dets[a].conf > dets[b].conf;
    });

    std::vector<uint8_t> removed(dets.size(), 0);
    for (size_t _i = 0; _i < idx.size(); ++_i) {
        int32_t i = idx[_i];
        if (removed[i]) continue;

        out.push_back(dets[i]);
        if ((int32_t)out.size() >= max_det) break;

        for (size_t _j = _i + 1; _j < idx.size(); ++_j) {
            int32_t j = idx[_j];
            if (removed[j]) continue;
            if (iou_xyxy(dets[i].box, dets[j].box) > iou_thres) {
                removed[j] = 1;
            }
        }
    }
    return out;
}

template<bool Decode, typename T>
BBox get_box(const T& cx, const T& cy, const T& w, const T& h, const GridAndStride& gs)
{
    if constexpr (Decode) {
        // Cast before exp() to avoid ambiguous overloads for float16/bfloat16.
        const float wf = std::exp(static_cast<float>(w)) * static_cast<float>(gs.stride);
        const float hf = std::exp(static_cast<float>(h)) * static_cast<float>(gs.stride);
        return BBox(Point(static_cast<float>(cx + gs.gridx * gs.stride),
                          static_cast<float>(cy + gs.gridy * gs.stride)),
                    wf, hf);
    } else {
        return BBox(Point(static_cast<float>(cx), static_cast<float>(cy)),
                    static_cast<float>(w), static_cast<float>(h));
    }
}

template <typename T>
static inline float to_f32(const T v) {
    if constexpr (std::is_same_v<T, int8_t>) {
        return static_cast<float>(static_cast<int>(v));
    } else {
        return static_cast<float>(v);
    }
}

template <typename T>
struct RowReader {
    const T* base {nullptr};
    int32_t pred_idx {0};
    int32_t num_preds {0};
    int32_t row_stride {0}; // num attrs
    float inv_scale {1.0f};   // 1 / dequantize_scale
    bool dequantize {false};

    template <bool Transposed>
    inline float raw(const int32_t idx) const {
        // Non-transposed: [pred, attr] => contiguous per prediction
        // Transposed:     [attr, pred] => contiguous per attribute
        float v;
        if constexpr (!Transposed) {
            v = to_f32(base[pred_idx * row_stride + idx]);
        } else {
            v = to_f32(base[idx * num_preds + pred_idx]);
        }
        if (dequantize) v *= inv_scale;
        return v;
    }

    template <bool ApplySigmoid, bool Transposed>
    inline float score(const int32_t idx) const {
        const float v = raw<Transposed>(idx);
        if constexpr (ApplySigmoid) {
            return sigmoid(v);
        } else {
            return v;
        }
    }
};

#ifdef __ARM_NEON
static inline float32x4_t bf16x4_to_f32x4(const uint16_t* ptr) {
    uint16x4_t raw = vld1_u16(ptr);
    uint32x4_t shifted = vshll_n_u16(raw, 16);
    return vreinterpretq_f32_u32(shifted);
}

struct NeonArgmaxResult { float max_val; int32_t max_idx; };

static inline NeonArgmaxResult
neon_argmax_bf16_contiguous(const std::bfloat16_t* data, int32_t count) {
    const uint16_t* ptr = reinterpret_cast<const uint16_t*>(data);
    const int32_t vec_count = count & ~3;

    float32x4_t vmax = vdupq_n_f32(-std::numeric_limits<float>::infinity());
    uint32x4_t vidx_max = vdupq_n_u32(0);
    uint32x4_t vidx_cur = {0, 1, 2, 3};
    const uint32x4_t vidx_inc = vdupq_n_u32(4);

    for (int32_t c = 0; c < vec_count; c += 4) {
        float32x4_t vals = bf16x4_to_f32x4(ptr + c);
        uint32x4_t cmp = vcgtq_f32(vals, vmax);
        vmax = vbslq_f32(cmp, vals, vmax);
        vidx_max = vbslq_u32(cmp, vidx_cur, vidx_max);
        vidx_cur = vaddq_u32(vidx_cur, vidx_inc);
    }

    float lanes[4];
    uint32_t idx_lanes[4];
    vst1q_f32(lanes, vmax);
    vst1q_u32(idx_lanes, vidx_max);

    float best_val = lanes[0];
    int32_t best_idx = static_cast<int32_t>(idx_lanes[0]);
    for (int k = 1; k < 4; ++k) {
        if (lanes[k] > best_val) {
            best_val = lanes[k];
            best_idx = static_cast<int32_t>(idx_lanes[k]);
        }
    }

    for (int32_t c = vec_count; c < count; ++c) {
        uint32_t raw32 = static_cast<uint32_t>(ptr[c]) << 16;
        float v;
        std::memcpy(&v, &raw32, sizeof(float));
        if (v > best_val) {
            best_val = v;
            best_idx = c;
        }
    }

    return {best_val, best_idx};
}

static inline NeonArgmaxResult
neon_argmax_f32_contiguous(const float* data, int32_t count) {
    const int32_t vec_count = count & ~3;

    float32x4_t vmax = vdupq_n_f32(-std::numeric_limits<float>::infinity());
    uint32x4_t vidx_max = vdupq_n_u32(0);
    uint32x4_t vidx_cur = {0, 1, 2, 3};
    const uint32x4_t vidx_inc = vdupq_n_u32(4);

    for (int32_t c = 0; c < vec_count; c += 4) {
        float32x4_t vals = vld1q_f32(data + c);
        uint32x4_t cmp = vcgtq_f32(vals, vmax);
        vmax = vbslq_f32(cmp, vals, vmax);
        vidx_max = vbslq_u32(cmp, vidx_cur, vidx_max);
        vidx_cur = vaddq_u32(vidx_cur, vidx_inc);
    }

    float lanes[4];
    uint32_t idx_lanes[4];
    vst1q_f32(lanes, vmax);
    vst1q_u32(idx_lanes, vidx_max);

    float best_val = lanes[0];
    int32_t best_idx = static_cast<int32_t>(idx_lanes[0]);
    for (int k = 1; k < 4; ++k) {
        if (lanes[k] > best_val) {
            best_val = lanes[k];
            best_idx = static_cast<int32_t>(idx_lanes[k]);
        }
    }

    for (int32_t c = vec_count; c < count; ++c) {
        if (data[c] > best_val) {
            best_val = data[c];
            best_idx = c;
        }
    }

    return {best_val, best_idx};
}
#endif

template <typename T,
          bool ApplySigmoid,
          bool MultiLabel,
          bool BoxGridDecode,
          bool HasObjectness,
          bool Transposed>
static std::vector<Detection>
yolo_postprocess_impl(const T* input_tensor,
                      int32_t num_preds,
                      const int32_t num_classes,
                      const int32_t row_stride,
                      YoloPostProcessParams& params)
{
    const float max_wh = 7680.0f; // must exceed max image dimension
    std::vector<Detection> candidates;
    candidates.reserve(std::min<int32_t>(num_preds, 1024));

    const float conf_thresh = static_cast<float>(params.conf_thresh);

    const bool deq = params.dequantize;
    const float inv_scale =
        (deq && params.dequantize_scale != 0.0f) ? (1.0f / params.dequantize_scale) : 1.0f;

    START_PROFILE("yolo_postprocess_prepcand_v2", "Post process :: candidate preparation (v2)");
    RowReader<T> rr;
    rr.base = input_tensor;
    rr.num_preds = num_preds;
    rr.row_stride = row_stride;
    rr.inv_scale = inv_scale;
    rr.dequantize = deq;

    for (int32_t i = 0; i < num_preds; ++i) {
        rr.pred_idx = i;

        float obj_conf = 1.0f;
        if constexpr (HasObjectness) {
            obj_conf = rr.template score<ApplySigmoid, Transposed>(4);
            // Optimization: if obj_conf itself is below threshold, no class can lift it above threshold.
            // (This assumes class scores are in [0, 1] when ApplySigmoid==true; matches typical YOLO heads.)
            if (obj_conf < conf_thresh) continue;
        }

        // Boxes: dequantize if needed, never sigmoid.
        const float cx = rr.template raw<Transposed>(0);
        const float cy = rr.template raw<Transposed>(1);
        const float w  = rr.template raw<Transposed>(2);
        const float h  = rr.template raw<Transposed>(3);

        BBox base_box;
        if constexpr (BoxGridDecode) {
            // Only touch grid_strides if decoding is enabled (avoids invalid access when disabled).
            base_box = get_box<true>(cx, cy, w, h, params.grid_strides[i]);
        } else {
            // Avoid indexing params.grid_strides when not decoding.
            const GridAndStride dummy{0, 0, 0};
            base_box = get_box<false>(cx, cy, w, h, dummy);
        }

        constexpr int32_t score_offset = HasObjectness ? 1 : 0;
        const int32_t cls_base = 4 + score_offset;

        if constexpr (MultiLabel) {
            for (int32_t c = 0; c < num_classes; ++c) {
                const float cls_score = rr.template score<ApplySigmoid, Transposed>(cls_base + c);
                const float conf = obj_conf * cls_score;
                if (conf < conf_thresh) continue;

                BBox box = base_box;
                candidates.emplace_back(box, conf, c);
            }
        } else {
            int32_t best_cls = -1;
            float best_raw_score = -std::numeric_limits<float>::infinity();

#ifdef __ARM_NEON
            constexpr bool use_neon = !Transposed &&
                (std::is_same_v<T, std::bfloat16_t> || std::is_same_v<T, float>);
            if constexpr (use_neon) {
                const T* cls_ptr = &input_tensor[i * row_stride + cls_base];
                NeonArgmaxResult ar;
                if constexpr (std::is_same_v<T, std::bfloat16_t>) {
                    ar = neon_argmax_bf16_contiguous(cls_ptr, num_classes);
                } else {
                    ar = neon_argmax_f32_contiguous(reinterpret_cast<const float*>(cls_ptr), num_classes);
                }
                if (deq) ar.max_val *= inv_scale;
                best_raw_score = ar.max_val;
                best_cls = ar.max_idx;
            } else
#endif
            {
                for (int32_t c = 0; c < num_classes; ++c) {
                    const float v = rr.template raw<Transposed>(cls_base + c);
                    if (v > best_raw_score) {
                        best_raw_score = v;
                        best_cls = c;
                    }
                }
            }

            if (best_cls >= 0) {
                float best_score;
                if constexpr (ApplySigmoid) {
                    best_score = sigmoid(best_raw_score);
                } else {
                    best_score = best_raw_score;
                }
                const float conf = obj_conf * best_score;
                if (conf >= conf_thresh) {
                    BBox box = base_box;
                    candidates.emplace_back(box, conf, best_cls);
                }
            }
        }
    }
    STOP_PROFILE("yolo_postprocess_prepcand_v2");

    if (!params.class_agnostic) {
        for (auto& d : candidates) {
            d.box = d.box.addXOffset(max_wh * d.class_id);
        }
    }

    START_PROFILE("yolo_postprocess_nms_v2", "Post process :: nms (v2)");
    auto results = nms(candidates, static_cast<float>(params.iou_thresh), params.max_detections);
    if (!params.class_agnostic) {
        for (auto& d : results) {
            d.box = d.box.addXOffset(-max_wh * d.class_id);
        }
    }
    STOP_PROFILE("yolo_postprocess_nms_v2");

    return results;
}

enum : uint8_t {
    kSigmoid  = 1u << 0,
    kMulti    = 1u << 1,
    kDecode   = 1u << 2,
    kObj      = 1u << 3,
    kTransposed = 1u << 4,
};

template <typename T, uint8_t Mask>
static std::vector<Detection>
yolo_postprocess_dispatch_mask(const T* input_tensor,
                               int32_t num_preds,
                               const int32_t num_classes,
                               const int32_t row_stride,
                               YoloPostProcessParams& params)
{
    constexpr bool ApplySigmoid  = (Mask & kSigmoid) != 0;
    constexpr bool MultiLabel    = (Mask & kMulti) != 0;
    constexpr bool BoxGridDecode = (Mask & kDecode) != 0;
    constexpr bool HasObjectness = (Mask & kObj) != 0;
    constexpr bool Transposed    = (Mask & kTransposed) != 0;

    return yolo_postprocess_impl<T, ApplySigmoid, MultiLabel, BoxGridDecode, HasObjectness, Transposed>(
        input_tensor, num_preds, num_classes, row_stride, params);
}

template <typename T>
static std::vector<Detection>
 yolo_postprocess_v2(const T* input_tensor,
                     int32_t num_preds,
                     const int32_t num_classes,
                     const int32_t row_stride,
                     YoloPostProcessParams& params)
{
    const uint8_t mask =
        (params.apply_sigmoid ? kSigmoid : 0) |
        (params.multi_label ? kMulti : 0) |
        (params.box_grid_decode ? kDecode : 0) |
        (params.has_objectness_score ? kObj : 0) | 
        (params.transposed ? kTransposed : 0);

    switch (mask) {
#define YPP_CASE(M)                                                                                 \
        case (uint8_t)(M):                                                                          \
            return yolo_postprocess_dispatch_mask<T, (uint8_t)(M)>(                                 \
                input_tensor, num_preds, num_classes, row_stride, params)
        YPP_CASE(0);  YPP_CASE(1);  YPP_CASE(2);  YPP_CASE(3);
        YPP_CASE(4);  YPP_CASE(5);  YPP_CASE(6);  YPP_CASE(7);
        YPP_CASE(8);  YPP_CASE(9);  YPP_CASE(10); YPP_CASE(11);
        YPP_CASE(12); YPP_CASE(13); YPP_CASE(14); YPP_CASE(15);
        YPP_CASE(16); YPP_CASE(17); YPP_CASE(18); YPP_CASE(19);
        YPP_CASE(20); YPP_CASE(21); YPP_CASE(22); YPP_CASE(23);
        YPP_CASE(24); YPP_CASE(25); YPP_CASE(26); YPP_CASE(27);
        YPP_CASE(28); YPP_CASE(29); YPP_CASE(30); YPP_CASE(31);
        default:
            // Should be unreachable (mask is 5 bits), but keep a safe fallback.
            return yolo_postprocess_dispatch_mask<T, 0>(
                input_tensor, num_preds, num_classes, row_stride, params);
#undef YPP_CASE
    }
}

static std::string
normalize_preset_name(std::string preset)
{
    preset = trim_copy(std::move(preset));
    std::transform(preset.begin(), preset.end(), preset.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return preset;
}

static bool
yolo_preset_matches(std::string preset, std::string family)
{
    preset = normalize_preset_name(std::move(preset));
    family = normalize_preset_name(std::move(family));
    return preset == ("yolo" + family) || preset == ("yolov" + family);
}

static void
apply_yolo_preset(const std::string& preset_name, YoloPostProcessParams& params)
{
    const std::string preset = normalize_preset_name(preset_name);
    params.preset = preset;

    if (yolo_preset_matches(preset, "5") || yolo_preset_matches(preset, "7")) {
        params.has_objectness_score = true;
        params.apply_sigmoid = false;
        params.box_grid_decode = false;
        params.preds_per_location = 3;
        return;
    }
    if (yolo_preset_matches(preset, "8") || yolo_preset_matches(preset, "9") ||
        yolo_preset_matches(preset, "11") || yolo_preset_matches(preset, "12")) {
        params.has_objectness_score = false;
        params.apply_sigmoid = false;
        params.box_grid_decode = false;
        params.preds_per_location = 1;
        return;
    }
    if (yolo_preset_matches(preset, "x")) {
        params.has_objectness_score = true;
        params.apply_sigmoid = false;
        params.box_grid_decode = true;
        params.preds_per_location = 1;
        params.decode_strides = {8, 16, 32};
        return;
    }

    throw std::runtime_error(
        "Unknown YOLO preset '" + preset + "'. Supported presets: yolo[v]5, yolo[v]7, yolo[v]8, yolo[v]9, yolo[v]11, yolo[v]12, yolo[v]x");
}

static std::vector<int32_t>
parse_grid_strides_json(const boost::property_tree::ptree& pt, VvasLogger* logger_handle)
{
    std::vector<int32_t> strides;
    const auto child = pt.get_child("grid_strides");
    for (const auto& item : child) {
        const int32_t stride = item.second.get_value<int32_t>();
        if (stride <= 0) {
            throw std::runtime_error("grid_strides values must be positive integers");
        }
        strides.push_back(stride);
    }
    if (strides.empty()) {
        throw std::runtime_error("grid_strides must contain at least one stride");
    }
    LOG_DEBUG_OBJ(logger_handle, "grid_strides count=%zu", strides.size());
    return strides;
}

static std::string
format_strides(const std::vector<int32_t>& strides)
{
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < strides.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << strides[i];
    }
    out << "]";
    return out.str();
}

static void
validate_postprocess_params(const YoloPostProcessParams& params)
{
    if (!std::isfinite(params.conf_thresh) ||
        params.conf_thresh < 0.0f || params.conf_thresh > 1.0f) {
        throw std::runtime_error("conf_thresh must be finite and in range [0.0, 1.0]");
    }
    if (!std::isfinite(params.iou_thresh) ||
        params.iou_thresh < 0.0f || params.iou_thresh > 1.0f) {
        throw std::runtime_error("iou_thresh must be finite and in range [0.0, 1.0]");
    }
    if (params.preds_per_location <= 0) {
        throw std::runtime_error("preds_per_location must be a positive integer");
    }
    if (params.max_detections <= 0) {
        throw std::runtime_error("max_detections must be a positive integer");
    }
    if (params.input_width <= 0 || params.input_height <= 0) {
        throw std::runtime_error("input_width and input_height must be positive integers");
    }
    if (params.box_grid_decode) {
        if (params.decode_strides.empty()) {
            throw std::runtime_error("box_grid_decode requires non-empty grid_strides");
        }
        for (const int32_t stride : params.decode_strides) {
            if (stride <= 0) {
                throw std::runtime_error("grid_strides values must be positive integers");
            }
            if (params.input_width < stride || params.input_height < stride) {
                throw std::runtime_error("grid_strides values must not exceed input_width/input_height");
            }
            if ((params.input_width % stride) != 0 || (params.input_height % stride) != 0) {
                throw std::runtime_error("input_width and input_height must be divisible by every grid_strides value");
            }
        }
    }
}

static vector<VvasTensorInfo>
read_tensor_info(uint32_t num_tensors, VvasTensorInfo** t_info, VvasLogger* logger_handle)
{
    vector<VvasTensorInfo> info;
    LOG_DEBUG_OBJ(logger_handle, "Num tensors = %u", num_tensors);
    for(uint32_t i=0; i < num_tensors; i++){
        VvasTensorInfo& tmp = info.emplace_back();

        tmp.size = t_info[i]->size;
        tmp.scale_coeff = t_info[i]->scale_coeff;
        tmp.valid_shapes = t_info[i]->valid_shapes;
        tmp.data_type = t_info[i]->data_type;
        tmp.name = t_info[i]->name ? strdup(t_info[i]->name) : nullptr;
        for(uint32_t j=0; j<tmp.valid_shapes; j++)
            tmp.shape[j] = t_info[i]->shape[j];
        std::ostringstream shapes;
        shapes << "[ ";
        for (int32_t d = 0; d < MAX_SHAPE_SIZE; ++d) {
            shapes << tmp.shape[d] << " ";
        }
        shapes << "]";
        LOG_DEBUG_OBJ(logger_handle,
                      "Tensor %u :: name=%s data_type=%d size=%u scale_coeff=%f valid_shapes=%u shape=%s",
                      i, tmp.name ? tmp.name : "(null)", (int)tmp.data_type, tmp.size, tmp.scale_coeff,
                      tmp.valid_shapes, shapes.str().c_str());
    }
    return info;
}

static YoloPostProcessParams
read_postprocess_config(char *json_conf,
                        VvasLogger* &logger_handle,
                        VvasLogLevel &log_level,
                        std::string& class_label_file)
{
    YoloPostProcessParams params;

    boost::property_tree::ptree pt;
    try {
        std::istringstream json_stream(json_conf);
        boost::property_tree::read_json(json_stream, pt);
    } catch (const std::exception& e) {
        // Logger is not set up yet, fall back to process-wide VVAS logging.
        LOG_ERROR(DEFAULT_VVAS_LOG_LEVEL, "Error reading JSON : %s", e.what());
        throw;
    }

    // Create logger handle early so we can route all logs through VVAS logging.
    int json_log_level = pt.get<int>("log_level", (int)DEFAULT_VVAS_LOG_LEVEL);
    if (json_log_level < (int)LOG_LEVEL_NONE) json_log_level = (int)LOG_LEVEL_NONE;
    if (json_log_level > (int)LOG_LEVEL_DEBUG) json_log_level = (int)LOG_LEVEL_DEBUG;
    log_level = (VvasLogLevel)json_log_level;

    char *module_name = nullptr;
    logger_handle = vvas_logger_register(CORE_POST_PROCESS, log_level, &module_name);
    if (!logger_handle) {
        LOG_ERROR(log_level, "Failed to register YOLO postprocess with VVAS logger");
        throw std::runtime_error("vvas_logger_register failed");
    }
    g_yolo_pp_log_level = vvas_logger_get_log_level(logger_handle);
    g_yolo_pp_logger_handle = logger_handle;

    params.conf_thresh = pt.get<float>("conf_thresh", default_conf_thresh);
    params.iou_thresh = pt.get<float>("iou_thresh", default_iou_thresh);
    params.class_agnostic = pt.get<bool>("class_agnostic", false);
    params.multi_label = pt.get<bool>("multi_label", false);
    params.max_detections= pt.get<int>("max_detections", default_max_detections);
    class_label_file = pt.get<std::string>("class_label_file", "");

    if (auto preset_opt = pt.get_optional<std::string>("preset")) {
        apply_yolo_preset(*preset_opt, params);
        LOG_INFO_OBJ(logger_handle, "Applied YOLO preset '%s'", params.preset.c_str());
    }

    if (auto v = pt.get_optional<bool>("box_grid_decode")) {
        params.box_grid_decode = *v;
    }
    if (auto v = pt.get_optional<int>("preds_per_location")) {
        params.preds_per_location = *v;
    }
    if (auto v = pt.get_optional<bool>("apply_sigmoid")) {
        params.apply_sigmoid = *v;
    }
    if (auto v = pt.get_optional<bool>("has_objectness_score")) {
        params.has_objectness_score = *v;
    }
    if (pt.get_child_optional("grid_strides")) {
        params.decode_strides = parse_grid_strides_json(pt, logger_handle);
    }
    if (auto v = pt.get_optional<int>("input_width")) {
        params.input_width = *v;
    }
    if (auto v = pt.get_optional<int>("input_height")) {
        params.input_height = *v;
    }

    validate_postprocess_params(params);

    LOG_DEBUG_OBJ(logger_handle, "conf_thresh=%f", params.conf_thresh);
    LOG_DEBUG_OBJ(logger_handle, "iou_thresh=%f", params.iou_thresh);
    LOG_DEBUG_OBJ(logger_handle, "class_agnostic=%d", (int)params.class_agnostic);
    LOG_DEBUG_OBJ(logger_handle, "multi_label=%d", (int)params.multi_label);
    LOG_DEBUG_OBJ(logger_handle, "box_grid_decode=%d", (int)params.box_grid_decode);
    LOG_DEBUG_OBJ(logger_handle, "preds_per_location=%d", (int)params.preds_per_location);
    LOG_DEBUG_OBJ(logger_handle, "apply_sigmoid=%d", (int)params.apply_sigmoid);
    LOG_DEBUG_OBJ(logger_handle, "has_objectness_score=%d", (int)params.has_objectness_score);
    LOG_DEBUG_OBJ(logger_handle, "max_detections=%d", (int)params.max_detections);
    LOG_DEBUG_OBJ(logger_handle, "input_width=%d", (int)params.input_width);
    LOG_DEBUG_OBJ(logger_handle, "input_height=%d", (int)params.input_height);
    LOG_DEBUG_OBJ(logger_handle, "log_level=%d", (int)log_level);
    if (!params.preset.empty()) {
        LOG_DEBUG_OBJ(logger_handle, "preset=%s", params.preset.c_str());
    }
    LOG_DEBUG_OBJ(logger_handle, "decode_strides=%s", format_strides(params.decode_strides).c_str());
    if (!class_label_file.empty()) {
        LOG_INFO_OBJ(logger_handle, "class_label_file=%s", class_label_file.c_str());
    }

    return params;
}
struct YoloPriv{
  uint32_t num_tensors;
  vector<VvasTensorInfo> info;
  YoloPostProcessParams params;
  std::string class_label_file;
  std::vector<std::string> class_labels; // one label per class_id
  VvasLogger* logger_handle {nullptr};
  VvasLogLevel log_level {DEFAULT_VVAS_LOG_LEVEL};
};

struct ParsedYoloOutputShape {
    uint32_t num_preds {0};
    uint32_t num_attrs {0};
    bool transposed {false}; // true if [attrs, preds], false if [preds, attrs]
    uint32_t preds_dim_index {0};
    uint32_t attrs_dim_index {0};
};

static std::optional<ParsedYoloOutputShape>
parse_yolo_output_shape(const VvasTensorInfo& info,
                        const uint32_t min_expected_attrs,
                        VvasLogger* logger_handle)
{
    // Supported shapes:
    // - [preds, attrs, 0, 0, 0]
    // - [1, preds, attrs, 0, 0]
    // - [attrs, preds, 0, 0, 0]   (transposed)
    // - [1, attrs, preds, 0, 0]   (transposed)
    struct Dim { uint32_t idx; uint32_t val; };
    std::vector<Dim> dims;
    dims.reserve(MAX_SHAPE_SIZE);
    for (uint32_t d = 0; d < MAX_SHAPE_SIZE; ++d) {
        const uint32_t v = info.shape[d];
        if (v > 1) dims.push_back({d, v}); // ignore 0 and 1 (batch)
    }

    if (dims.size() < 2) {
        std::ostringstream shapes;
        for (int32_t d = 0; d < MAX_SHAPE_SIZE; d++) shapes << info.shape[d] << " ";
        LOG_ERROR_OBJ(logger_handle, "Invalid/unsupported shape: %s", shapes.str().c_str());
        return std::nullopt;
    }

    ParsedYoloOutputShape out;
    // Pick the two largest dims as [preds, attrs] and validate attrs size.
    // This supports padding, e.g. attrs=88 while min_expected_attrs=85.
    std::sort(dims.begin(), dims.end(), [](const Dim& a, const Dim& b){ return a.val > b.val; });
    const Dim preds_dim = dims[0];
    const Dim attrs_dim = dims[1];
    if (attrs_dim.val < min_expected_attrs) {
        std::ostringstream shapes;
        for (int32_t d = 0; d < MAX_SHAPE_SIZE; d++) shapes << info.shape[d] << " ";
        LOG_ERROR_OBJ(logger_handle,
                      "Unsupported attrs dimension: expected_at_least=%u got=%u (shape: %s). "
                      "If your model has different class count, update num_classes/labels accordingly.",
                      min_expected_attrs, attrs_dim.val, shapes.str().c_str());
        return std::nullopt;
    }
    out.num_preds = preds_dim.val;
    out.num_attrs = attrs_dim.val;
    out.attrs_dim_index = attrs_dim.idx;
    out.preds_dim_index = preds_dim.idx;
    out.transposed = out.attrs_dim_index < out.preds_dim_index;
    return out;
}

#ifdef __cplusplus
extern "C" {
#endif

void* postprocess_init(char* json_conf,
        VvasTensorInfo** t_info,
        uint32_t num_valid_tensors)
{
    std::unique_ptr<YoloPriv> pp_private = std::make_unique<YoloPriv>();

    pp_private->num_tensors = num_valid_tensors;
    auto cleanup_logger = [&]() {
        if (pp_private->logger_handle) {
            vvas_logger_deregister(pp_private->logger_handle);
            pp_private->logger_handle = nullptr;
        }
    };
    try {
        pp_private->params = read_postprocess_config(json_conf,
                                                     pp_private->logger_handle,
                                                     pp_private->log_level,
                                                     pp_private->class_label_file);
    } catch (const std::exception&) {
        cleanup_logger();
        return nullptr;
    }

    pp_private->info = read_tensor_info(num_valid_tensors, t_info, pp_private->logger_handle);

    if (pp_private->info.empty()) {
        LOG_ERROR_OBJ(pp_private->logger_handle, "No output tensor info provided");
        cleanup_logger();
        return nullptr;
    }
    const VvasTensorInfo& output_tensor = pp_private->info[0];

    if (output_tensor.data_type == VVAS_TENSOR_DATA_TYPE_INT8) {
        pp_private->params.dequantize = true;
        pp_private->params.dequantize_scale = output_tensor.scale_coeff;
        pp_private->params.dequantize_offset = 0.0f;
    }

    // Load class labels (optional override). If missing/invalid, fall back to built-in COCO labels.
    pp_private->class_labels = coco_names;
    if (!pp_private->class_label_file.empty()) {
        auto labels = read_class_labels_file(pp_private->class_label_file, pp_private->logger_handle);
        if (!labels.empty()) {
            pp_private->class_labels = std::move(labels);
        }
    }

    // Determine output layout and dims
    const uint32_t expected_attrs =
        4u + (pp_private->params.has_objectness_score ? 1u : 0u) + (uint32_t)pp_private->class_labels.size();
    auto parsed = parse_yolo_output_shape(output_tensor, expected_attrs, pp_private->logger_handle);
    if (!parsed) {
        LOG_ERROR_OBJ(pp_private->logger_handle, "Failed to parse output tensor shape");
        cleanup_logger();
        return nullptr;
    }
    pp_private->params.num_preds = parsed->num_preds;
    pp_private->params.row_stride = parsed->num_attrs;
    pp_private->params.transposed = parsed->transposed;
    LOG_INFO_OBJ(pp_private->logger_handle,
                 "YOLO output layout: %s, num_preds=%u, num_attrs=%u (attrs_dim_index=%u preds_dim_index=%u)",
                 pp_private->params.transposed ? "[attrs,preds]" : "[preds,attrs]",
                 pp_private->params.num_preds,
                 pp_private->params.row_stride,
                 parsed->attrs_dim_index,
                 parsed->preds_dim_index);

    // Prepare grid/stride list
    if(pp_private->params.box_grid_decode){
        const std::vector<int32_t>& strides = pp_private->params.decode_strides;
        pp_private->params.grid_strides = generate_grids_and_strides(
            pp_private->params.input_width, pp_private->params.input_height,
            pp_private->params.preds_per_location, strides);

        if ((uint32_t)pp_private->params.grid_strides.size() != pp_private->params.num_preds) {
            LOG_ERROR_OBJ(pp_private->logger_handle,
                          "Mismatch: grid_strides.size()=%zu vs num_preds=%u "
                          "(input_width=%d input_height=%d preds_per_location=%d configured_strides=%s)",
                          pp_private->params.grid_strides.size(),
                          pp_private->params.num_preds,
                          pp_private->params.input_width,
                          pp_private->params.input_height,
                          pp_private->params.preds_per_location,
                          format_strides(strides).c_str());
            cleanup_logger();
            return nullptr;
        }
    }

    return pp_private.release();
}

VvasReturnType postprocess_run(void* pp_private,
                VvasMemory** tensor_memory,
                uint32_t cur_batch_size,
                VvasList** res)
{
    YoloPriv* pp_handle = static_cast<YoloPriv*>(pp_private);

    for (uint32_t i = 0; i < cur_batch_size; i++) {
        const uint64_t frame_id = g_yolo_pp_frame_counter.fetch_add(1, std::memory_order_relaxed);
        VvasMemoryMapInfo map_info = {};
        if (VVAS_RET_SUCCESS !=
            vvas_memory_map(tensor_memory[i], VVAS_DATA_MAP_READ, &map_info)) {
            LOG_ERROR_OBJ(pp_handle->logger_handle, "Failed to map tensor memory for read");
            return VVAS_RET_ERROR;
        }

        START_PROFILE("postprocess_run_inpcpy", "Post process :: input copy");

        STOP_PROFILE("postprocess_run_inpcpy");
        START_PROFILE("postprocess_run_pptotal", "Post process :: yolo_postprocess function total");

        std::vector<Detection> detections;
        const auto& class_labels = pp_handle->class_labels.empty() ? coco_names : pp_handle->class_labels;
        if (pp_handle->info[0].data_type == VVAS_TENSOR_DATA_TYPE_FLOAT32) {
            detections = yolo_postprocess_v2(reinterpret_cast<const float*>(map_info.data),
                pp_handle->params.num_preds, class_labels.size(),
                pp_handle->params.row_stride, pp_handle->params);
        } else if (pp_handle->info[0].data_type == VVAS_TENSOR_DATA_TYPE_INT8) {
            detections = yolo_postprocess_v2(reinterpret_cast<const int8_t*>(map_info.data),
                pp_handle->params.num_preds, class_labels.size(),
                pp_handle->params.row_stride, pp_handle->params);
        } else if (pp_handle->info[0].data_type == VVAS_TENSOR_DATA_TYPE_BF16) {
            detections = yolo_postprocess_v2(reinterpret_cast<const std::bfloat16_t*>(map_info.data),
                pp_handle->params.num_preds, class_labels.size(),
                pp_handle->params.row_stride, pp_handle->params);
        } else if (pp_handle->info[0].data_type == VVAS_TENSOR_DATA_TYPE_FP16) {
            detections = yolo_postprocess_v2(reinterpret_cast<const std::float16_t*>(map_info.data),
                pp_handle->params.num_preds, class_labels.size(),
                pp_handle->params.row_stride, pp_handle->params);
        } else {
            LOG_ERROR_OBJ(pp_handle->logger_handle, "Unknown VvasTensorDataType %d", (int)pp_handle->info[0].data_type);
            vvas_memory_unmap(tensor_memory[i], &map_info);
            return VVAS_RET_ERROR;
        }

        STOP_PROFILE("postprocess_run_pptotal");
        START_PROFILE("postprocess_run_prepout", "Post process :: Results copy");

        LOG_DEBUG_OBJ(pp_handle->logger_handle,
                      "Frame %" PRIu64 " (batch_idx=%u): selected_boxes=%zu BEGIN",
                      frame_id, i, detections.size());

        size_t det_idx = 0;
        for (const auto& d : detections) {
            if (((d.box.getBR().x - d.box.getTL().x) > 0) && (d.box.getBR().y - d.box.getTL().y > 0)) {
                LOG_DEBUG_OBJ(pp_handle->logger_handle,
                              "  [%zu] cls=%d (%s) conf=%f box=[%f,%f,%f,%f] w=%f h=%f",
                              det_idx,
                              d.class_id,
                              (d.class_id >= 0 && (size_t)d.class_id < class_labels.size())
                                  ? class_labels[d.class_id].c_str()
                                  : "(unknown)",
                              d.conf,
                              d.box.getTL().x, d.box.getTL().y, d.box.getBR().x, d.box.getBR().y,
                              (d.box.getBR().x - d.box.getTL().x),
                              (d.box.getBR().y - d.box.getTL().y));
                det_idx++;
                VvasInferResult* infer_result = vvas_infer_result_detection_create();
                VvasInferDetection* det = (VvasInferDetection*)infer_result->data;
                det->bbox.x = std::max(0, (int32_t)std::round(d.box.getTL().x));
                det->bbox.y = std::max(0, (int32_t)std::round(d.box.getTL().y));
                det->bbox.width = std::max(0, (int32_t)std::round(d.box.getBR().x - d.box.getTL().x));
                det->bbox.height = std::max(0, (int32_t)std::round(d.box.getBR().y - d.box.getTL().y));
                det->class_id = d.class_id;
                det->probability = d.conf;
                const char* lbl =
                    (d.class_id >= 0 && (size_t)d.class_id < class_labels.size())
                        ? class_labels[d.class_id].c_str()
                        : "unknown";
                det->label = strdup(lbl);
                infer_result->infer_result_type = VVAS_INFER_RESULT_DETECTION;
                res[i] = vvas_list_append(res[i], infer_result);
            }
        }
        LOG_DEBUG_OBJ(pp_handle->logger_handle,
                      "Frame %" PRIu64 " (batch_idx=%u): appended_boxes=%zu END",
                      frame_id, i, det_idx);

        STOP_PROFILE("postprocess_run_prepout");

        vvas_memory_unmap(tensor_memory[i], &map_info);
    }
    return VVAS_RET_SUCCESS;
}

VvasReturnType postprocess_deinit(void* pp_private)
{
    YoloPriv* pp_handle = static_cast<YoloPriv*>(pp_private);
    if (pp_handle) {
        for (auto &ti : pp_handle->info) {
            if (ti.name) {
                free(ti.name);
                ti.name = nullptr;
            }
        }
    }
    if (pp_handle && pp_handle->logger_handle) {
        VvasLogger* old = pp_handle->logger_handle;
        vvas_logger_deregister(pp_handle->logger_handle);
        pp_handle->logger_handle = nullptr;
        if (g_yolo_pp_logger_handle == old) {
            g_yolo_pp_logger_handle = nullptr;
        }
    }
    delete pp_handle;

#ifdef PROFILE
    profilers.clear();
#endif

    return VVAS_RET_SUCCESS;
}
#ifdef __cplusplus
}  //extern "C"
#endif
