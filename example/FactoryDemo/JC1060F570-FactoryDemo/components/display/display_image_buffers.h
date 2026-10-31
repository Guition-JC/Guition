#pragma once

/**
 * @brief Install the 128-byte-aligned image allocator after the final esp_lv_decoder_init() and before decoding.
 */
void display_image_buffers_init(void);
