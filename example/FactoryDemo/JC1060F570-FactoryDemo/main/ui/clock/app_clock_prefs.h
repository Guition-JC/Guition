/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Style values are persisted in NVS and must retain their numeric identities. */
typedef enum {
    APP_CLOCK_STYLE_ANALOG = 0,
    APP_CLOCK_STYLE_FLIP,
    APP_CLOCK_STYLE_SEVEN_SEGMENT,
    APP_CLOCK_STYLE_IMAGE,
    APP_CLOCK_STYLE_COUNT,
} app_clock_style_t;

/** @brief Default style before a preference is stored. */
#define APP_CLOCK_STYLE_DEFAULT APP_CLOCK_STYLE_SEVEN_SEGMENT

/**
 * @brief Check whether the style has a renderer.
 * @param style Style to check.
 * @return true if the style has a renderer.
 */
bool app_clock_style_is_valid(app_clock_style_t style);

/**
 * @brief Read the display style; unsupported stored values use the default.
 * @param[out] style Receives the display style on success.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no value is saved, or another error.
 */
esp_err_t app_clock_prefs_load_style(app_clock_style_t *style);

/**
 * @brief Persist the display style in the clock preference namespace.
 * @param style Supported display style to save.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an unsupported style, or an NVS error.
 */
esp_err_t app_clock_prefs_save_style(app_clock_style_t style);

/**
 * @brief Read whether the long-press hint has been shown.
 * @param[out] seen Receives false if the preference is absent.
 * @return ESP_OK on success or an error if the preference cannot be read.
 */
esp_err_t app_clock_prefs_hint_seen(bool *seen);

/**
 * @brief Persist completion of the one-time hint.
 * @return ESP_OK on success or an NVS error.
 */
esp_err_t app_clock_prefs_mark_hint_seen(void);

/**
 * @brief Read the stable analog face ID independently of the display style.
 * @param[out] id Buffer receiving the NUL-terminated face ID.
 * @param capacity Size of id in bytes.
 * @return ESP_OK on success or an argument or NVS error.
 */
esp_err_t app_clock_prefs_load_face(char *id, size_t capacity);
/**
 * @brief Persist the stable analog face ID independently of the display style.
 * @param id Nonempty NUL-terminated face ID.
 * @return ESP_OK on success or an argument or NVS error.
 */
esp_err_t app_clock_prefs_save_face(const char *id);

#ifdef __cplusplus
}
#endif
