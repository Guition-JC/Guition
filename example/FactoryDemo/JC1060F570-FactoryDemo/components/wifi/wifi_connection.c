#include "wifi_connection.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"

static const char *TAG = "wifi_connection";
#define WIFI_CONNECTION_LOCK_TIMEOUT_MS 2000U
static wifi_service_connection_info_t s_info;

void wifi_connection_on_sta_connected(SemaphoreHandle_t lock, const wifi_event_sta_connected_t *event)
{
    const size_t ssid_length = event->ssid_len < WIFI_SERVICE_SSID_MAX_LEN
                                   ? event->ssid_len
                                   : WIFI_SERVICE_SSID_MAX_LEN;
    if (lock == NULL || xSemaphoreTake(lock, pdMS_TO_TICKS(WIFI_CONNECTION_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    memcpy(s_info.ssid, event->ssid, ssid_length);
    s_info.ssid[ssid_length] = '\0';
    s_info.connected = false;
    s_info.ipv4[0] = '\0';
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "STA connected to AP; waiting for DHCP");
}

void wifi_connection_on_sta_disconnected(SemaphoreHandle_t lock)
{
    if (lock == NULL || xSemaphoreTake(lock, pdMS_TO_TICKS(WIFI_CONNECTION_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    s_info.connected = false;
    s_info.ipv4[0] = '\0';
    xSemaphoreGive(lock);
}

void wifi_connection_on_got_ip(SemaphoreHandle_t lock, const ip_event_got_ip_t *event)
{
    wifi_ap_record_t ap_info = {0};
    const bool has_ap_info = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
    char ipv4[sizeof(s_info.ipv4)];

    if (lock == NULL || xSemaphoreTake(lock, pdMS_TO_TICKS(WIFI_CONNECTION_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    snprintf(s_info.ipv4, sizeof(s_info.ipv4), IPSTR, IP2STR(&event->ip_info.ip));
    s_info.connected = true;
    if (has_ap_info) {
        s_info.rssi = ap_info.rssi;
        s_info.channel = ap_info.primary;
    }
    memcpy(ipv4, s_info.ipv4, sizeof(ipv4));
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "STA got IPv4: %s", ipv4);
}

bool wifi_connection_get_info(SemaphoreHandle_t lock, wifi_service_connection_info_t *info)
{
    if (lock == NULL || info == NULL) {
        return false;
    }
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(WIFI_CONNECTION_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    *info = s_info;
    xSemaphoreGive(lock);
    return info->connected;
}
