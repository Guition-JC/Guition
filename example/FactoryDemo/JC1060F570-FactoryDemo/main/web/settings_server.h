/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * @file
 * @brief Settings HTTP server sharing port 80 with the provisioning portal.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register network listeners and serve settings when an address is available.
 * @return ESP_OK on success or an event-registration error.
 */
esp_err_t settings_server_init(void);

/**
 * @brief Stop the settings HTTP server if it is running.
 */
void settings_server_stop(void);

#ifdef __cplusplus
}
#endif
