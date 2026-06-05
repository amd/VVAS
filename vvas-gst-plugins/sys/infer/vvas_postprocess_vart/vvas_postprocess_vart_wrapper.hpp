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

/**
 * @file vvas_postprocess_vart_wrapper.hpp
 * @brief C++ wrapper class that bridges VVAS C API to VART C++ API
 *
 * This wrapper allows VVAS applications to use VART postprocessing
 * implementations (ResNet50, YOLOv2, SSDResNet34) through the standard
 * VVAS library interface.
 */

#pragma once

#include <memory>
#include <vector>
#include <string>
#include <vvas_core/vvas_postprocess.h>
#include <vvas_core/vvas_memory.h>
#include <vart/vart_postprocess.hpp>
#include <vart/vart_device.hpp>

/**
 * @class VvasPostProcessVartWrapper
 * @brief Wrapper class that manages VART PostProcess instances for VVAS
 *
 * This class handles:
 * - Creation of VART Device and PostProcess objects
 * - Conversion between VVAS and VART data structures
 * - Memory mapping and lifecycle management
 * - Result conversion from VART to VVAS format
 */
class VvasPostProcessVartWrapper {
public:
  /**
   * Constructor - Initialize VART postprocessing
   *
   * @param json_config JSON configuration string (must include "model-type" field)
   * @param t_info Array of VvasTensorInfo pointers
   * @param num_tensors Number of tensors in t_info array
   * @param batch_size Maximum batch size supported
   *
   * @throws std::runtime_error on initialization failure
   */
  VvasPostProcessVartWrapper(
    char* json_config,
    VvasTensorInfo** t_info,
    uint32_t num_tensors,
    uint32_t batch_size);

  /**
   * Destructor - Cleanup VART resources
   */
  ~VvasPostProcessVartWrapper();

  /**
   * Run postprocessing on tensor data
   *
   * @param tensor_memory Array of VvasMemory pointers (size = cur_batch_size * num_tensors)
   * @param cur_batch_size Current batch size (must be <= batch_size from constructor)
   * @param res Output array of VvasList pointers (size = cur_batch_size)
   *
   * @return VVAS_RET_SUCCESS on success, VVAS_RET_ERROR on failure
   */
  VvasReturnType run(VvasMemory** tensor_memory, uint32_t cur_batch_size, VvasList** res);

  /**
   * Get the configured batch size
   *
   * @return Maximum batch size
   */
  uint32_t get_batch_size() const { return batch_size_; }

private:
  /** VART objects */
  std::unique_ptr<vart::PostProcess> vart_postprocess_;
  std::shared_ptr<vart::Device> vart_device_;

  /** Configuration */
  uint32_t batch_size_;
  uint32_t num_tensors_;        /** Total tensors (input + output) */
  uint32_t num_output_tensors_; /** Only output tensors (for postprocessing) */
  std::string json_config_;

  /** Memory management */
  std::vector<VvasMemoryMapInfo> mapped_memory_;

  /** Helper methods */

  /**
   * Parse JSON config to determine PostProcessType
   *
   * @param json_config JSON configuration string
   * @return VART PostProcessType enum
   * @throws std::runtime_error if model-type is invalid
   */
  vart::PostProcessType parse_postprocess_type(const std::string& json_config);

  /**
   * Convert VVAS tensor info to VART format
   *
   * @param vvas_info Array of VvasTensorInfo pointers
   * @param num_tensors Number of tensors
   * @return Vector of VART TensorInfo structures
   */
  std::vector<vart::TensorInfo> convert_tensor_info(
    VvasTensorInfo** vvas_info,
    uint32_t num_tensors);

  /**
   * Convert VART results to VVAS format
   *
   * @param vart_results VART inference results (batch x results)
   * @param vvas_results Output VVAS result lists (array of VvasList*)
   * @param batch_size Number of batch items
   * @return VVAS_RET_SUCCESS on success
   */
  VvasReturnType convert_results(
    const std::vector<std::vector<std::shared_ptr<vart::InferResult>>>& vart_results,
    VvasList** vvas_results,
    uint32_t batch_size);

  /**
   * Convert single classification result
   */
  VvasInferResult* convert_classification_result(
    const vart::ClassificationResData* classification);

  /**
   * Convert single detection result
   */
  VvasInferResult* convert_detection_result(
    const vart::DetectionResData* detection);
};
