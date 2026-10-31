#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Clear references to connection-panel widgets.
 */
void app_wifi_connection_view_reset(void);
/**
 * @brief Create connection details and the disconnect action under parent.
 * @param parent Parent object for connection details.
 */
void app_wifi_connection_view_create(lv_obj_t *parent);
/**
 * @brief Refresh displayed connection details from the Wi-Fi service.
 */
void app_wifi_connection_view_update(void);

#ifdef __cplusplus
}
#endif
