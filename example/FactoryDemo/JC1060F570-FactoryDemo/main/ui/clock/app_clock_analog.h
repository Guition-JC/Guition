/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Create the analog clock. All APIs require the LVGL lock, including destruction.
 * @param parent Parent LVGL object.
 * @param width Panel width in pixels.
 * @param height Panel height in pixels.
 * @param sleep Whether the clock is used in sleep mode.
 * @param toast Callback for displaying a message.
 */
void app_clock_analog_start(lv_obj_t *parent, int32_t width, int32_t height,
                             bool sleep, void (*toast)(const char *));
/**
 * @brief Delete analog widgets, stop loading and release owned image buffers; hold the LVGL lock.
 */
void app_clock_analog_destroy(void);
/**
 * @brief Request the previous or next face; hold the LVGL lock.
 * @param direction Negative selects the previous face; zero or positive selects the next.
 */
void app_clock_analog_step(int direction);
/**
 * @brief Update hand angles from local time; invalid time resets the hands; hold the LVGL lock.
 * @param valid Whether the supplied time is valid.
 * @param hour Local hour.
 * @param minute Local minute.
 * @param second Local second.
 */
void app_clock_analog_time(bool valid, uint8_t hour, uint8_t minute, uint8_t second);
/**
 * @brief Report whether the active analog face uses image assets; hold the LVGL lock.
 * @return true if the active face uses image assets.
 */
bool app_clock_analog_is_image(void);
/**
 * @brief Report whether the active analog face comes from SD storage; hold the LVGL lock.
 * @return true if the active face comes from SD storage.
 */
bool app_clock_analog_is_sd(void);
/**
 * @brief Report whether the analog help modal is open; hold the LVGL lock.
 * @return true if the help modal is open.
 */
bool app_clock_analog_has_modal(void);
/**
 * @brief Pause or resume background loading as the face is covered or revealed; hold the LVGL lock.
 * @param covered true when another view covers the face.
 */
void app_clock_analog_covered(bool covered);

/**
 * @brief Reset the help flag once per app session, independently of style changes.
 */
void app_clock_analog_new_session(void);
