/**
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_ft_touch_exit_cb_t)(void *user_data);

/**
 * @brief Build the dot canvas and exit button; parent must have no padding.
 * @param parent Parent LVGL object, with no padding.
 * @param width Canvas width in pixels.
 * @param height Canvas height in pixels.
 * @param exit_cb Callback invoked by the exit control.
 * @param user_data Context passed to exit_cb.
 * @return true on success; false for invalid dimensions, a null parent or allocation failure.
 */
bool app_ft_touch_view_build(lv_obj_t *parent, int32_t width, int32_t height,
                               app_ft_touch_exit_cb_t exit_cb, void *user_data);

/**
 * @brief Release the canvas buffer; LVGL children remain owned by parent.
 */
void app_ft_touch_view_cleanup(void);

#ifdef __cplusplus
}
#endif
