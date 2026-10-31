#pragma once

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Apply a plain background and corner radius in pixels.
 * @param obj Object to style.
 * @param color Background color.
 * @param radius Corner radius in pixels.
 */
void i2ctools_view_plain(lv_obj_t *obj, lv_color_t color, int radius);
/**
 * @brief Create a positioned label owned by parent.
 * @param parent Parent object.
 * @param text Label text.
 * @param color Text color.
 * @param x Horizontal position.
 * @param y Vertical position.
 * @return Created label object.
 */
lv_obj_t *i2ctools_view_label(lv_obj_t *parent, const char *text,
                              lv_color_t color, int x, int y);
/**
 * @brief Create a flex-sized button and register its event callback.
 * @param parent Parent object.
 * @param text Button caption.
 * @param width_percent Width as a percentage of the parent.
 * @param height_percent Height as a percentage of the parent.
 * @param callback Event callback.
 * @return Created button object.
 */
lv_obj_t *i2ctools_view_flex_button(lv_obj_t *parent, const char *text,
                                    int width_percent, int height_percent,
                                    lv_event_cb_t callback);
/**
 * @brief Create a flex-sized text field with placeholder text.
 * @param parent Parent object.
 * @param text Initial field text.
 * @param width_percent Width as a percentage of the parent.
 * @param height_percent Height as a percentage of the parent.
 * @param placeholder Placeholder text.
 * @return Created text field object.
 */
lv_obj_t *i2ctools_view_flex_text_field(lv_obj_t *parent, const char *text,
                                        int width_percent, int height_percent,
                                        const char *placeholder);

#ifdef __cplusplus
}
#endif
