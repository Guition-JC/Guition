#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

typedef void (*app_touch_view_close_cb_t)(void *user_data);

/**
 * @brief Build the drawing surface at the runtime display dimensions in pixels.
 * @param parent Parent object for the drawing surface.
 * @param screen_width Display width in pixels.
 * @param screen_height Display height in pixels.
 * @param close_cb Callback invoked to request closing the view.
 * @param user_data Context passed to the close callback.
 * @return true on successful creation, otherwise false.
 */
bool app_touch_view_build(lv_obj_t *parent, uint16_t screen_width, uint16_t screen_height,
                            app_touch_view_close_cb_t close_cb, void *user_data);

/**
 * @brief Release drawing resources; other LVGL children remain owned by parent.
 */
void app_touch_view_cleanup(void);

/**
 * @brief Report whether a drawing session is active.
 * @return true while a drawing session is active.
 */
bool app_touch_view_is_active(void);
