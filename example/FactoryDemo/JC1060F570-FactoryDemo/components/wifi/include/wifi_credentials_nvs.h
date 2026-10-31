#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "wifi_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Stored target-network credentials. */
typedef struct {
    char ssid[WIFI_SERVICE_SSID_MAX_LEN + 1];
    char password[WIFI_SERVICE_PASSWORD_MAX_LEN + 1];
    wifi_service_security_t security;
} wifi_credentials_t;

/**
 * @brief Load a complete, versioned target Wi-Fi record; missing data is reported as found=false.
 * @param[out] credentials Receives the stored credentials.
 * @param[out] found Receives whether a record was found.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_credentials_nvs_load(wifi_credentials_t *credentials, bool *found);

/**
 * @brief Persist one validated target Wi-Fi record using an invalid-write-valid commit sequence.
 * @param credentials Wi-Fi credentials.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_credentials_nvs_save(const wifi_credentials_t *credentials);

/**
 * @brief Erase all target Wi-Fi credentials from the dedicated namespace.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_credentials_nvs_clear(void);

#ifdef __cplusplus
}
#endif
