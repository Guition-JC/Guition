/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef DESKTOP_BIG_BUFFER_H
#define DESKTOP_BIG_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Allocate size bytes in PSRAM, retrying after cache release; return NULL on failure.
 * @param size Allocation size in bytes.
 * @return PSRAM buffer, or NULL if allocation fails or size is zero.
 */
void *desktop_big_buffer_alloc(size_t size);

/**
 * @brief Check for a contiguous PSRAM block of size bytes, releasing the image cache if needed.
 * @param size Required contiguous block size in bytes.
 * @return true if a block of the requested size is available after any cache release.
 */
bool desktop_big_buffer_reserve(size_t size);

#endif
