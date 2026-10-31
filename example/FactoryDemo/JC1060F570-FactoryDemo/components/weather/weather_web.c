/** SPDX-License-Identifier: GPL-2.0-only */
/** The weather page and its API. */
#include "weather_web.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "desktop_language.h"
#include "weather_rules.h"
#include "weather_service.h"

#define TAG "weather-web"

/** @brief The page posts one small object (the API key is the longest field). */
#define WEATHER_BODY_MAX 512

extern const char weather_html_start[] asm("_binary_weather_html_start");
extern const char weather_html_end[] asm("_binary_weather_html_end");

static void copy_setting(char *destination, size_t capacity, const char *source)
{
    const size_t length = strlen(source);
    const size_t copied = length < capacity - 1 ? length : capacity - 1;
    memcpy(destination, source, copied);
    destination[copied] = '\0';
}

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

static esp_err_t weather_page_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, weather_html_start,
                           (ssize_t)(weather_html_end - weather_html_start));
}

static void add_weather_json(cJSON *parent)
{
    weather_config_t config;
    weather_snapshot_t snapshot;
    if (weather_service_get_config(&config) != ESP_OK) {
        weather_service_default_config(&config);
    }
    if (weather_service_get_snapshot(&snapshot) != ESP_OK) {
        memset(&snapshot, 0, sizeof(snapshot));
    }

    /** Use the panel language to localize the web page. */
    cJSON_AddStringToObject(parent, "language",
                            desktop_language_current() == DESKTOP_LANGUAGE_CHINESE ? "zh" : "en");
    cJSON_AddBoolToObject(parent, "chinese_available", desktop_language_chinese_available());

    cJSON *weather = cJSON_AddObjectToObject(parent, "weather");
    if (weather == NULL) {
        return;
    }
    cJSON_AddStringToObject(weather, "provider", weather_provider_name(config.provider));
    cJSON_AddStringToObject(weather, "location", weather_location_mode_name(config.location_mode));
    cJSON_AddStringToObject(weather, "city", config.city);
    /** Expose only whether an API key is configured. */
    cJSON_AddBoolToObject(weather, "key_set", config.key[0] != '\0');
    cJSON_AddBoolToObject(weather, "valid", snapshot.valid);
    cJSON_AddStringToObject(weather, "condition", weather_condition_name(
                                                     (weather_condition_t)snapshot.condition));
    cJSON_AddStringToObject(weather, "wind",
                            weather_wind_dir_name((weather_wind_dir_t)snapshot.wind_direction));
    cJSON_AddStringToObject(weather, "place", snapshot.place);
    cJSON_AddNumberToObject(weather, "temp_c", snapshot.temp_c);
    cJSON_AddNumberToObject(weather, "humidity", snapshot.humidity);
    cJSON_AddNumberToObject(weather, "wind_dmps", snapshot.wind_dmps);
    cJSON_AddNumberToObject(weather, "updated", (double)snapshot.updated_epoch);
    cJSON_AddBoolToObject(weather, "error", snapshot.last_error != ESP_OK);
}

static esp_err_t weather_get_handler(httpd_req_t *request)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    add_weather_json(root);
    return send_json(request, root, NULL);
}

static esp_err_t weather_refresh_handler(httpd_req_t *request)
{
    weather_service_request_refresh();
    return send_ok(request);
}

/**
 * @brief ESP_OK means "applied"; ESP_FAIL means "a response was already sent".
 * @param request HTTP request used to send error responses.
 * @param weather JSON object containing the weather settings.
 * @return ESP_OK if applied, or ESP_FAIL if a response was already sent.
 */
static esp_err_t apply_weather_settings(httpd_req_t *request, const cJSON *weather)
{
    weather_config_t config;
    if (weather_service_get_config(&config) != ESP_OK) {
        (void)send_error(request, "weather_unavailable");
        return ESP_FAIL;
    }

    const cJSON *provider = cJSON_GetObjectItem(weather, "provider");
    const cJSON *location = cJSON_GetObjectItem(weather, "location");
    const cJSON *city = cJSON_GetObjectItem(weather, "city");
    const cJSON *key = cJSON_GetObjectItem(weather, "key");

    if (cJSON_IsString(provider)) {
        const uint8_t parsed = weather_provider_from_name(provider->valuestring);
        if (parsed >= WEATHER_PROVIDER_COUNT) {
            (void)send_error(request, "unknown_provider");
            return ESP_FAIL;
        }
        config.provider = parsed;
    }
    if (cJSON_IsString(location)) {
        const uint8_t parsed = weather_location_mode_from_name(location->valuestring);
        if (parsed >= WEATHER_LOCATION_COUNT) {
            (void)send_error(request, "unknown_location");
            return ESP_FAIL;
        }
        config.location_mode = parsed;
    }
    if (cJSON_IsString(city)) {
        copy_setting(config.city, sizeof(config.city), city->valuestring);
    }
    /** An empty API key preserves the stored key. */
    if (cJSON_IsString(key) && key->valuestring[0] != '\0') {
        copy_setting(config.key, sizeof(config.key), key->valuestring);
    }

    if (weather_service_set_config(&config) != ESP_OK) {
        (void)send_error(request, "weather_rejected");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t weather_post_handler(httpd_req_t *request)
{
    char body[WEATHER_BODY_MAX];
    if (read_body(request, body, sizeof(body)) != ESP_OK) {
        return send_error(request, "invalid_body");
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_error(request, "invalid_json");
    }

    const cJSON *weather = cJSON_GetObjectItem(root, "weather");
    esp_err_t result = ESP_OK;
    if (!cJSON_IsObject(weather)) {
        result = send_error(request, "invalid_body");
    } else if (apply_weather_settings(request, weather) != ESP_OK) {
        result = ESP_FAIL;
    } else {
        result = send_ok(request);
    }
    cJSON_Delete(root);
    return result;
}

esp_err_t weather_web_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/weather", .method = HTTP_GET, .handler = weather_page_get_handler},
        {.uri = "/api/weather", .method = HTTP_GET, .handler = weather_get_handler},
        {.uri = "/api/weather", .method = HTTP_POST, .handler = weather_post_handler},
        {.uri = "/api/weather/refresh", .method = HTTP_POST, .handler = weather_refresh_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        const esp_err_t ret = httpd_register_uri_handler(server, &routes[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "route %s unavailable: %s", routes[i].uri, esp_err_to_name(ret));
            return ret;
        }
    }
    return ESP_OK;
}
