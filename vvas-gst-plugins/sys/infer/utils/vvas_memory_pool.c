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

#include "vvas_memory_pool.h"

/**
 * @struct VvasMemoryPoolPriv
 * @brief A structure representing the memory pool.
 */
typedef struct
{
  VvasQueue *queue;
  VvasMutex mutex;
  VvasContext *ctx;
  size_t pool_size;
  size_t size;
  VvasAllocationType mem_type;
  VvasAllocationFlags mem_flags;
  uint8_t mbank_idx;
  VvasMemoryReleaseCallback release_cb;
  void *user_data;
} VvasMemoryPoolPriv;

/**
 * @fn VvasMemoryPool* vvas_memory_pool_create(VvasContext *vvas_ctx,
 *                                    size_t pool_size,
 *                                    VvasAllocationType mem_type,
 *                                    VvasAllocationFlags mem_flags,
 *                                    uint8_t mbank_idx,
 *                                    size_t size,
 *                                    VvasReturnType *ret,
 *                                    VvasMemoryReleaseCallback release_cb,
 *                                    void *user_data)
 * @param[in] vvas_ctx - Address of the VvasContext handle
 * @param[in] pool_size - The number of memory blocks to be allocated in the pool
 * @param[in] mem_type - Type of memory to be allocated
 * @param[in] mem_flags - Flags for memory allocation
 * @param[in] mbank_idx - Index of the memory bank on which memory is allocated
 * @param[in] size - Size of each memory block to be allocated
 * @param[out] ret Address to store return value. Upon case of error, \p ret is useful in understanding the root cause
 * @param[in] release_cb - Callback function to be invoked when a memory block is released
 * @param[in] user_data - User-specific data to pass to the callback function
 * @brief Creates and initializes a memory pool with the specified number of memory blocks.
 * @return On success, returns a pointer to VvasMemoryPool.\n
 *         On failure, returns NULL.
 */
VvasMemoryPool *
vvas_memory_pool_create (VvasContext *vvas_ctx, size_t pool_size,
    VvasAllocationType mem_type, VvasAllocationFlags mem_flags,
    uint8_t mbank_idx, size_t size, VvasReturnType *ret,
    VvasMemoryReleaseCallback release_cb, void *user_data)
{
  VvasMemoryPoolPriv *priv = NULL;
  /* check arguments validity */
  if (!vvas_ctx) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, DEFAULT_VVAS_LOG_LEVEL,
        "vvas_vvas_ctx is NULL");
    if (ret)
      *ret = VVAS_RET_INVALID_ARGS;
    return NULL;
  }

  if (!ALLOC_TYPE_IS_VALID (mem_type) || !pool_size || !size) {
    VVAS_LOG_ERROR_OBJ (vvas_ctx->logger_handle, "invalid arguments");
    if (ret)
      *ret = VVAS_RET_INVALID_ARGS;
    return NULL;
  }

  priv = (VvasMemoryPoolPriv *) calloc (1, sizeof (VvasMemoryPoolPriv));

  if (!priv) {
    if (ret)
      *ret = VVAS_RET_ALLOC_ERROR;
    return NULL;
  }

  priv->queue = vvas_queue_new (pool_size);
  priv->release_cb = release_cb;
  priv->user_data = user_data;

  vvas_mutex_init (&priv->mutex);

  for (size_t i = 0; i < pool_size; ++i) {
    VvasMemory *mem =
        vvas_memory_alloc (vvas_ctx, mem_type, mem_flags, mbank_idx, size, ret);
    if (!mem) {
      vvas_memory_pool_free (priv);
      return NULL;
    }
    vvas_queue_enqueue (priv->queue, mem);
  }

  if (ret)
    *ret = VVAS_RET_SUCCESS;

  priv->ctx = vvas_ctx;
  priv->pool_size = pool_size;
  priv->size = size;
  priv->mem_type = mem_type;
  priv->mem_flags = mem_flags;
  priv->mbank_idx = mbank_idx;
  return (VvasMemoryPool *) priv;
}

/**
 * @fn VvasMemory* vvas_memory_pool_acquire_memory(VvasMemoryPool *pool)
 * @param[in] pool - Pointer to the VvasMemoryPool instance
 * @param[out] ret - Return value indicating success or error
 * @brief Acquires a memory block from the memory pool.
 * @return On success, returns a VvasMemory handle.\n
 *         On failure, returns NULL if no memory block is available.
 */
VvasMemory *
vvas_memory_pool_acquire_memory (VvasMemoryPool *pool, VvasReturnType *ret)
{
  VvasMemoryPoolPriv *priv = (VvasMemoryPoolPriv *) pool;
  VvasMemory *mem = NULL;
  if (!priv) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, DEFAULT_VVAS_LOG_LEVEL,
        "invalid arguments");
    if (ret)
      *ret = VVAS_RET_INVALID_ARGS;
    return NULL;
  }
  vvas_mutex_lock (&priv->mutex);
  mem = (VvasMemory *) vvas_queue_dequeue (priv->queue);

  if (mem) {
    if (ret)
      *ret = VVAS_RET_SUCCESS;
    VVAS_LOG_DEBUG_OBJ (priv->ctx->logger_handle, "memory acquired %p", mem);
  } else {
    VVAS_LOG_ERROR_OBJ (priv->ctx->logger_handle,
        "failed to get memory from pool");
    if (ret)
      *ret = VVAS_RET_ERROR;
  }


  vvas_mutex_unlock (&priv->mutex);
  return mem;
}

/**
 * @fn VvasMemory* vvas_memory_pool_acquire_memory_with_timeout(VvasMemoryPool *pool, uint64_t timeout_ms)
 * @param[in] pool - Pointer to the VvasMemoryPool instance
 * @param[in] timeout_ms - Timeout in milliseconds to wait for memory acquisition
 * @param[out] ret - Return value indicating success or error
 * @brief Acquires a memory block from the pool with a timeout.
 * @return On success, returns a VvasMemory handle.\n
 *         On failure, returns NULL if no memory block is available within the timeout.
 */
VvasMemory *
vvas_memory_pool_acquire_memory_with_timeout (VvasMemoryPool *pool,
    uint64_t timeout_ms, VvasReturnType *ret)
{
  VvasMemoryPoolPriv *priv = (VvasMemoryPoolPriv *) pool;
  VvasMemory *mem = NULL;
  if (!priv) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, DEFAULT_VVAS_LOG_LEVEL,
        "invalid arguments");
    if (ret)
      *ret = VVAS_RET_INVALID_ARGS;
    return NULL;
  }

  vvas_mutex_lock (&priv->mutex);
  mem = (VvasMemory *) vvas_queue_dequeue_timeout (priv->queue, timeout_ms);
  if (mem) {
    VVAS_LOG_DEBUG_OBJ (priv->ctx->logger_handle, "memory acquired %p", mem);
    if (ret)
      *ret = VVAS_RET_SUCCESS;
  } else {
    VVAS_LOG_ERROR_OBJ (priv->ctx->logger_handle,
        "failed to get memory from pool");
    if (ret)
      *ret = VVAS_RET_ERROR;
  }
  vvas_mutex_unlock (&priv->mutex);

  return mem;
}

/**
 * @fn void vvas_memory_pool_release_memory(VvasMemoryPool *pool, VvasMemory *mem)
 * @param[in] pool - Pointer to the VvasMemoryPool instance
 * @param[in] mem - Memory block to be released back to the pool
 * @brief Releases a memory block back into the pool and invokes the release callback.
 * @return None
 */
void
vvas_memory_pool_release_memory (VvasMemoryPool *pool, VvasMemory *mem)
{
  VvasMemoryPoolPriv *priv = (VvasMemoryPoolPriv *) pool;
  if (!priv || !mem) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, DEFAULT_VVAS_LOG_LEVEL,
        "invalid arguments");
    return;
  }

  vvas_queue_enqueue (priv->queue, mem);

  if (priv->release_cb) {
    priv->release_cb (mem, priv->user_data);
  }

  VVAS_LOG_DEBUG_OBJ (priv->ctx->logger_handle, "memory released %p", mem);
}

/**
 * @fn void vvas_memory_pool_free(VvasMemoryPool *pool)
 * @param[in] pool - Pointer to the VvasMemoryPool instance
 * @brief Frees all resources associated with the memory pool.
 * @return None
 */
void
vvas_memory_pool_free (VvasMemoryPool *pool)
{
  VvasMemoryPoolPriv *priv = (VvasMemoryPoolPriv *) pool;
  if (!priv) {
    VVAS_LOG_MESSAGE (VVAS_LOG_LEVEL_ERROR, DEFAULT_VVAS_LOG_LEVEL,
        "invalid arguments");
    return;
  }
  vvas_mutex_lock (&priv->mutex);

  while (!vvas_queue_is_empty (priv->queue)) {
    VvasMemory *mem = (VvasMemory *) vvas_queue_dequeue (priv->queue);
    vvas_memory_free (mem);
  }

  vvas_queue_free (priv->queue);
  vvas_mutex_unlock (&priv->mutex);
  vvas_mutex_clear (&priv->mutex);

  free (priv);
}
