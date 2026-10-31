#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the provisioning QR code and access instructions under parent.
 * @param parent Parent object for provisioning controls.
 */
void app_wifi_provisioning_view_create(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif
