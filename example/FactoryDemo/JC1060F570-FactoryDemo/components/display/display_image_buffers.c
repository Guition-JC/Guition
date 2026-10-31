#include <stdint.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "draw/lv_draw_buf_private.h"
#include "display_image_buffers.h"

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_DECODER
/** @brief Allocate image buffers at 128-byte alignment and free them with the matching heap API. */
#define IMAGE_BUFFER_ALIGNMENT 128U

static void *image_buffer_alloc(size_t size, lv_color_format_t color_format)
{
    (void)color_format;
    if (size == 0 || size > SIZE_MAX - (IMAGE_BUFFER_ALIGNMENT - 1U)) {
        return NULL;
    }
    size = (size + IMAGE_BUFFER_ALIGNMENT - 1U) & ~(size_t)(IMAGE_BUFFER_ALIGNMENT - 1U);
#if CONFIG_SPIRAM_ENC_EXEMPT
    const uint32_t caps = MALLOC_CAP_SPIRAM_NO_ENC | MALLOC_CAP_8BIT;
#elif CONFIG_SPIRAM
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    const uint32_t caps = MALLOC_CAP_DEFAULT;
#endif
    return heap_caps_aligned_calloc(IMAGE_BUFFER_ALIGNMENT, 1, size, caps);
}

static void *image_buffer_align(void *buffer, lv_color_format_t color_format)
{
    (void)color_format;
    return buffer;
}
#endif

void display_image_buffers_init(void)
{
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_DECODER
    lv_draw_buf_handlers_t *handlers = lv_draw_buf_get_image_handlers();
    handlers->buf_malloc_cb = image_buffer_alloc;
    handlers->align_pointer_cb = image_buffer_align;
    handlers->buf_free_cb = heap_caps_free;
#endif
}
