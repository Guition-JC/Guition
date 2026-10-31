#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Shared Wi-Fi page palette. */
#define APP_WIFI_C_CANVAS      lv_color_hex(0xDCE8F5)
#define APP_WIFI_C_SURFACE     lv_color_hex(0xF7FAFD)
#define APP_WIFI_C_MUTED       lv_color_hex(0xE2ECF6)
#define APP_WIFI_C_INK         lv_color_hex(0x122033)
#define APP_WIFI_C_INK_MUTED   lv_color_hex(0x53667B)
#define APP_WIFI_C_SYSTEM      lv_color_hex(0x101A2B)
#define APP_WIFI_C_SYSTEM_TEXT lv_color_hex(0xF8FBFF)
#define APP_WIFI_C_ACCENT      lv_color_hex(0x147EFB)
#define APP_WIFI_C_ACCENT_SOFT lv_color_hex(0xD9EAFF)
#define APP_WIFI_C_SUCCESS     lv_color_hex(0x22A96B)
#define APP_WIFI_C_DANGER      lv_color_hex(0xE05151)

/**
 * @brief Apply the panel background and corner radius in pixels.
 * @param object Panel to style.
 * @param color Background color.
 * @param radius Corner radius in pixels.
 */
void app_wifi_style_panel(lv_obj_t *object, lv_color_t color, int radius);
/**
 * @brief Create a wrapping label owned by parent.
 * @param parent Parent object.
 * @param text Label text.
 * @param color Text color.
 * @return Created label object.
 */
lv_obj_t *app_wifi_make_flex_label(lv_obj_t *parent, const char *text, lv_color_t color);
/**
 * @brief Create a positioned button and attach its click callback.
 * @param parent Parent object.
 * @param text Button caption.
 * @param x Horizontal position in pixels.
 * @param y Vertical position in pixels.
 * @param width Button width in pixels.
 * @param height Button height in pixels.
 * @param color Button background color.
 * @param callback Optional click callback.
 * @param user_data Context passed to the callback.
 * @return Created button object.
 */
lv_obj_t *app_wifi_make_button(lv_obj_t *parent, const char *text, int x, int y, int width, int height,
                                  lv_color_t color, lv_event_cb_t callback, void *user_data);

#ifdef __cplusplus
}
#endif
