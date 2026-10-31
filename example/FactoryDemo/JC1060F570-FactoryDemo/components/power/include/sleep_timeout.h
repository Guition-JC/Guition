/** SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Cache the NVS sleep timeout; callers run on the LVGL task. */

/**
 * @brief Load the stored timeout into the runtime cache; defaults when nothing valid is stored.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t sleep_timeout_restore(void);

/**
 * @brief Effective timeout in seconds (0 = auto sleep disabled).
 * @return Effective timeout in seconds (0 = auto sleep disabled).
 */
uint32_t sleep_timeout_get(void);

/**
 * @brief Apply and persist a timeout. Unsupported values fall back to the default.
 * @param timeout_seconds Timeout in seconds.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t sleep_timeout_set(uint32_t timeout_seconds);

#ifdef __cplusplus
}
#endif
