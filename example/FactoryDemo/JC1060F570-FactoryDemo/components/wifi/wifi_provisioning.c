#include "wifi_provisioning.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "provisioning_server.h"
#define PROVISIONING_AP_CHANNEL 1
#define PROVISIONING_AP_MAX_CONNECTIONS 4

static const char *TAG = "wifi_provisioning";

static void build_qr_payload(const wifi_provisioning_context_t *context)
{
    snprintf(context->qr_payload, context->qr_capacity, "WIFI:T:WPA;S:%s;P:%s;;",
             context->ssid, context->password);
}

static void clear_sensitive_state(const wifi_provisioning_context_t *context)
{
    if (context->ssid != NULL && context->ssid_capacity > 0) {
        context->ssid[0] = '\0';
    }
    if (context->password != NULL && context->password_capacity > 0) {
        context->password[0] = '\0';
    }
    if (context->token != NULL && context->token_capacity > 0) {
        context->token[0] = '\0';
    }
    if (context->qr_payload != NULL && context->qr_capacity > 0) {
        context->qr_payload[0] = '\0';
    }
}

static void record_cleanup_error(esp_err_t *first_error, const char *step, esp_err_t ret)
{
    if (ret == ESP_OK) {
        return;
    }
    ESP_LOGE(TAG, "%s failed: %s", step, esp_err_to_name(ret));
    if (*first_error == ESP_OK) {
        *first_error = ret;
    }
}

static void rollback_start(const wifi_provisioning_context_t *context)
{
    provisioning_server_invalidate_token();

    const esp_err_t server_ret = provisioning_server_stop();
    if (server_ret != ESP_OK) {
        ESP_LOGE(TAG, "rollback provisioning HTTP server failed: %s", esp_err_to_name(server_ret));
    }
    esp_err_t dns_ret = ESP_OK;
    if (*context->dns_handle != NULL) {
        dns_ret = dns_captive_stop(context->dns_handle);
        if (dns_ret != ESP_OK) {
            ESP_LOGE(TAG, "rollback captive DNS stop failed: %s", esp_err_to_name(dns_ret));
        }
    }

    const esp_err_t mode_ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (mode_ret != ESP_OK) {
        ESP_LOGE(TAG, "rollback provisioning AP failed: %s", esp_err_to_name(mode_ret));
    }
    *context->active = server_ret != ESP_OK || dns_ret != ESP_OK || *context->dns_handle != NULL ||
                       mode_ret != ESP_OK;
    clear_sensitive_state(context);
}

esp_err_t wifi_provisioning_start(const wifi_provisioning_context_t *context)
{
    if (context == NULL || context->ap_netif == NULL || context->wifi_started == NULL ||
        context->scan_in_progress == NULL || context->dns_handle == NULL || context->active == NULL ||
        context->ssid == NULL || context->password == NULL || context->token == NULL ||
        context->qr_payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*context->active) {
        return ESP_ERR_INVALID_STATE;
    }

    provisioning_server_invalidate_token();
    esp_err_t ret = ESP_OK;
    uint8_t mac[6];
    ret = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read Wi-Fi MAC failed: %s", esp_err_to_name(ret));
        goto rollback;
    }
    snprintf(context->ssid, context->ssid_capacity, "JC1060-SETUP-%02X%02X", mac[4], mac[5]);
    snprintf(context->password, context->password_capacity, "setup-%02X%02X%02X", mac[3], mac[4], mac[5]);
    build_qr_payload(context);

    if (*context->scan_in_progress) {
        ret = esp_wifi_scan_stop();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_STATE) {
            ESP_LOGE(TAG, "stop scan before provisioning failed: %s", esp_err_to_name(ret));
            goto rollback;
        }
        *context->scan_in_progress = false;
    }

    wifi_config_t config = {
        .ap = {
            .channel = PROVISIONING_AP_CHANNEL,
            .max_connection = PROVISIONING_AP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    memcpy(config.ap.ssid, context->ssid, strlen(context->ssid));
    memcpy(config.ap.password, context->password, strlen(context->password));
    config.ap.ssid_len = strlen(context->ssid);

    ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "enable APSTA mode failed: %s", esp_err_to_name(ret));
        goto rollback;
    }
    ret = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set provisioning AP config failed: %s", esp_err_to_name(ret));
        goto rollback;
    }
    if (!*context->wifi_started) {
        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "start provisioning Wi-Fi failed: %s", esp_err_to_name(ret));
            goto rollback;
        }
        *context->wifi_started = true;
    }
    if (*context->dns_handle == NULL) {
        ret = dns_captive_start(context->ap_netif, context->dns_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "start captive DNS failed: %s", esp_err_to_name(ret));
            goto rollback;
        }
    }
    ret = provisioning_server_start(context->ap_netif, context->token);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start provisioning server failed: %s", esp_err_to_name(ret));
        goto rollback;
    }
    *context->active = true;
    ESP_LOGI(TAG, "provisioning AP ready: ssid=%s", context->ssid);
    return ESP_OK;

rollback:
    rollback_start(context);
    return ret;
}

esp_err_t wifi_provisioning_stop(const wifi_provisioning_context_t *context)
{
    if (context == NULL || context->wifi_started == NULL || context->dns_handle == NULL ||
        context->active == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    provisioning_server_invalidate_token();
    esp_err_t first_error = ESP_OK;

    const esp_err_t http_ret = provisioning_server_stop();
    record_cleanup_error(&first_error, "stop provisioning HTTP server", http_ret);

    esp_err_t dns_ret = ESP_OK;
    if (*context->dns_handle != NULL) {
        dns_ret = dns_captive_stop(context->dns_handle);
        record_cleanup_error(&first_error, "stop captive DNS", dns_ret);
    }

    const esp_err_t mode_ret = esp_wifi_set_mode(WIFI_MODE_STA);
    record_cleanup_error(&first_error, "disable provisioning AP", mode_ret);

    clear_sensitive_state(context);
    *context->active = http_ret != ESP_OK || dns_ret != ESP_OK || *context->dns_handle != NULL || mode_ret != ESP_OK;
    if (*context->active) {
        ESP_LOGW(TAG, "provisioning cleanup incomplete; retry is safe");
    } else {
        ESP_LOGI(TAG, "provisioning AP stopped; STA mode retained");
    }
    return first_error;
}
