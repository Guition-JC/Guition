#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_ethernet_toast_cb_t)(const char *message);

/**
 * @brief Build Ethernet status and throughput controls under parent.
 * @param parent Parent LVGL object.
 * @param toast_cb Callback used to display messages.
 */
void app_ethernet_view_build(lv_obj_t *parent, app_ethernet_toast_cb_t toast_cb);
/**
 * @brief Stop page polling, request throughput shutdown and clear page state.
 */
void app_ethernet_view_cleanup(void);

#ifdef __cplusplus
}
#endif
