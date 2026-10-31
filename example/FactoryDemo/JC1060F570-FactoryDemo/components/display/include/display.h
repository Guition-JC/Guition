#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

/** @brief Backlight range in application percentages. */
#define DISPLAY_BACKLIGHT_MIN_PERCENT 10U
#define DISPLAY_BACKLIGHT_MAX_PERCENT 100U
/** @brief Level applied when NVS holds no stored preference. */
#define DISPLAY_BACKLIGHT_DEFAULT_PERCENT 80U

/**
 * @brief Initialize the panel, LVGL adapter and touch input.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t display_init(void);

/**
 * @brief Borrow the touch device for the display lifetime; return NULL if unavailable.
 * @return Borrowed touch device, or NULL if unavailable.
 */
lv_indev_t *display_get_touch_indev(void);

/**
 * @brief Return the LCD dimensions loaded from esp_board_manager.
 * @param[out] width Receives the LCD width in pixels.
 * @param[out] height Receives the LCD height in pixels.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t display_get_size(uint16_t *width, uint16_t *height);

/**
 * @brief Restore backlight after display_init(); return ESP_ERR_NOT_SUPPORTED if no brightness device exists.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t display_backlight_init(void);

/**
 * @brief Apply a backlight level; the value is clamped to the application range.
 * @param percent Percentage value.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t display_backlight_set(uint8_t percent);

/**
 * @brief Blank the backlight without changing the stored brightness preference.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t display_backlight_off(void);

/**
 * @brief Write the applied backlight level to NVS; the level itself is unchanged.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t display_backlight_persist(void);

/**
 * @brief Return the applied backlight level in percent, or 0 before it is known.
 * @return The applied backlight level in percent, or 0 before it is known.
 */
uint8_t display_backlight_get(void);

/**
 * @brief Acquire the adapter lock before any LVGL mutation; unlock on the same path.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if the adapter lock was acquired.
 */
bool lvgl_display_lock(int timeout_ms);
/**
 * @brief Release the LVGL adapter lock acquired by the caller.
 */
void lvgl_display_unlock(void);
