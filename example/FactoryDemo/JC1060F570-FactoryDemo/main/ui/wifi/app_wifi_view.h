#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_wifi_toast_cb_t)(const char *message);

/**
 * @brief Build the network list and schedule the initial scan.
 * @param parent Parent object for the network list.
 * @param toast_cb Callback for displaying messages.
 */
void app_wifi_view_build(lv_obj_t *parent, app_wifi_toast_cb_t toast_cb);
/**
 * @brief Stop page timers and release page-owned scan records.
 */
void app_wifi_view_cleanup(void);

#ifdef __cplusplus
}
#endif
