/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "desktop_big_buffer.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"

#define TAG "desktop_mem"

/**
 * @brief Release the LVGL image cache before retrying a large PSRAM allocation.
 * @param size Requested allocation size in bytes, used for diagnostics.
 */
static void hand_the_image_cache_back(size_t size)
{
    ESP_LOGW(TAG, "no %u-byte PSRAM block (free=%u largest=%u); handing the image cache back",
             (unsigned)size, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    lv_image_cache_drop(NULL);
}

bool desktop_big_buffer_reserve(size_t size)
{
    if (size == 0) {
        return false;
    }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= size) {
        return true;
    }
    hand_the_image_cache_back(size);
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= size;
}

void *desktop_big_buffer_alloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer != NULL) {
        return buffer;
    }
    if (!desktop_big_buffer_reserve(size)) {
        return NULL;
    }
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
