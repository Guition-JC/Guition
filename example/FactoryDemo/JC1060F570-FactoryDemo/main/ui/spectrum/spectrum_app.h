#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the spectrum view and start its private GMF microphone pipeline.
 * @param parent Parent object for the spectrum view.
 * @return ESP_OK on success, otherwise a pipeline or initialization error.
 */
esp_err_t spectrum_app_start(lv_obj_t *parent);

/**
 * @brief Stop the private pipeline, release input ownership, and delete the view.
 */
void spectrum_app_stop(void);

#ifdef __cplusplus
}
#endif
