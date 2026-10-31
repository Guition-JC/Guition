/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * @file
 * @brief HTTP access to device settings and standby media.
 */
#include "settings_server.h"
#include "standby_web.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "cJSON.h"
#include "display.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "ethernet_service.h"
#include "music_player_service.h"
#include "desktop_language.h"
#include "provisioning_server.h"
#include "weather_rules.h"
#include "weather_service.h"
#include "weather_web.h"
#include "wifi_service.h"

#define TAG "web-settings"

/**
 * @brief Handle captive-portal ownership of port 80.
 * @param active Whether the provisioning portal is active.
 * @param user_data Listener context.
 */
static void settings_server_note_provisioning(bool active, void *user_data);

/** @brief Maximum settings request body size, in bytes. */
#define SETTINGS_BODY_MAX (48U * 1024U)
#define SETTINGS_MAX_OPEN_SOCKETS 4
/** @brief HTTP task stack size, in bytes. */
#define SETTINGS_TASK_STACK 6144

extern const char settings_html_start[] asm("_binary_settings_html_start");
extern const char settings_html_end[] asm("_binary_settings_html_end");

static httpd_handle_t s_server;
static bool s_provisioning;

/**
 * @brief Copy a setting string with capacity-bounded NUL termination.
 * @param[out] destination Buffer receiving the string.
 * @param capacity Buffer size in bytes; must be nonzero.
 * @param source NUL-terminated string to copy.
 */
static void copy_setting(char *destination, size_t capacity, const char *source)
{
    const size_t length = strlen(source);
    const size_t copied = length < capacity - 1 ? length : capacity - 1;
    memcpy(destination, source, copied);
    destination[copied] = '\0';
}

/**
 * @brief Send a JSON response and release the supplied JSON object.
 * @param request HTTP request to answer.
 * @param root JSON object consumed by this function.
 * @param status Optional HTTP status string; NULL uses the default.
 * @return HTTP send result, including the error response if serialization fails.
 */
static esp_err_t send_json(httpd_req_t *request, cJSON *root, const char *status)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (status != NULL) {
        httpd_resp_set_status(request, status);
    }
    const esp_err_t ret = httpd_resp_sendstr(request, text);
    free(text);
    return ret;
}

static esp_err_t send_ok(httpd_req_t *request)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(request, root, NULL);
}

static esp_err_t send_error(httpd_req_t *request, const char *code)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "code", code);
    return send_json(request, root, "400 Bad Request");
}

/**
 * @brief Read and NUL-terminate the HTTP body within the supplied capacity.
 * @param request HTTP request containing the body.
 * @param[out] buffer Buffer receiving the body and terminator.
 * @param capacity Buffer size in bytes.
 * @return ESP_OK on success, ESP_ERR_INVALID_SIZE for an invalid length, or ESP_FAIL on receive failure.
 */
static esp_err_t read_body(httpd_req_t *request, char *buffer, size_t capacity)
{
    if (request->content_len <= 0 || (size_t)request->content_len >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t received = 0;
    while (received < (size_t)request->content_len) {
        const int chunk =
            httpd_req_recv(request, buffer + received, (size_t)request->content_len - received);
        if (chunk <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)chunk;
    }
    buffer[received] = '\0';
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, settings_html_start,
                           (ssize_t)(settings_html_end - settings_html_start));
}

static void add_weather_json(cJSON *parent)
{
    weather_config_t config;
    if (weather_service_get_config(&config) != ESP_OK) {
        weather_service_default_config(&config);
    }

    /** Expose the weather status-bar field mask. */
    cJSON *weather = cJSON_AddObjectToObject(parent, "weather");
    if (weather == NULL) {
        return;
    }
    cJSON_AddNumberToObject(weather, "fields", config.fields);
}

static esp_err_t settings_get_handler(httpd_req_t *request)
{
    uint8_t volume = 0;
    bool muted = false;
    (void)music_player_service_get_volume(&volume, &muted);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "language",
                            desktop_language_current() == DESKTOP_LANGUAGE_CHINESE ? "zh" : "en");
    cJSON_AddBoolToObject(root, "chinese_available", desktop_language_chinese_available());
    cJSON_AddNumberToObject(root, "brightness", display_backlight_get());
    cJSON_AddNumberToObject(root, "volume", volume);
    cJSON_AddBoolToObject(root, "muted", muted);
    add_weather_json(root);
    if (standby_web_add_settings(root) != ESP_OK) {
        cJSON_Delete(root);
        return send_error(request, "standby_unavailable");
    }
    return send_json(request, root, NULL);
}

static esp_err_t standby_media_get_handler(httpd_req_t *request)
{
    cJSON *root = standby_web_media();
    if (root == NULL) return send_error(request, "media_unavailable");
    return send_json(request, root, NULL);
}

/**
 * @brief Apply weather status-bar fields.
 * @return ESP_OK on success, ESP_FAIL after sending an error response.
 */
static esp_err_t apply_weather_fields(httpd_req_t *request, const cJSON *weather)
{
    const cJSON *fields = cJSON_GetObjectItem(weather, "fields");
    if (!cJSON_IsNumber(fields)) {
        return ESP_OK;
    }

    weather_config_t config;
    if (weather_service_get_config(&config) != ESP_OK) {
        (void)send_error(request, "weather_unavailable");
        return ESP_FAIL;
    }
    config.fields = (uint8_t)fields->valueint & WEATHER_FIELD_ALL;

    if (weather_service_set_config(&config) != ESP_OK) {
        (void)send_error(request, "weather_rejected");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief Validate and apply settings from the HTTP JSON request.
 * @param request HTTP request containing the settings.
 * @return Result of sending the response, or an error when handling fails.
 */
static esp_err_t settings_post_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len >= SETTINGS_BODY_MAX)
        return send_error(request, "invalid_body");
    const size_t capacity = (size_t)request->content_len + 1U;
    char *body = malloc(capacity);
    if (body == NULL) return send_error(request, "no_memory");
    if (read_body(request, body, capacity) != ESP_OK) {
        free(body);
        return send_error(request, "invalid_body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return send_error(request, "invalid_json");
    }
    const char *standby_error = standby_web_apply(root);
    if (standby_error != NULL) {
        cJSON_Delete(root);
        return send_error(request, standby_error);
    }

    /** Stop processing settings after the first error response. */
    bool applied = true;
    esp_err_t response = ESP_OK;

    const cJSON *language = cJSON_GetObjectItem(root, "language");
    if (applied && cJSON_IsString(language)) {
        const desktop_language_t wanted = strcmp(language->valuestring, "zh") == 0
                                            ? DESKTOP_LANGUAGE_CHINESE
                                            : DESKTOP_LANGUAGE_ENGLISH;
        if (wanted != desktop_language_current() && !desktop_language_request(wanted)) {
            response = send_error(request, "language_unavailable");
            applied = false;
        }
    }

    const cJSON *brightness = cJSON_GetObjectItem(root, "brightness");
    if (applied && cJSON_IsNumber(brightness)) {
        if (display_backlight_set((uint8_t)brightness->valueint) == ESP_OK) {
            (void)display_backlight_persist();
        }
    }

    const cJSON *volume = cJSON_GetObjectItem(root, "volume");
    if (applied && cJSON_IsNumber(volume)) {
        if (music_player_service_set_volume((uint8_t)volume->valueint) == ESP_OK) {
            (void)music_player_service_save_volume();
        }
    }
    const cJSON *muted = cJSON_GetObjectItem(root, "muted");
    if (applied && cJSON_IsBool(muted)) {
        if (music_player_service_set_mute(cJSON_IsTrue(muted)) == ESP_OK) {
            (void)music_player_service_save_volume();
        }
    }

    const cJSON *weather = cJSON_GetObjectItem(root, "weather");
    if (applied && cJSON_IsObject(weather)) {
        if (apply_weather_fields(request, weather) != ESP_OK) {
            applied = false;
        }
    }

    cJSON_Delete(root);
    if (!applied) {
        return response;
    }
    return send_ok(request);
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/", .method = HTTP_GET, .handler = root_get_handler},
    {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_handler},
    {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler},
    {.uri = "/api/standby/media", .method = HTTP_GET, .handler = standby_media_get_handler},
};

static void stop_server(void)
{
    if (s_server == NULL) {
        return;
    }
    (void)httpd_stop(s_server);
    s_server = NULL;
}

static esp_err_t start_server(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = SETTINGS_MAX_OPEN_SOCKETS;
    config.stack_size = SETTINGS_TASK_STACK;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    /** Listen on all interfaces for Wi-Fi and Ethernet clients. */
    config.if_name = NULL;

    const esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        s_server = NULL;
        ESP_LOGW(TAG, "settings server could not start: %s", esp_err_to_name(ret));
        return ret;
    }
    for (size_t i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); ++i) {
        const esp_err_t route_ret = httpd_register_uri_handler(s_server, &s_routes[i]);
        if (route_ret != ESP_OK) {
            ESP_LOGE(TAG, "route %s unavailable: %s", s_routes[i].uri,
                     esp_err_to_name(route_ret));
            stop_server();
            return route_ret;
        }
    }
    /** Register weather endpoints on the shared HTTP server. */
    const esp_err_t weather_ret = weather_web_register(s_server);
    if (weather_ret != ESP_OK) {
        ESP_LOGW(TAG, "weather page unavailable: %s", esp_err_to_name(weather_ret));
    }
    ESP_LOGI(TAG, "settings page ready at http://<device-address>/");
    return ESP_OK;
}

static bool station_has_address(void)
{
    if (wifi_service_get_state() == WIFI_SERVICE_STATE_CONNECTED) {
        return true;
    }
    ethernet_service_status_t status = {0};
    return ethernet_service_get_status(&status) == ESP_OK && status.ip_valid;
}

/**
 * @brief Start or stop HTTP serving as network address availability changes.
 * @param argument Event-handler context (unused).
 * @param base Event base.
 * @param event_id IP event identifier.
 * @param data Event data (unused).
 */
static void network_event(void *argument, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)argument;
    (void)data;
    if (base != IP_EVENT) {
        return;
    }
    if (event_id == IP_EVENT_STA_GOT_IP || event_id == IP_EVENT_ETH_GOT_IP) {
        if (!s_provisioning) {
            (void)start_server();
        }
        return;
    }
    if (event_id == IP_EVENT_STA_LOST_IP || event_id == IP_EVENT_ETH_LOST_IP) {
        if (!station_has_address()) {
            stop_server();
        }
    }
}

esp_err_t settings_server_init(void)
{
    const esp_err_t ret = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, network_event, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "IP event handler unavailable: %s", esp_err_to_name(ret));
    }
    /** Subscribe to captive-portal ownership changes for port 80. */
    (void)provisioning_server_register_state_listener(settings_server_note_provisioning, NULL);
    /** Start serving immediately if a network address is already available. */
    if (station_has_address() && !s_provisioning) {
        (void)start_server();
    }
    return ret;
}

static void settings_server_note_provisioning(bool active, void *user_data)
{
    (void)user_data;
    s_provisioning = active;
    if (active) {
        stop_server();
    } else if (station_has_address()) {
        (void)start_server();
    }
}

void settings_server_stop(void)
{
    stop_server();
}
