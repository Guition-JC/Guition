#pragma once

#include <stdbool.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wifi_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Record station association under the supplied mutex.
 * @param lock Lock used to protect the operation.
 * @param event Event to process.
 */
void wifi_connection_on_sta_connected(SemaphoreHandle_t lock, const wifi_event_sta_connected_t *event);
/**
 * @brief Clear station connection information under the supplied mutex.
 * @param lock Lock used to protect the operation.
 */
void wifi_connection_on_sta_disconnected(SemaphoreHandle_t lock);
/**
 * @brief Record the station IPv4 address under the supplied mutex.
 * @param lock Lock used to protect the operation.
 * @param event Event to process.
 */
void wifi_connection_on_got_ip(SemaphoreHandle_t lock, const ip_event_got_ip_t *event);
/**
 * @brief Copy station connection information under the supplied mutex.
 * @param lock Lock used to protect the operation.
 * @param[out] info Receives the copied information.
 * @return true if connection information was copied; false otherwise.
 */
bool wifi_connection_get_info(SemaphoreHandle_t lock, wifi_service_connection_info_t *info);

#ifdef __cplusplus
}
#endif
