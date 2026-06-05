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

#include "vvas_tensor_pool.hpp"

/**
 * @brief Constructor for VvasTensorPool.
 *
 * This constructor initializes a VvasTensorPool object with the given context, pool size,
 * memory allocation type, memory allocation flags, and a vector of sizes. It creates memory
 * pools for each size specified in the sizes vector.
 *
 * @param ctx Pointer to the VvasContext.
 * @param pool_size Size of the memory pool.
 * @param mem_type Type of memory allocation.
 * @param mem_flags Flags for memory allocation.
 * @param sizes Vector containing sizes for which memory pools are to be created.
 *
 * @throws std::runtime_error if memory pool creation fails.
 */
VvasTensorPool::VvasTensorPool (VvasContext * ctx, size_t pool_size,
    VvasAllocationType mem_type, VvasAllocationFlags mem_flags, int mbank_idx,
    const std::vector < size_t >&sizes)
{
  for (const auto & size:sizes) {
    VvasReturnType ret;
    VvasMemoryPool *pool =
        vvas_memory_pool_create (ctx, pool_size, mem_type, mem_flags, mbank_idx, size,
        &ret,
        nullptr, nullptr);
    if (ret != VVAS_RET_SUCCESS) {
      throw std::runtime_error ("Failed to create memory pool.");
    }
    memory_pools.push_back (pool);
  }
}

/**
 * @brief Acquires a set of memories from the memory pools.
 *
 * This function locks the mutex to ensure thread safety, then iterates over
 * the memory pools to acquire memories. If acquiring a memory fails, it throws
 * a runtime error.
 *
 * @return A vector of pointers to the acquired VvasMemory objects.
 *
 * @throws std::runtime_error If acquiring memory from any pool fails.
 */
std::vector < VvasMemory * >VvasTensorPool::acquire_memories ()
{
  std::lock_guard < std::mutex > lock (mutex);
  std::vector < VvasMemory * >acquired_memories;
  for (auto & pool:memory_pools) {
    VvasReturnType ret;
    VvasMemory *
        mem = vvas_memory_pool_acquire_memory (pool, &ret);
    if (ret != VVAS_RET_SUCCESS) {
      throw std::runtime_error ("Failed to acquire memory from pool.");
    }
    acquired_memories.push_back (mem);
  }
  return acquired_memories;
}

/**
 * @brief Releases a collection of VvasMemory objects back to their respective memory pools.
 *
 * This function iterates over a vector of VvasMemory pointers and releases each memory
 * object back to its corresponding memory pool.
 *
 * @param memories A vector of pointers to VvasMemory objects that need to be released.
 */
void
VvasTensorPool::release_memories (const std::vector < VvasMemory * >&memories)
{
  for (size_t i = 0; i < memories.size (); ++i) {
    vvas_memory_pool_release_memory (memory_pools[i], memories[i]);
  }
}

/**
 * @brief Maps a list of VvasMemory objects to their corresponding VvasMemoryMapInfo structures.
 *
 * This function takes a vector of VvasMemory pointers and maps each memory object
 * to a VvasMemoryMapInfo structure using the specified mapping flags. If the mapping
 * operation fails for any memory object, a runtime exception is thrown.
 *
 * @param memories A vector of pointers to VvasMemory objects that need to be mapped.
 * @param flags The mapping flags to be used for the memory mapping operation.
 * @return A vector of VvasMemoryMapInfo structures corresponding to the mapped memories.
 * @throws std::runtime_error If any memory mapping operation fails.
 */
std::vector < VvasMemoryMapInfo > VvasTensorPool::map_memories (const std::vector <
    VvasMemory * >&memories, VvasDataMapFlags flags)
{
  std::vector < VvasMemoryMapInfo > mapInfoList;
  for (auto & mem:memories) {
    VvasMemoryMapInfo mapInfo;
    VvasReturnType ret = vvas_memory_map (mem, flags, &mapInfo);
    if (ret != VVAS_RET_SUCCESS) {
      throw std::runtime_error ("Failed to map memory.");
    }
    mapInfoList.push_back (mapInfo);
  }
  return mapInfoList;
}

/**
 * @brief Unmaps a list of VvasMemory objects.
 *
 * This function unmaps the memories provided in the `memories` vector using
 * the corresponding mapping information in the `mapInfoList` vector.
 *
 * @param memories A vector of pointers to VvasMemory objects to be unmapped.
 * @param mapInfoList A vector of VvasMemoryMapInfo structures containing the
 *        mapping information for each memory object.
 *
 * @throws std::runtime_error if the sizes of `memories` and `mapInfoList` do not match.
 */
void
VvasTensorPool::unmap_memories (const std::vector < VvasMemory * >&memories,
    std::vector < VvasMemoryMapInfo > mapInfoList)
{
  if (memories.size () != mapInfoList.size ()) {
    throw
        std::runtime_error ("Mismatch between memories and mapInfoList sizes.");
  }

  for (size_t i = 0; i < memories.size (); ++i) {
    vvas_memory_unmap (memories[i], &mapInfoList[i]);
  }
}

/**
 * @brief Destructor for the VvasTensorPool class.
 *
 * This destructor iterates through the memory pools and frees each one.
 */
VvasTensorPool::~VvasTensorPool ()
{
  for (auto & pool:memory_pools) {
    vvas_memory_pool_free (pool);
  }
}
