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
 * @file postprocess_vvas_vart.cpp
 * @brief VVAS C API exports for VART postprocessing wrapper
 *
 * This file implements the three required VVAS postprocessing functions:
 * - postprocess_init: Initialize postprocessing
 * - postprocess_run: Run postprocessing on tensor data
 * - postprocess_deinit: Cleanup resources
 *
 * These functions are dynamically loaded by VVAS applications via
 * libvvascore_postprocess_vart.so
 */

#include <vvas_core/vvas_postprocess.h>
#include <vvas_utils/vvas_utils.h>
#include "vvas_postprocess_vart_wrapper.hpp"
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <sstream>
#include <iostream>
#include <cstring>
#include <cstdlib>

static const bool vvas_vart_debug_enabled =
  (std::getenv("VVAS_VART_DEBUG") != nullptr);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * postprocess_init - Initialize VART-based postprocessing
 *
 * This function is called by VVAS to create a postprocessing instance.
 * It creates a VvasPostProcessVartWrapper that manages VART PostProcess objects.
 *
 * @param json_conf JSON configuration string with fields:
 *                  - "model-type": Required. One of "resnet50", "yolov2", "ssdresnet34"
 *                  - "batch-size": Optional. Default 1
 *                  - "device-index": Optional. Default 0
 *                  - Model-specific parameters (e.g., "topk", "label-file-path")
 *
 * @param t_info Array of tensor information structures
 * @param num_valid_tensors Number of tensors in t_info array
 *
 * @return Opaque handle to VvasPostProcessVartWrapper, or NULL on failure
 *
 */
void* postprocess_init(char* json_conf,
  VvasTensorInfo** t_info,
  uint32_t num_valid_tensors)
{
  if (!json_conf) {
    std::cerr << "[VVAS-VART Wrapper] Error: NULL JSON configuration" << std::endl;
    return nullptr;
  }

  if (!t_info) {
    std::cerr << "[VVAS-VART Wrapper] Error: NULL tensor info array" << std::endl;
    return nullptr;
  }

  if (num_valid_tensors == 0) {
    std::cerr << "[VVAS-VART Wrapper] Error: num_valid_tensors is 0" << std::endl;
    return nullptr;
  }

  if (vvas_vart_debug_enabled) {
    std::cout << "[VVAS-VART Wrapper] Initializing postprocessing..." << std::endl;
    std::cout << "[VVAS-VART Wrapper]   Num tensors: " << num_valid_tensors << std::endl;
    std::cout << "[VVAS-VART Wrapper]   JSON config: " << json_conf << std::endl;
  }

  try {
    /** Parse JSON to get batch size */
    uint32_t batch_size = 1;  /** Default batch size */

    /** Create wrapper (no context needed, Device uses -1 and empty string) */
    VvasPostProcessVartWrapper* wrapper = new VvasPostProcessVartWrapper(
      json_conf,
      t_info,
      num_valid_tensors,
      batch_size
    );

    if (!wrapper) {
      std::cerr << "[VVAS-VART Wrapper] Error: Failed to allocate wrapper" << std::endl;
      return nullptr;
    }

    if (vvas_vart_debug_enabled) {
      std::cout << "[VVAS-VART Wrapper] Initialization successful" << std::endl;
    }
    return static_cast<void*>(wrapper);

  } catch (const std::exception& e) {
    std::cerr << "[VVAS-VART Wrapper] Exception in postprocess_init: " << e.what() << std::endl;
    return nullptr;
  } catch (...) {
    std::cerr << "[VVAS-VART Wrapper] Unknown exception in postprocess_init" << std::endl;
    return nullptr;
  }
}

/**
 * postprocess_run - Run postprocessing on tensor data
 *
 * This function processes inference output tensors and generates
 * human-readable results (classifications, detections, etc.)
 *
 * @param pp_private Handle returned from postprocess_init
 * @param tensor_memory Array of VvasMemory pointers containing tensor data
 *                      Size = cur_batch_size * num_tensors_per_batch
 * @param cur_batch_size Current batch size (must be <= batch_size from init)
 * @param res Output array of VvasList pointers (size = cur_batch_size)
 *            Each VvasList contains VvasInferResult* items
 *
 * @return VVAS_RET_SUCCESS on success, VVAS_RET_ERROR on failure
 *
 * Note: Current implementation assumes 1 tensor per batch item.
 * For multi-tensor models, tensor_memory layout is:
 *   [batch0_tensor0, batch0_tensor1, ..., batch1_tensor0, batch1_tensor1, ...]
 */
VvasReturnType postprocess_run(void* pp_private,
  VvasMemory** tensor_memory,
  uint32_t cur_batch_size,
  VvasList** res)
{
  if (!pp_private) {
    std::cerr << "[VVAS-VART Wrapper] Error: NULL postprocess handle" << std::endl;
    return VVAS_RET_ERROR;
  }

  if (!tensor_memory) {
    std::cerr << "[VVAS-VART Wrapper] Error: NULL tensor memory array" << std::endl;
    return VVAS_RET_ERROR;
  }

  if (!res) {
    std::cerr << "[VVAS-VART Wrapper] Error: NULL result array" << std::endl;
    return VVAS_RET_ERROR;
  }

  if (cur_batch_size == 0) {
    std::cerr << "[VVAS-VART Wrapper] Error: cur_batch_size is 0" << std::endl;
    return VVAS_RET_ERROR;
  }

  try {
    VvasPostProcessVartWrapper* wrapper = static_cast<VvasPostProcessVartWrapper*>(pp_private);

    /** Run postprocessing */
    VvasReturnType ret = wrapper->run(tensor_memory, cur_batch_size, res);

    if (ret == VVAS_RET_SUCCESS) {
      /** Log result counts in debug mode */
      if (vvas_vart_debug_enabled) {
        for (uint32_t i = 0; i < cur_batch_size; i++) {
          uint32_t count = res[i] ? vvas_list_length(res[i]) : 0;
          if (count > 0) {
            std::cout << "[VVAS-VART Wrapper] Batch " << i << ": " << count
              << " result(s)" << std::endl;
          }
        }
      }
    }

    return ret;

  } catch (const std::exception& e) {
    std::cerr << "[VVAS-VART Wrapper] Exception in postprocess_run: " << e.what() << std::endl;
    return VVAS_RET_ERROR;
  } catch (...) {
    std::cerr << "[VVAS-VART Wrapper] Unknown exception in postprocess_run" << std::endl;
    return VVAS_RET_ERROR;
  }
}

/**
 * postprocess_deinit - Cleanup postprocessing resources
 *
 * This function destroys the VvasPostProcessVartWrapper and frees all
 * associated resources.
 *
 * @param pp_private Handle returned from postprocess_init
 *
 * @return VVAS_RET_SUCCESS on success, VVAS_RET_ERROR on failure
 */
VvasReturnType postprocess_deinit(void* pp_private)
{
  if (!pp_private) {
    std::cerr << "[VVAS-VART Wrapper] Warning: NULL handle in postprocess_deinit" << std::endl;
    return VVAS_RET_ERROR;
  }

  try {
    if (vvas_vart_debug_enabled) {
      std::cout << "[VVAS-VART Wrapper] Deinitializing postprocessing..." << std::endl;
    }

    VvasPostProcessVartWrapper* wrapper = static_cast<VvasPostProcessVartWrapper*>(pp_private);
    delete wrapper;

    if (vvas_vart_debug_enabled) {
      std::cout << "[VVAS-VART Wrapper] Deinitialization successful" << std::endl;
    }
    return VVAS_RET_SUCCESS;

  } catch (const std::exception& e) {
    std::cerr << "[VVAS-VART Wrapper] Exception in postprocess_deinit: " << e.what() << std::endl;
    return VVAS_RET_ERROR;
  } catch (...) {
    std::cerr << "[VVAS-VART Wrapper] Unknown exception in postprocess_deinit" << std::endl;
    return VVAS_RET_ERROR;
  }
}

#ifdef __cplusplus
}
#endif
