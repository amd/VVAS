/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
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

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <vart/vart_postprocess_types.hpp>
#include <vvas_utils/vvas_utils.h>
#include "vvas_postprocess_vart_wrapper.hpp"

/**
 * @brief Debug logging flag cached at startup from VVAS_VART_DEBUG.
 */
static const bool vvas_vart_wrapper_debug_enabled =
  (std::getenv("VVAS_VART_DEBUG") != nullptr);

/**
 * @brief Debug logging macro - can be controlled via environment variable
 * @param msg The message to log
 */
#define VVAS_VART_LOG_DEBUG(msg) \
  if (vvas_vart_wrapper_debug_enabled) { \
    std::cout << "[VVAS-VART Debug] " << msg << std::endl; \
  }

/**
 * @brief Constructor for VvasPostProcessVartWrapper
 * @param json_config The JSON configuration for the postprocess
 * @param t_info The tensor information
 * @param num_tensors The number of tensors
 * @param batch_size The batch size
 */
VvasPostProcessVartWrapper::VvasPostProcessVartWrapper(
  char* json_config,
  VvasTensorInfo** t_info,
  uint32_t num_tensors,
  uint32_t batch_size)
  : batch_size_(batch_size)
  , num_tensors_(num_tensors)
{
  if (!json_config || !t_info || num_tensors == 0 || batch_size == 0) {
    throw std::runtime_error("Invalid parameters to VvasPostProcessVartWrapper");
  }

  /** Store JSON config */
  json_config_ = std::string(json_config);

  /** Parse JSON to determine PostProcessType */
  vart::PostProcessType pp_type = parse_postprocess_type(json_config_);

  /** Create VART Device using get_device_hdl with ID -1 and empty string */
  try {
    vart_device_ = vart::Device::get_device_hdl(-1, "");
    if (!vart_device_) {
      throw std::runtime_error("get_device_hdl returned null");
    }
  } catch (const std::exception& e) {
    std::cerr << "Failed to create VART device: " << e.what() << std::endl;
    throw;
  }

  /** Create VART PostProcess instance */
  try {
    VVAS_VART_LOG_DEBUG("Creating VART PostProcess with type: "
      << static_cast<int>(pp_type));
    vart_postprocess_ = std::make_unique<vart::PostProcess>(
      pp_type,
      json_config_,
      vart_device_
    );
    if (!vart_postprocess_) {
      throw std::runtime_error("PostProcess creation returned null");
    }
    VVAS_VART_LOG_DEBUG("VART PostProcess created successfully");
  } catch (const std::exception& e) {
    std::cerr << "Failed to create VART postprocess: " << e.what() << std::endl;
    throw;
  }

  /** Convert tensor info and configure */
  std::vector<vart::TensorInfo> vart_tensor_info = convert_tensor_info(t_info, num_tensors);
  VVAS_VART_LOG_DEBUG("vart_tensor_info size: " << vart_tensor_info.size());

  /** Count output tensors (for postprocessing in run()) */
  num_output_tensors_ = 0;
  for (uint32_t i = 0; i < num_tensors; i++) {
    if (t_info[i] && t_info[i]->direction == VVAS_TENSOR_DATA_DIRECTION_OUTPUT) {
      num_output_tensors_++;
    }
  }

  if (num_output_tensors_ == 0) {
    throw std::runtime_error("No output tensors found in tensor info");
  }

  try {
    VVAS_VART_LOG_DEBUG("Setting VART PostProcess config with "
      << vart_tensor_info.size() << " tensors and batch_size=" << batch_size);
    vart_postprocess_->set_config(vart_tensor_info, batch_size);
    VVAS_VART_LOG_DEBUG("VART PostProcess config set successfully");
  } catch (const std::exception& e) {
    std::cerr << "Failed to set VART postprocess config: " << e.what() << std::endl;
    throw;
  }

  /** Reserve space for memory mapping (only output tensors) */
  mapped_memory_.reserve(batch_size * num_output_tensors_);

  VVAS_VART_LOG_DEBUG("VvasPostProcessVartWrapper initialized successfully"
    << " (batch_size=" << batch_size
    << ", total_tensors=" << num_tensors
    << ", output_tensors=" << num_output_tensors_ << ")");
}

VvasPostProcessVartWrapper::~VvasPostProcessVartWrapper() {
  /** RAII handles cleanup of vart_postprocess_ and vart_device_ */
}

vart::PostProcessType VvasPostProcessVartWrapper::parse_postprocess_type(
  const std::string& json_config)
{
  namespace pt = boost::property_tree;
  pt::ptree root;
  std::istringstream json_stream(json_config);

  try {
    pt::read_json(json_stream, root);
  } catch (const std::exception& e) {
    std::cerr << "Error parsing JSON config: " << e.what() << std::endl;
    throw std::runtime_error("Invalid JSON configuration");
  }

  /** Look for "type" field (required) */
  std::string pp_type;
  try {
    pp_type = root.get<std::string>("type");
  } catch (const std::exception& e) {
    throw std::runtime_error(
      "JSON config must include 'type' field (specifying postprocess type)");
  }

  /** Convert to lowercase for case-insensitive comparison */
  std::transform(pp_type.begin(), pp_type.end(), pp_type.begin(),
    [](unsigned char c) { return std::tolower(c); });

  if (pp_type == "resnet50") {
    return vart::PostProcessType::RESNET50;
  } else if (pp_type == "yolov2") {
    return vart::PostProcessType::YOLOV2;
  } else if (pp_type == "ssdresnet34") {
    return vart::PostProcessType::SSDRESNET34;
  } else if (pp_type == "softmax") {
    return vart::PostProcessType::SOFTMAX;
  } else if (pp_type == "argmax") {
    return vart::PostProcessType::ARGMAX;
  } else if (pp_type == "topk") {
    return vart::PostProcessType::TOPK;
  } else if (pp_type == "nms") {
    return vart::PostProcessType::NMS;
  } else if (pp_type == "threshold") {
    return vart::PostProcessType::THRESHOLD;
  } else if (pp_type == "label_mapping" || pp_type == "label-mapping") {
    return vart::PostProcessType::LABEL_MAPPING;
  } else if (pp_type == "normalization") {
    return vart::PostProcessType::NORMALIZATION;
  } else if (pp_type == "anchor_adjustment" || pp_type == "anchor-adjustment") {
    return vart::PostProcessType::ANCHOR_ADJUSTMENT;
  } else if (pp_type == "calibration_temperature" || pp_type == "calibration-temperature") {
    return vart::PostProcessType::CALIBRATION_TEMPERATURE;
  } else if (pp_type == "calibration_platt" || pp_type == "calibration-platt") {
    return vart::PostProcessType::CALIBRATION_PLATT;
  } else if (pp_type == "bias_correction" || pp_type == "bias-correction") {
    return vart::PostProcessType::BIAS_CORRECTION;
  } else if (pp_type == "outlier_detection" || pp_type == "outlier-detection") {
    return vart::PostProcessType::OUTLIER_DETECTION;
  } else if (pp_type == "uncertainty_estimation" || pp_type == "uncertainty-estimation") {
    return vart::PostProcessType::UNCERTAINTY_ESTIMATION;
  } else if (pp_type == "distance_iou_nms" || pp_type == "distance-iou-nms") {
    return vart::PostProcessType::DISTANCE_IOU_NMS;
  } else if (pp_type == "soft_nms" || pp_type == "soft-nms") {
    return vart::PostProcessType::SOFT_NMS;
  } else if (pp_type == "classwise_nms" || pp_type == "classwise-nms") {
    return vart::PostProcessType::CLASSWISE_NMS;
  } else if (pp_type == "object_count" || pp_type == "object-count") {
    return vart::PostProcessType::OBJECT_COUNT;
  } else {
    throw std::runtime_error("Unsupported type: " + pp_type +
      " (supported: resnet50, yolov2, ssdresnet34, softmax, argmax, topk, nms, threshold, "
      "label_mapping, normalization, anchor_adjustment, calibration_temperature, calibration_platt, "
      "bias_correction, outlier_detection, uncertainty_estimation, distance_iou_nms, soft_nms, "
      "classwise_nms, object_count)");
  }
}

std::vector<vart::TensorInfo> VvasPostProcessVartWrapper::convert_tensor_info(
  VvasTensorInfo** vvas_info,
  uint32_t num_tensors)
{
  std::vector<vart::TensorInfo> vart_tensor_vec;
  vart_tensor_vec.reserve(num_tensors);

  for (uint32_t i = 0; i < num_tensors; i++) {
    if (!vvas_info[i]) {
      throw std::runtime_error("NULL tensor info at index " + std::to_string(i));
    }

    vart::TensorInfo info;

    /** Direct mappings */
    info.size = vvas_info[i]->size;
    info.scale_coeff = vvas_info[i]->scale_coeff;

    /** Convert fixed array to vector */
    for (uint32_t j = 0; j < vvas_info[i]->valid_shapes; j++) {
      info.shape.push_back(vvas_info[i]->shape[j]);
    }

    /** Name conversion */
    info.name = vvas_info[i]->name ? std::string(vvas_info[i]->name) : "";

    /** Data type mapping */
    switch (vvas_info[i]->data_type) {
      case VVAS_TENSOR_DATA_TYPE_INT8:
        info.data_type = vart::TensorDataType::INT8;
        break;
      case VVAS_TENSOR_DATA_TYPE_BF16:
        info.data_type = vart::TensorDataType::BF16;
        break;
      case VVAS_TENSOR_DATA_TYPE_FLOAT32:
        info.data_type = vart::TensorDataType::FLOAT32;
        break;
      case VVAS_TENSOR_DATA_TYPE_FP16:
        info.data_type = vart::TensorDataType::FP16;
        break;
      default:
        info.data_type = vart::TensorDataType::UNKNOWN;
        std::cerr << "Warning: Unknown tensor data type for tensor " << i << std::endl;
        break;
    }

    if (vvas_info[i]->direction == VVAS_TENSOR_DATA_DIRECTION_OUTPUT) {
      info.direction = vart::TensorDataDirection::OUTPUT;
    } else if (vvas_info[i]->direction == VVAS_TENSOR_DATA_DIRECTION_INPUT) {
      info.direction = vart::TensorDataDirection::INPUT;
    } else {
      throw std::runtime_error("Unsupported tensor data direction for tensor " + std::to_string(i));
    }

    vart_tensor_vec.push_back(std::move(info));
  }

  return vart_tensor_vec;
}

VvasReturnType VvasPostProcessVartWrapper::run(
  VvasMemory** tensor_memory,
  uint32_t cur_batch_size,
  VvasList** res)
{ 
  std::vector<std::vector<std::shared_ptr<vart::InferResult>>> vart_results;
  if (!tensor_memory || !res) {
    std::cerr << "Invalid parameters to run()" << std::endl;
    return VVAS_RET_ERROR;
  }

  /** Validate vart_postprocess_ is not null */
  if (!vart_postprocess_) {
    std::cerr << "VART postprocess object is null" << std::endl;
    return VVAS_RET_ERROR;
  }

  /** Validate tensor_memory pointers */
  for (size_t i = 0; i < cur_batch_size * num_output_tensors_; i++) {
    if (!tensor_memory[i]) {
      std::cerr << "Null tensor_memory at index " << i << std::endl;
      return VVAS_RET_ERROR;
    }
  }

  try {
    /** Step 1: Map VvasMemory to get data pointers */
    mapped_memory_.clear();
    mapped_memory_.reserve(cur_batch_size * num_output_tensors_);

    for (size_t i = 0; i < cur_batch_size * num_output_tensors_; i++) {
      VvasMemoryMapInfo map_info = {};
      VvasReturnType map_ret = vvas_memory_map(
            tensor_memory[i], VVAS_DATA_MAP_READ, &map_info);

      if (VVAS_RET_SUCCESS != map_ret) {
        std::cerr << "Failed to map VvasMemory at index " << i << std::endl;

        // Cleanup already mapped memory
        for (size_t j = 0; j < i; j++) {
          vvas_memory_unmap(tensor_memory[j], &mapped_memory_[j]);
        }
        mapped_memory_.clear();
        return VVAS_RET_ERROR;
      }

      mapped_memory_.push_back(map_info);
    }

    /** Step 2: Process each batch item individually */
    std::vector<std::vector<std::shared_ptr<vart::InferResult>>> vart_results;
    vart_results.reserve(cur_batch_size);

    for (uint32_t batch_idx = 0; batch_idx < cur_batch_size; batch_idx++) {
      /** Prepare data pointers for this batch's output tensors */
      std::vector<int8_t*> data_ptrs;
      data_ptrs.reserve(num_output_tensors_);

      for (uint32_t out_idx = 0; out_idx < num_output_tensors_; out_idx++) {
        uint32_t index = (batch_idx * num_output_tensors_) + out_idx;
        uint8_t* data_ptr = reinterpret_cast<uint8_t*>(mapped_memory_[index].data);
        data_ptrs.push_back(reinterpret_cast<int8_t*>(data_ptr));
      }

      /** Step 3: Call VART postprocess for this batch (batch_size=1) */
      VVAS_VART_LOG_DEBUG("Processing batch " << (batch_idx + 1) << "/" << cur_batch_size
        << " with " << data_ptrs.size() << " output tensors");

      auto batch_result = vart_postprocess_->process(std::move(data_ptrs), 1);

      /** Collect the result for this batch */
      vart_results.push_back(batch_result[0]);
    }

    VVAS_VART_LOG_DEBUG("vart_postprocess_->process() completed for all " 
      << cur_batch_size << " batches");

    /** Step 4: Unmap VvasMemory */
    for (size_t i = 0; i < cur_batch_size * num_output_tensors_; i++) {
      vvas_memory_unmap(tensor_memory[i], &mapped_memory_[i]);
    }
    mapped_memory_.clear();

    /** Step 5: Convert results to VVAS format */
    return convert_results(vart_results, res, cur_batch_size);

  } catch (const std::exception& e) {
    std::cerr << "Exception in VART postprocess: " << e.what() << std::endl;

    /** Emergency cleanup - unmap any mapped memory */
    for (size_t i = 0; i < cur_batch_size * num_output_tensors_; i++) {
      vvas_memory_unmap(tensor_memory[i], &mapped_memory_[i]);
    }
    mapped_memory_.clear();

    return VVAS_RET_ERROR;
  }
}

VvasReturnType VvasPostProcessVartWrapper::convert_results(
  const std::vector<std::vector<std::shared_ptr<vart::InferResult>>>& vart_results,
  VvasList** vvas_results,
  uint32_t batch_size)
{
  for (uint32_t batch_idx = 0; batch_idx < batch_size; batch_idx++) {
    /** Initialize result list for this batch */
    vvas_results[batch_idx] = nullptr;

    if (batch_idx >= vart_results.size()) {
      /** No results for this batch item */
      continue;
    }

    const auto& frame_results = vart_results[batch_idx];

    for (const auto& infer_result : frame_results) {
      if (!infer_result) continue;

      vart::InferResultData* result_data = infer_result->get_infer_result();
      if (!result_data) continue;

      VvasInferResult* vvas_result = nullptr;

      switch (result_data->result_type) {
        case vart::InferResultType::CLASSIFICATION: {
          vart::ClassificationResData* classification =
            static_cast<vart::ClassificationResData*>(result_data);
          vvas_result = convert_classification_result(classification);
          break;
        }

        case vart::InferResultType::DETECTION: {
          vart::DetectionResData* detection =
            static_cast<vart::DetectionResData*>(result_data);
          vvas_result = convert_detection_result(detection);
          break;
        }

        case vart::InferResultType::ROOT:
          /** Skip root nodes (used for tree structure) */
          continue;

        default:
          std::cerr << "Warning: Unsupported VART result type: "
            << static_cast<int>(result_data->result_type) << std::endl;
          continue;
      }

      if (vvas_result) {
        vvas_results[batch_idx] = vvas_list_append(vvas_results[batch_idx], vvas_result);
      }
    }
  }

  return VVAS_RET_SUCCESS;
}

VvasInferResult* VvasPostProcessVartWrapper::convert_classification_result(
  const vart::ClassificationResData* classification)
{
  if (!classification) return nullptr;

  VvasInferResult* vvas_result = vvas_infer_result_classification_create();
  if (!vvas_result) {
    std::cerr << "Failed to create VVAS classification result" << std::endl;
    return nullptr;
  }

  VvasList** classification_list = (VvasList**)&vvas_result->data;

  /** Convert each classification entry */
  size_t num_classes = classification->label.size();
  for (size_t i = 0; i < num_classes; i++) {
    VvasInferClassification* c = vvas_inferclassification_new();
    if (!c) {
      std::cerr << "Failed to create VVAS classification entry at index " << i << std::endl;
      /** Continue with partial results - VVAS core will free them */
      continue;
    }

    c->id = (i < classification->index.size()) ? classification->index[i] : i;
    c->probability = (i < classification->confidence.size()) ?
      classification->confidence[i] : 0.0;
    c->label = strdup(classification->label[i].c_str());

    if (!c->label) {
      std::cerr << "Failed to allocate label string at index " << i << std::endl;
      vvas_inferclassification_free(c);
      continue;
    }

    *classification_list = vvas_list_append(*classification_list, c);
  }

  vvas_result->infer_result_type = VVAS_INFER_RESULT_CLASSIFICATION;
  return vvas_result;
}

VvasInferResult* VvasPostProcessVartWrapper::convert_detection_result(
  const vart::DetectionResData* detection)
{
  if (!detection) return nullptr;

  VvasInferResult* vvas_result = vvas_infer_result_detection_create();
  if (!vvas_result) {
    std::cerr << "Failed to create VVAS detection result" << std::endl;
    return nullptr;
  }

  VvasInferDetection* det = (VvasInferDetection*)vvas_result->data;
  if (!det) {
    std::cerr << "Invalid detection data pointer in VVAS result" << std::endl;
    /** Return partial result - VVAS core will manage it */
    return nullptr;
  }

  /** Convert bounding box and detection info */
  det->bbox.x = detection->x;
  det->bbox.y = detection->y;
  det->bbox.width = detection->width;
  det->bbox.height = detection->height;
  det->label = strdup(detection->label.c_str());

  if (!det->label) {
    std::cerr << "Failed to allocate label string for detection" << std::endl;
    /** Return partial result - VVAS core will manage it */
    return nullptr;
  }

  det->probability = detection->confidence;
  det->class_id = detection->class_id;

  vvas_result->infer_result_type = VVAS_INFER_RESULT_DETECTION;
  return vvas_result;
}
