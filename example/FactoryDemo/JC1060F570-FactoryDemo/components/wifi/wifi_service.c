#include "wifi_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "wifi_provisioning.h"
#include "provisioning_server.h"
#include "wifi_connection.h"
#include "wifi_credentials_nvs.h"
#include "time_service.h"

static const char *TAG = "wifi_service";

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static SemaphoreHandle_t s_lock;
/** Scan result storage used by esp_wifi_scan_get_ap_records(). */
EXT_RAM_BSS_ATTR static wifi_service_ap_t s_scan_results[WIFI_SERVICE_MAX_APS];
EXT_RAM_BSS_ATTR static wifi_ap_record_t s_scan_records[WIFI_SERVICE_MAX_APS];
static size_t s_scan_count;
static wifi_service_state_t s_state = WIFI_SERVICE_STATE_IDLE;
static wifi_service_link_listener_t s_link_listener;
static void *s_link_listener_user_data;
static bool s_link_listener_connected;
static dns_captive_handle_t s_dns_handle;
static bool s_provisioning_active;
static bool s_wifi_started;
static bool s_wifi_transitioning;
static bool s_scan_in_progress;
static char s_provisioning_ssid[WIFI_SERVICE_SSID_MAX_LEN + 1];
static char s_provisioning_password[WIFI_SERVICE_PASSWORD_MAX_LEN + 1];
static char s_provisioning_token[WIFI_SERVICE_PROVISIONING_TOKEN_LENGTH + 1];
static char s_provisioning_qr_payload[160];
static TaskHandle_t s_provisioning_shutdown_worker;
static bool s_provisioning_starting;
static wifi_credentials_t s_pending_credentials;
static bool s_pending_credentials_valid;
/** A replacement config is applied only after the old STA disconnect event. */
static bool s_disconnect_requested;
static bool s_reconnect_pending;
static bool s_station_request;
static bool s_provisioning_connect_requested;
static wifi_config_t s_next_config;

static const char PROVISIONING_TOKEN_ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
#define PROVISIONING_SHUTDOWN_DELAY_MS 3000
#define PROVISIONING_SHUTDOWN_TASK_STACK 3072
#define PROVISIONING_SHUTDOWN_TASK_PRIORITY 4
#define WIFI_SERVICE_LOCK_TIMEOUT_MS 2000U

static void clear_provisioning_token(void)
{
    memset(s_provisioning_token, 0, sizeof(s_provisioning_token));
}

static void clear_pending_credentials(void)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    memset(&s_pending_credentials, 0, sizeof(s_pending_credentials));
    s_pending_credentials_valid = false;
    xSemaphoreGive(s_lock);
}

static void set_pending_credentials(const char *ssid, wifi_service_security_t security, const char *password)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    memset(&s_pending_credentials, 0, sizeof(s_pending_credentials));
    memcpy(s_pending_credentials.ssid, ssid, strlen(ssid));
    memcpy(s_pending_credentials.password, password, strlen(password));
    s_pending_credentials.security = security;
    s_pending_credentials_valid = true;
    xSemaphoreGive(s_lock);
}

static void persist_pending_credentials(void)
{
    wifi_credentials_t credentials = {0};
    wifi_service_connection_info_t connection = {0};

    bool pending = false;
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    pending = s_pending_credentials_valid;
    if (pending) {
        credentials = s_pending_credentials;
    }
    xSemaphoreGive(s_lock);
    if (!pending || !wifi_connection_get_info(s_lock, &connection) ||
        strcmp(connection.ssid, credentials.ssid) != 0) {
        memset(&credentials, 0, sizeof(credentials));
        return;
    }

    const esp_err_t ret = wifi_credentials_nvs_save(&credentials);
    memset(&credentials, 0, sizeof(credentials));
    clear_pending_credentials();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "save connected target Wi-Fi credentials failed: %s", esp_err_to_name(ret));
    }
}

static esp_err_t restore_saved_wifi_credentials(void)
{
    wifi_credentials_t credentials = {0};
    bool found = false;
    esp_err_t ret = wifi_credentials_nvs_load(&credentials, &found);
    if (ret == ESP_ERR_INVALID_RESPONSE) {
        const esp_err_t clear_ret = wifi_credentials_nvs_clear();
        if (clear_ret == ESP_OK) {
            ESP_LOGW(TAG, "invalid saved target Wi-Fi credentials cleared");
            return ESP_OK;
        }
        ESP_LOGE(TAG, "clear invalid saved target Wi-Fi credentials failed: %s",
                 esp_err_to_name(clear_ret));
        return clear_ret;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "load saved target Wi-Fi credentials failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (!found) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "restoring saved target Wi-Fi connection");
    ret = wifi_service_connect(credentials.ssid, credentials.security, credentials.password);
    memset(&credentials, 0, sizeof(credentials));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "restore saved target Wi-Fi connection failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static void generate_provisioning_token(void)
{
    uint8_t random_bytes[WIFI_SERVICE_PROVISIONING_TOKEN_LENGTH];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (size_t index = 0; index < WIFI_SERVICE_PROVISIONING_TOKEN_LENGTH; ++index) {
        s_provisioning_token[index] = PROVISIONING_TOKEN_ALPHABET[random_bytes[index] & 0x1FU];
    }
    s_provisioning_token[WIFI_SERVICE_PROVISIONING_TOKEN_LENGTH] = '\0';
    memset(random_bytes, 0, sizeof(random_bytes));
}

static void set_state(wifi_service_state_t state)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    s_state = state;
    xSemaphoreGive(s_lock);
}

/**
 * @brief Notify link transitions outside s_lock; listeners may query service state.
 * @param connected Current link connection state.
 */
static void notify_link_listener(bool connected)
{
    wifi_service_link_listener_t listener = NULL;
    void *user_data = NULL;

    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    if (s_link_listener_connected != connected) {
        s_link_listener_connected = connected;
        listener = s_link_listener;
        user_data = s_link_listener_user_data;
    }
    xSemaphoreGive(s_lock);

    if (listener != NULL) {
        listener(connected, user_data);
    }
}

static wifi_provisioning_context_t provisioning_context(void)
{
    return (wifi_provisioning_context_t){
        .ap_netif = s_ap_netif,
        .wifi_started = &s_wifi_started,
        .scan_in_progress = &s_scan_in_progress,
        .dns_handle = &s_dns_handle,
        .active = &s_provisioning_active,
        .ssid = s_provisioning_ssid,
        .ssid_capacity = sizeof(s_provisioning_ssid),
        .password = s_provisioning_password,
        .password_capacity = sizeof(s_provisioning_password),
        .token = s_provisioning_token,
        .token_capacity = sizeof(s_provisioning_token),
        .qr_payload = s_provisioning_qr_payload,
        .qr_capacity = sizeof(s_provisioning_qr_payload),
    };
}

static void provisioning_shutdown_worker(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(PROVISIONING_SHUTDOWN_DELAY_MS));

    const esp_err_t stop_ret = wifi_service_stop_provisioning();
    if (stop_ret != ESP_OK && stop_ret != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "stop provisioning after STA IP failed: %s", esp_err_to_name(stop_ret));
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "clear provisioning shutdown worker state timed out");
        vTaskDelete(NULL);
        return;
    }
    s_provisioning_shutdown_worker = NULL;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

static esp_err_t schedule_provisioning_shutdown(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_provisioning_shutdown_worker != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (!s_provisioning_active || !s_provisioning_connect_requested) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    TaskHandle_t worker = NULL;
    const BaseType_t created = xTaskCreate(provisioning_shutdown_worker, "wifi_prov_stop",
                                           PROVISIONING_SHUTDOWN_TASK_STACK, NULL,
                                           PROVISIONING_SHUTDOWN_TASK_PRIORITY, &worker);
    if (created != pdPASS) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    s_provisioning_shutdown_worker = worker;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static esp_err_t start_station_connection(wifi_config_t *config)
{
    set_state(WIFI_SERVICE_STATE_CONNECTING);
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, config);
    if (ret == ESP_OK) {
        ret = esp_wifi_connect();
    }
    if (ret != ESP_OK) {
        clear_pending_credentials();
        set_state(WIFI_SERVICE_STATE_FAILED);
        ESP_LOGE(TAG, "start station connection failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static bool handle_requested_disconnect(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return true;
    }
    if (!s_disconnect_requested) {
        xSemaphoreGive(s_lock);
        return false;
    }
    const bool reconnect = s_reconnect_pending;
    wifi_config_t next_config = s_next_config;
    s_disconnect_requested = false;
    s_reconnect_pending = false;
    memset(&s_next_config, 0, sizeof(s_next_config));
    xSemaphoreGive(s_lock);

    wifi_connection_on_sta_disconnected(s_lock);
    time_service_notify_network_lost();
    if (reconnect) {
        start_station_connection(&next_config);
    } else {
        set_state(WIFI_SERVICE_STATE_IDLE);
    }
    memset(&next_config, 0, sizeof(next_config));
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE) {
        s_station_request = false;
        xSemaphoreGive(s_lock);
    }
    return true;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Wi-Fi event handler lock timed out");
        return;
    }
    const bool ignore_event = s_wifi_transitioning || !s_wifi_started ||
        (s_disconnect_requested &&
         (event_base == IP_EVENT || event_id == WIFI_EVENT_STA_CONNECTED));
    xSemaphoreGive(s_lock);
    if (ignore_event) {
        return;
    }
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_SCAN_DONE: {
            uint16_t count = WIFI_SERVICE_MAX_APS;
            const esp_err_t records_ret = esp_wifi_scan_get_ap_records(&count, s_scan_records);
            wifi_service_connection_info_t connection = {0};
            if (records_ret == ESP_OK) {
                wifi_connection_get_info(s_lock, &connection);
            }
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
                ESP_LOGW(TAG, "Wi-Fi scan result lock timed out");
                return;
            }
            const bool scan_expected = s_scan_in_progress;
            s_scan_in_progress = false;
            if (!scan_expected) {
                xSemaphoreGive(s_lock);
                break;
            }
            if (records_ret == ESP_OK) {
                s_scan_count = count;
                for (size_t index = 0; index < count; ++index) {
                    memcpy(s_scan_results[index].ssid, s_scan_records[index].ssid,
                           sizeof(s_scan_results[index].ssid));
                    s_scan_results[index].ssid[WIFI_SERVICE_SSID_MAX_LEN] = '\0';
                    s_scan_results[index].rssi = s_scan_records[index].rssi;
                    s_scan_results[index].channel = s_scan_records[index].primary;
                    s_scan_results[index].authmode = s_scan_records[index].authmode;
                }
                if (s_state == WIFI_SERVICE_STATE_SCANNING) {
                    s_state = connection.connected ? WIFI_SERVICE_STATE_CONNECTED : WIFI_SERVICE_STATE_READY;
                }
            } else if (s_state == WIFI_SERVICE_STATE_SCANNING) {
                s_state = WIFI_SERVICE_STATE_FAILED;
            }
            xSemaphoreGive(s_lock);
            if (records_ret == ESP_OK) {
                ESP_LOGI(TAG, "scan complete: %u APs", (unsigned int)count);
            } else {
                ESP_LOGE(TAG, "scan complete but esp_wifi_scan_get_ap_records failed");
            }
            break;
        }
        case WIFI_EVENT_STA_CONNECTED:
            wifi_connection_on_sta_connected(s_lock, event_data);
            set_state(WIFI_SERVICE_STATE_CONNECTING);
            /** Associated but without an IPv4 address: not usable yet. */
            notify_link_listener(false);
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *event = event_data;
            if (handle_requested_disconnect()) {
                notify_link_listener(false);
                break;
            }
            const wifi_service_state_t state = wifi_service_get_state();
            wifi_service_connection_info_t connection = {0};
            const bool was_connected = wifi_service_get_connection_info(&connection);
            ESP_LOGW(TAG, "STA disconnected: reason=%d state=%d", event->reason, state);
            if (was_connected || state == WIFI_SERVICE_STATE_CONNECTING || state == WIFI_SERVICE_STATE_CONNECTED) {
                wifi_connection_on_sta_disconnected(s_lock);
                time_service_notify_network_lost();
                set_state(WIFI_SERVICE_STATE_FAILED);
            }
            notify_link_listener(false);
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_connection_on_got_ip(s_lock, event_data);
        esp_netif_dns_info_t dns_main = {0};
        esp_netif_dns_info_t dns_backup = {0};
        const esp_err_t dns_main_ret = esp_netif_get_dns_info(
            s_sta_netif, ESP_NETIF_DNS_MAIN, &dns_main);
        const esp_err_t dns_backup_ret = esp_netif_get_dns_info(
            s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns_backup);
        if (dns_main_ret == ESP_OK || dns_backup_ret == ESP_OK) {
            char dns_main_text[16] = "unset";
            char dns_backup_text[16] = "unset";
            if (dns_main_ret == ESP_OK) {
                snprintf(dns_main_text, sizeof(dns_main_text), IPSTR,
                         IP2STR(&dns_main.ip.u_addr.ip4));
            }
            if (dns_backup_ret == ESP_OK) {
                snprintf(dns_backup_text, sizeof(dns_backup_text), IPSTR,
                         IP2STR(&dns_backup.ip.u_addr.ip4));
            }
            ESP_LOGI(TAG, "STA DNS main=%s backup=%s", dns_main_text, dns_backup_text);
        } else {
            ESP_LOGW(TAG, "STA DNS unavailable: main=%s backup=%s",
                     esp_err_to_name(dns_main_ret), esp_err_to_name(dns_backup_ret));
        }
        set_state(WIFI_SERVICE_STATE_CONNECTED);
        notify_link_listener(true);
        const esp_err_t time_ret = time_service_notify_network_ready();
        if (time_ret != ESP_OK) {
            ESP_LOGW(TAG, "time service network-ready notification failed: %s",
                     esp_err_to_name(time_ret));
        }
        persist_pending_credentials();
        const esp_err_t schedule_ret = schedule_provisioning_shutdown();
        if (schedule_ret != ESP_OK && schedule_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "schedule provisioning shutdown failed: %s", esp_err_to_name(schedule_ret));
        }
    }
}

static esp_err_t initialize_network_stack(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "initialize NVS failed");

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&config), TAG, "initialize Wi-Fi failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set Wi-Fi storage failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
                        TAG, "register Wi-Fi event handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL),
                        TAG, "register IP event handler failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set Wi-Fi mode failed");
    ret = esp_wifi_start();
    if (ret == ESP_OK) {
        s_wifi_started = true;
    }
    return ret;
}

esp_err_t wifi_service_init(void)
{
    if (s_lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t ret = initialize_network_stack();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi service initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_err_t time_ret = time_service_init();
    if (time_ret != ESP_OK) {
        ESP_LOGW(TAG, "time service unavailable; Wi-Fi remains usable: %s",
                 esp_err_to_name(time_ret));
    }

    const esp_err_t restore_ret = restore_saved_wifi_credentials();
    if (restore_ret != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi service initialized without saved credential restore");
    } else {
        ESP_LOGI(TAG, "Wi-Fi service initialized");
    }
    return ESP_OK;
}

static bool wifi_has_station_config(void)
{
    wifi_config_t config = {0};
    return esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK && config.sta.ssid[0] != '\0';
}

bool wifi_service_is_started(void)
{
    if (s_lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    const bool started = s_wifi_started;
    xSemaphoreGive(s_lock);
    return started;
}

esp_err_t wifi_service_start(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_wifi_transitioning || s_provisioning_active || s_provisioning_starting ||
        s_provisioning_shutdown_worker != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_wifi_started) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_wifi_transitioning = true;
    xSemaphoreGive(s_lock);

    /** Do not hold s_lock while the driver emits start callbacks. */
    const esp_err_t start_ret = esp_wifi_start();
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Wi-Fi start completion lock timed out");
        return ESP_ERR_TIMEOUT;
    }
    s_wifi_transitioning = false;
    if (start_ret == ESP_OK) {
        s_wifi_started = true;
        s_state = WIFI_SERVICE_STATE_READY;
    }
    xSemaphoreGive(s_lock);
    if (start_ret != ESP_OK) {
        ESP_LOGE(TAG, "start Wi-Fi failed: %s", esp_err_to_name(start_ret));
        return start_ret;
    }

    if (wifi_has_station_config()) {
        set_state(WIFI_SERVICE_STATE_CONNECTING);
        const esp_err_t connect_ret = esp_wifi_connect();
        if (connect_ret != ESP_OK) {
            set_state(WIFI_SERVICE_STATE_FAILED);
            ESP_LOGE(TAG, "reconnect Wi-Fi failed: %s", esp_err_to_name(connect_ret));
            return connect_ret;
        }
    }
    ESP_LOGI(TAG, "Wi-Fi started; retained credentials reconnecting");
    return ESP_OK;
}

esp_err_t wifi_service_start_scan(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_scan_config_t config = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 40, .max = 120},
    };
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_wifi_started || s_wifi_transitioning || s_scan_in_progress || s_station_request ||
        s_state == WIFI_SERVICE_STATE_CONNECTING || s_provisioning_starting) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = WIFI_SERVICE_STATE_SCANNING;
    s_scan_in_progress = true;
    xSemaphoreGive(s_lock);

    const esp_err_t ret = esp_wifi_scan_start(&config, false);
    if (ret != ESP_OK) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Wi-Fi scan failure lock timed out");
            return ret;
        }
        s_scan_in_progress = false;
        s_state = WIFI_SERVICE_STATE_FAILED;
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "scan started");
    }
    return ret;
}

bool wifi_service_get_connection_info(wifi_service_connection_info_t *info)
{
    return wifi_connection_get_info(s_lock, info);
}

wifi_service_state_t wifi_service_get_state(void)
{
    if (s_lock == NULL) {
        return WIFI_SERVICE_STATE_FAILED;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return WIFI_SERVICE_STATE_FAILED;
    }
    const wifi_service_state_t state = s_state;
    xSemaphoreGive(s_lock);
    return state;
}

esp_err_t wifi_service_register_link_listener(wifi_service_link_listener_t listener, void *user_data)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_link_listener = listener;
    s_link_listener_user_data = user_data;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

size_t wifi_service_get_scan_results(wifi_service_ap_t *results, size_t capacity)
{
    bool scanning;
    return wifi_service_get_scan_snapshot(results, capacity, &scanning);
}

size_t wifi_service_get_scan_snapshot(wifi_service_ap_t *results, size_t capacity, bool *scanning)
{
    if (scanning == NULL) {
        return 0;
    }
    *scanning = false;
    if (s_lock == NULL || results == NULL || capacity == 0) {
        return 0;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return 0;
    }
    *scanning = s_scan_in_progress;
    const size_t count = s_scan_count < capacity ? s_scan_count : capacity;
    memcpy(results, s_scan_results, count * sizeof(*results));
    xSemaphoreGive(s_lock);
    return count;
}

esp_err_t wifi_service_connect(const char *ssid, wifi_service_security_t security, const char *password)
{
    if (s_lock == NULL || ssid == NULL || password == NULL ||
        (security != WIFI_SERVICE_SECURITY_OPEN && security != WIFI_SERVICE_SECURITY_PSK)) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t ssid_length = strlen(ssid);
    const size_t password_length = strlen(password);
    if (ssid_length == 0 || ssid_length > WIFI_SERVICE_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((security == WIFI_SERVICE_SECURITY_OPEN && password_length != 0) ||
        (security == WIFI_SERVICE_SECURITY_PSK &&
         (password_length < 8 || password_length > WIFI_SERVICE_PASSWORD_MAX_LEN))) {
        return ESP_ERR_WIFI_PASSWORD;
    }

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, ssid_length);
    memcpy(config.sta.password, password, password_length);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_wifi_started || s_wifi_transitioning || s_station_request || s_scan_in_progress ||
        s_provisioning_starting || s_provisioning_shutdown_worker != NULL ||
        s_state == WIFI_SERVICE_STATE_CONNECTING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_station_request = true;
    s_state = WIFI_SERVICE_STATE_CONNECTING;
    s_provisioning_connect_requested = s_provisioning_active;
    xSemaphoreGive(s_lock);

    set_pending_credentials(ssid, security, password);
    wifi_ap_record_t current_ap;
    const bool associated = esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK;
    if (associated) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        s_next_config = config;
        s_reconnect_pending = true;
        s_disconnect_requested = true;
        xSemaphoreGive(s_lock);
        /** Never set a new STA config while the old association is live. */
        const esp_err_t ret = esp_wifi_disconnect();
        if (ret == ESP_OK) {
            return ESP_OK; /** The disconnect event starts the replacement. */
        }
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE) {
            s_disconnect_requested = false;
            s_reconnect_pending = false;
            s_station_request = false;
            memset(&s_next_config, 0, sizeof(s_next_config));
            xSemaphoreGive(s_lock);
        }
        clear_pending_credentials();
        set_state(WIFI_SERVICE_STATE_FAILED);
        return ret;
    }

    wifi_connection_on_sta_disconnected(s_lock);
    const esp_err_t ret = start_station_connection(&config);
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE) {
        s_station_request = false;
        xSemaphoreGive(s_lock);
    }
    return ret;
}

esp_err_t wifi_service_disconnect(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_wifi_started || s_wifi_transitioning || s_station_request || s_provisioning_starting ||
        s_provisioning_shutdown_worker != NULL || s_state == WIFI_SERVICE_STATE_CONNECTING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_station_request = true;
    s_disconnect_requested = true;
    s_reconnect_pending = false;
    s_provisioning_connect_requested = false;
    xSemaphoreGive(s_lock);

    clear_pending_credentials();
    wifi_ap_record_t ap;
    const bool associated = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    const esp_err_t ret = esp_wifi_disconnect();
    if (ret == ESP_OK && !associated) {
        wifi_connection_on_sta_disconnected(s_lock);
        time_service_notify_network_lost();
        set_state(WIFI_SERVICE_STATE_IDLE);
        notify_link_listener(false);
    }
    /** An idle station does not emit a disconnect event. */
    if (ret != ESP_OK || !associated) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE) {
            s_disconnect_requested = false;
            s_station_request = false;
            xSemaphoreGive(s_lock);
        }
    }
    return ret;
}

esp_err_t wifi_service_start_provisioning(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_provisioning_active && !s_provisioning_starting &&
        s_provisioning_shutdown_worker == NULL && s_provisioning_qr_payload[0] != '\0') {
        const bool retry_failed_connection = s_provisioning_connect_requested &&
                                             s_state != WIFI_SERVICE_STATE_CONNECTING &&
                                             s_state != WIFI_SERVICE_STATE_CONNECTED &&
                                             !s_station_request;
        xSemaphoreGive(s_lock);
        if (retry_failed_connection) {
            ESP_RETURN_ON_ERROR(wifi_service_stop_provisioning(), TAG, "restart provisioning failed");
            return wifi_service_start_provisioning();
        }
        return ESP_OK;
    }
    const bool already_in_use = s_station_request || s_wifi_transitioning ||
                                s_state == WIFI_SERVICE_STATE_CONNECTING ||
                                s_provisioning_active || s_provisioning_starting ||
                                s_provisioning_shutdown_worker != NULL;
    if (!already_in_use) {
        s_provisioning_starting = true;
        s_provisioning_connect_requested = false;
    }
    xSemaphoreGive(s_lock);
    if (already_in_use) {
        return ESP_ERR_INVALID_STATE;
    }

    generate_provisioning_token();
    const wifi_provisioning_context_t context = provisioning_context();
    const esp_err_t ret = wifi_provisioning_start(&context);
    wifi_service_connection_info_t connection = {0};
    wifi_connection_get_info(s_lock, &connection);
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Wi-Fi provisioning completion lock timed out");
        return ESP_ERR_TIMEOUT;
    }
    s_provisioning_starting = false;
    if (s_state == WIFI_SERVICE_STATE_SCANNING && !s_scan_in_progress) {
        s_state = connection.connected ? WIFI_SERVICE_STATE_CONNECTED : WIFI_SERVICE_STATE_READY;
    }
    xSemaphoreGive(s_lock);
    if (ret != ESP_OK) {
        clear_provisioning_token();
    }
    return ret;
}

esp_err_t wifi_service_stop_provisioning(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const wifi_provisioning_context_t context = provisioning_context();
    return wifi_provisioning_stop(&context);
}

const char *wifi_service_get_provisioning_ssid(void)
{
    return s_provisioning_ssid;
}

const char *wifi_service_get_provisioning_password(void)
{
    return s_provisioning_password;
}

const char *wifi_service_get_provisioning_token(void)
{
    return s_provisioning_token;
}

const char *wifi_service_get_provisioning_qr_payload(void)
{
    return s_provisioning_qr_payload;
}
