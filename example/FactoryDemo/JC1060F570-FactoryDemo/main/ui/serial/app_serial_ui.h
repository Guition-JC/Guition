#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_serial_toast_cb_t)(const char *message);

/**
 * @brief Create terminal controls under the supplied fullscreen objects.
 * @param body Parent object for terminal controls.
 * @param screen Full-screen host object.
 * @param show_toast Callback for displaying messages.
 */
void app_serial_ui_build(lv_obj_t *body, lv_obj_t *screen, app_serial_toast_cb_t show_toast);

/**
 * @brief Delete terminal timers and transient controls before rebuilding.
 */
void app_serial_ui_destroy(void);

#ifdef __cplusplus
}
#endif
