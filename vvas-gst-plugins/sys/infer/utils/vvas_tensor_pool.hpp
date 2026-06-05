/*
 * Copyright (C) 2024 - 2025 Advanced Micro Devices, Inc.
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

#pragma once

#include <vector>
#include <mutex>
#include <stdexcept>
#include <string>
#include "vvas_memory_pool.h"

/**
 * @class VvasTensorPool
 * @brief Manages a pool of VvasMemory objects for efficient memory allocation and deallocation of memory for Tensors.
 *
 * This class provides methods to acquire, release, map, and unmap memory objects from a pool.
 *
 * @details
 * The VvasTensorPool class is designed to manage memory pools for VvasMemory objects. It allows
 * for efficient memory management by reusing memory objects from the pool, reducing the overhead
 * of frequent memory allocations and deallocations.
 *
 * @constructor
 * @param ctx Pointer to the VvasContext.
 * @param pool_size Size of the memory pool.
 * @param mem_type Type of memory allocation.
 * @param mem_flags Flags for memory allocation.
 * @param sizes Vector containing the sizes of memory objects to be managed by the pool.
 *
 * @method acquire_memories
 * @brief Acquires memory objects from all pools.
 * @return A vector of pointers to VvasMemory objects.
 *
 * @method release_memories
 * @brief Releases memory objects back to the pools.
 * @param memories A vector of pointers to VvasMemory objects to be released.
 *
 * @method map_memories
 * @brief Maps all memory objects for usage.
 * @param memories A vector of pointers to VvasMemory objects to be mapped.
 * @param flags Flags for data mapping.
 * @return A vector of VvasMemoryMapInfo objects containing mapping information.
 *
 * @method unmap_memories
 * @brief Unmaps memory objects after usage.
 * @param memories A vector of pointers to VvasMemory objects to be unmapped.
 * @param mapInfoList A vector of VvasMemoryMapInfo objects containing mapping information.
 *
 * @destructor
 * @brief Destroys the VvasTensorPool object and releases all resources.
 */
class VvasTensorPool
{
public:
  VvasTensorPool (VvasContext * ctx, size_t pool_size, VvasAllocationType mem_type,
      VvasAllocationFlags mem_flags, int mbank_idx, const std::vector < size_t >&sizes);

  /* Acquire memory from all pools */
  std::vector < VvasMemory * >acquire_memories ();

  /* Release memory back to the pools */
  void release_memories (const std::vector < VvasMemory * >&memories);

  /* Helper function to map all memory objects */
  std::vector < VvasMemoryMapInfo > map_memories (const std::vector <
      VvasMemory * >&memories, VvasDataMapFlags flags);

  /* Unmap memory after usage */
  void unmap_memories (const std::vector < VvasMemory * >&memories,
      std::vector < VvasMemoryMapInfo > mapInfoList);

  ~VvasTensorPool ();

private:
  std::vector < VvasMemoryPool * >memory_pools;
  std::mutex mutex;
};
