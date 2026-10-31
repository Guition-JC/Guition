#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register /weather and weather API routes on the supplied HTTP server.
 * @param server Server instance.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t weather_web_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
