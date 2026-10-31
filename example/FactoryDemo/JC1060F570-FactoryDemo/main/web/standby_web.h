/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "cJSON.h"
#include "esp_err.h"

/**
 * @brief Append the current standby preferences and sleep timeout to JSON.
 * @param[in,out] root Caller-owned settings object.
 * @return ESP_OK, ESP_ERR_TIMEOUT or ESP_ERR_NO_MEM.
 */
esp_err_t standby_web_add_settings(cJSON *root);
/**
 * @brief Validate and apply standby settings without sending an HTTP response.
 * @param root Request JSON containing optional standby and timeout fields.
 * @return NULL on success, otherwise a static error code string.
 */
const char *standby_web_apply(const cJSON *root);
/**
 * @brief List supported local photos and videos without starting playback.
 * @return Caller-owned JSON, or NULL on allocation or display-lock failure.
 */
cJSON *standby_web_media(void);
