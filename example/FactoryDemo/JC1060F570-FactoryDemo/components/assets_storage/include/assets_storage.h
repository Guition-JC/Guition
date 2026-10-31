#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mount the read-only application-assets SPIFFS partition at /assets.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t assets_storage_mount(void);

/**
 * @brief Register the LVGL image decoder after LVGL has been initialized.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t assets_storage_init_lvgl_decoder(void);

/**
 * @brief True only when /assets is mounted and PNG paths can be passed to LVGL.
 * @return True only when /assets is mounted and PNG paths can be passed to LVGL.
 */
bool assets_storage_icons_ready(void);

#ifdef __cplusplus
}
#endif
