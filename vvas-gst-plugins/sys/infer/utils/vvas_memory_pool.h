/*
 *
 * Copyright (C) 2024 - 2025 Advanced Micro Devices, Inc.
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
 * DOC: VVAS Memory Pool APIs
 * This file contains structures and methods related VVAS memory.
 *
 */

#ifndef __VVAS_MEMORY_POOL_H__
#define __VVAS_MEMORY_POOL_H__

#include <vvas_core/vvas_memory.h>
#include <vvas_utils/vvas_utils.h>

#ifdef __cplusplus
extern "C"
{
#endif

  typedef void VvasMemoryPool;

/**
 *  typedef VvasMemoryDataFreeCB - Callback function to be called to free memory pointed by @data,
 *                                when VvasMemory handle is getting freed using @vvas_memory_free() API.
 *  @data: Address of the data pointer
 *  @user_data: User data pointer sent via @vvas_memory_alloc_from_data() API
 *  Return: None
 */
  typedef void (*VvasMemoryDataFreeCB) (void *data, void *user_data);

/**
 *  typedef VvasMemoryReleaseCallback - Callback function for memory release
 *  @mem: Memory handle that is released
 *  @user_data: User-specific data passed during pool creation
 *  Return: None
 */
  typedef void (*VvasMemoryReleaseCallback) (VvasMemory * mem, void *user_data);

/**
 * vvas_memory_pool_create () - Creates and initializes a memory pool with the specified number of VvasMemory blocks.
 * @ctx - Address of VvasContext handle created using @vvas_context_create()
 * @pool_size - The number of memory blocks to be allocated in the pool
 * @mem_type - Type of memory to be allocated
 * @mem_flags - Flags of type &enum VvasAllocationFlags
 * @mbank_idx - Index of the memory bank on which memory is allocated
 * @size - Size of each memory block to be allocated
 * @ret: Address to store return value. Upon case of error, @ret is useful in understanding the root cause
 * @release_cb - Callback function to be invoked when a memory block is released
 * @user_data - User-specific data to pass to the callback function
 *
 * Return:
 *  On success, returns a pointer to VvasMemoryPool,
 *  On failure, returns NULL.
 */
  VvasMemoryPool *vvas_memory_pool_create (VvasContext * ctx, size_t pool_size,
      VvasAllocationType mem_type, VvasAllocationFlags mem_flags,
      uint8_t mbank_idx, size_t size, VvasReturnType * ret,
      VvasMemoryReleaseCallback release_cb, void *user_data);

/**
 * vvas_memory_pool_acquire_memory () - Acquires a memory block from the memory pool.
 * @pool - Pointer to the VvasMemoryPool instance
 *
 * Return:
 * * On success, returns a VvasMemory handle.
 * * On failure, returns NULL if no memory block is available.
 */
  VvasMemory *vvas_memory_pool_acquire_memory (VvasMemoryPool * pool,
      VvasReturnType * ret);

/**
 * vvas_memory_pool_acquire_memory_with_timeout () - Acquires a memory block from the pool with a timeout.
 * @pool - Pointer to the VvasMemoryPool instance
 * @ret: Address to store return value. Upon case of error, @ret is useful in understanding the root cause
 *
 * Return:
 * * On success, returns a VvasMemory handle.\n
 * * On failure, returns NULL if no memory block is available within the timeout.
 */
  VvasMemory *vvas_memory_pool_acquire_memory_with_timeout (VvasMemoryPool *
      pool, uint64_t timeout_ms, VvasReturnType * ret);

/**
 * vvas_memory_pool_release_memory () - Releases a memory block back into the pool and invokes the release callback.
 * @pool - Pointer to the VvasMemoryPool instance
 * @mem - Memory block to be released back to the pool
 * @ret: Address to store return value. Upon case of error, @ret is useful in understanding the root cause
 *
 * Return: None
 */
  void vvas_memory_pool_release_memory (VvasMemoryPool * pool,
      VvasMemory * mem);

/**
 * vvas_memory_pool_free () - Frees all resources associated with the memory pool.
 * @pool - Pointer to the VvasMemoryPool instance
 *
 * Return: None
 */
  void vvas_memory_pool_free (VvasMemoryPool * pool);

#ifdef __cplusplus
}
#endif
#endif                          // __VVAS_MEMORY_POOL_H__
