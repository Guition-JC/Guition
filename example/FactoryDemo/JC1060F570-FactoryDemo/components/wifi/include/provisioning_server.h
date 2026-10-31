#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the provisioning portal with its AP interface and session token.
 * @param ap_netif Access-point network interface.
 * @param token Operation token.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t provisioning_server_start(esp_netif_t *ap_netif, const char *token);
/**
 * @brief Stop the provisioning portal and release its server resources.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t provisioning_server_stop(void);
/**
 * @brief Invalidate the current provisioning session token.
 */
void provisioning_server_invalidate_token(void);

/** @brief Notify portal activation on the task that starts or stops the server; active indicates ownership of port 80. */
typedef void (*provisioning_server_state_listener_t)(bool active, void *user_data);

/**
 * @brief Register the single state listener; NULL unregisters. Idempotent.
 * @param listener Listener callback.
 * @param user_data Caller-supplied context.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t provisioning_server_register_state_listener(provisioning_server_state_listener_t listener,
                                                      void *user_data);

#ifdef __cplusplus
}
#endif
