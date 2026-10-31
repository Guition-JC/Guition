#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Owned captive DNS server handle. */
typedef struct dns_captive_server *dns_captive_handle_t;

/**
 * @brief Start a DNS responder for the supplied network interface and return its owned handle.
 * @param netif Network interface.
 * @param handle Handle of the resource.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t dns_captive_start(esp_netif_t *netif, dns_captive_handle_t *handle);

/**
 * @brief Stop the server after socket cleanup; on timeout retain the handle so cleanup can be retried.
 * @param handle Handle of the resource.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t dns_captive_stop(dns_captive_handle_t *handle);

#ifdef __cplusplus
}
#endif
