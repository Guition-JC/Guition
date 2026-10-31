#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types_generic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SERVICE_MAX_APS 24
#define WIFI_SERVICE_SSID_MAX_LEN 32
#define WIFI_SERVICE_PASSWORD_MAX_LEN 63
#define WIFI_SERVICE_PROVISIONING_TOKEN_LENGTH 10

typedef enum {
    WIFI_SERVICE_SECURITY_OPEN,
    WIFI_SERVICE_SECURITY_PSK,
} wifi_service_security_t;

/** @brief Scanned wireless access point. */
typedef struct {
    char ssid[WIFI_SERVICE_SSID_MAX_LEN + 1];
    int8_t rssi;
    uint8_t channel;
    wifi_auth_mode_t authmode;
} wifi_service_ap_t;

/** @brief Current station link and IPv4 information. */
typedef struct {
    bool connected;
    char ssid[WIFI_SERVICE_SSID_MAX_LEN + 1];
    char ipv4[16];
    int8_t rssi;
    uint8_t channel;
} wifi_service_connection_info_t;

typedef enum {
    WIFI_SERVICE_STATE_IDLE,
    WIFI_SERVICE_STATE_SCANNING,
    WIFI_SERVICE_STATE_READY,
    WIFI_SERVICE_STATE_CONNECTING,
    WIFI_SERVICE_STATE_CONNECTED,
    WIFI_SERVICE_STATE_FAILED,
} wifi_service_state_t;

/** @brief Notify IPv4 connectivity on the Wi-Fi event task with the service lock released. LVGL access requires the display adapter lock. */
typedef void (*wifi_service_link_listener_t)(bool connected, void *user_data);

/**
 * @brief Initialize Wi-Fi resources and restore saved credentials.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_service_init(void);
/**
 * @brief Start the Wi-Fi driver and reconnect using the retained station configuration.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_service_start(void);
/**
 * @brief Return whether the Wi-Fi driver is currently started.
 * @return true if the Wi-Fi driver is currently started; false otherwise.
 */
bool wifi_service_is_started(void);
/**
 * @brief Start an asynchronous access-point scan.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_service_start_scan(void);
/**
 * @brief Return the current Wi-Fi service state.
 * @return The current Wi-Fi service state.
 */
wifi_service_state_t wifi_service_get_state(void);
/**
 * @brief Register the single station link listener; NULL unregisters. Idempotent.
 * @param listener Listener callback.
 * @param user_data Caller-supplied context.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_service_register_link_listener(wifi_service_link_listener_t listener, void *user_data);
/**
 * @brief Copy the current station connection information.
 * @param[out] info Receives the copied information.
 * @return true if connection information was copied; false otherwise.
 */
bool wifi_service_get_connection_info(wifi_service_connection_info_t *info);
/**
 * @brief Copy up to capacity scan results and return the copied count.
 * @param[out] results Receives the scan results.
 * @param capacity Capacity of the destination buffer.
 * @return Number of scan results copied.
 */
size_t wifi_service_get_scan_results(wifi_service_ap_t *results, size_t capacity);
/**
 * @brief Atomically copy the latest scan list and its in-progress flag.
 * @param results Destination for results.
 * @param capacity Capacity of the destination buffer.
 * @param scanning Receives the scan-in-progress flag.
 * @return Number of scan results copied.
 */
size_t wifi_service_get_scan_snapshot(wifi_service_ap_t *results, size_t capacity, bool *scanning);
/**
 * @brief Disconnect STA without stopping the provisioning AP or erasing saved credentials.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_service_disconnect(void);
/**
 * @brief Connect to the requested network and retain its credentials.
 * @param ssid Network SSID.
 * @param security Security mode.
 * @param password Network password.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_service_connect(const char *ssid, wifi_service_security_t security, const char *password);
/**
 * @brief Enable AP-based Wi-Fi provisioning.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_service_start_provisioning(void);
/**
 * @brief Disable AP-based Wi-Fi provisioning.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_service_stop_provisioning(void);
/**
 * @brief Borrow the active provisioning AP SSID.
 * @return Borrowed active provisioning AP SSID.
 */
const char *wifi_service_get_provisioning_ssid(void);
/**
 * @brief Borrow the active provisioning AP password.
 * @return Borrowed active provisioning AP password.
 */
const char *wifi_service_get_provisioning_password(void);
/**
 * @brief Borrow the active provisioning session token.
 * @return Borrowed active provisioning session token.
 */
const char *wifi_service_get_provisioning_token(void);
/**
 * @brief Borrow the provisioning QR-code payload.
 * @return Borrowed provisioning QR-code payload.
 */
const char *wifi_service_get_provisioning_qr_payload(void);

#ifdef __cplusplus
}
#endif
