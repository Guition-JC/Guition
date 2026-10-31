#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "dns_captive.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Borrowed provisioning state and output buffers. */
typedef struct {
    esp_netif_t *ap_netif;
    bool *wifi_started;
    bool *scan_in_progress;
    dns_captive_handle_t *dns_handle;
    bool *active;
    char *ssid;
    size_t ssid_capacity;
    char *password;
    size_t password_capacity;
    char *token;
    size_t token_capacity;
    char *qr_payload;
    size_t qr_capacity;
} wifi_provisioning_context_t;

/**
 * @brief Start the provisioning AP, captive DNS and portal.
 * @param context Caller-supplied context.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_provisioning_start(const wifi_provisioning_context_t *context);
/**
 * @brief Stop the provisioning portal, captive DNS and AP.
 * @param context Caller-supplied context.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_provisioning_stop(const wifi_provisioning_context_t *context);

#ifdef __cplusplus
}
#endif
