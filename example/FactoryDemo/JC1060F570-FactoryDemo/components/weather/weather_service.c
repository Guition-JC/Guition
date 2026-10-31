/** SPDX-License-Identifier: GPL-2.0-only */
#include "weather_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define WEATHER_NVS_NAMESPACE "weather"
#define WEATHER_NVS_PROVIDER "provider"
#define WEATHER_NVS_LOCATION "locmode"
#define WEATHER_NVS_FIELDS "fields"
#define WEATHER_NVS_KEY "key"
#define WEATHER_NVS_CITY "city"

/** @brief Automatic weather refresh interval, in milliseconds. */
#define WEATHER_REFRESH_PERIOD_MS (60 * 60 * 1000)
/** @brief Weather HTTP request timeout, in milliseconds. */
#define WEATHER_HTTP_TIMEOUT_MS 12000
#define WEATHER_HTTP_ATTEMPTS 2
#define WEATHER_HTTP_RETRY_DELAY_MS 500
#define WEATHER_BODY_CAPACITY 8192
/** @brief "q=" plus a fully encoded place and the separators around it. */
#define WEATHER_QUERY_CAPACITY (WEATHER_SERVICE_PLACE_MAX * 3 + 16)
#define WEATHER_URL_CAPACITY 576
/** @brief Weather worker stack size in bytes; allocate from internal RAM for TLS. */
#define WEATHER_WORKER_STACK 16384
#define WEATHER_WORKER_PRIORITY 5
/** @brief Coordinates as well as the name: the name is what the bar draws, the pair is what the weather request is built from. */
#define WEATHER_IP_API_URL "http://ip-api.com/json/?fields=status,city,countryCode,lat,lon"
/** @brief OpenWeather's own answer names the city in Latin script; its built-in geocoding API carries the localised names beside it. */
#define WEATHER_GEOCODE_URL "https://api.openweathermap.org/geo/1.0/direct?q=%s&limit=1&appid=%s"

static const char *TAG = "weather";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_worker;
static weather_service_listener_t s_listener;
static void *s_listener_context;
static weather_config_t s_config;
static weather_snapshot_t s_snapshot;
static bool s_initialized;
/** Which language the providers are asked in. Follows the panel, not a setting. */
static bool s_chinese = true;

static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (destination == NULL || capacity == 0) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    const size_t length = strlen(source);
    const size_t copied = length < capacity - 1 ? length : capacity - 1;
    memcpy(destination, source, copied);
    destination[copied] = '\0';
}

static int round_to_int(double value)
{
    return (int)(value + (value < 0 ? -0.5 : 0.5));
}

/**
 * @brief Percent-encode everything a URL cannot carry literally; ',' stays readable so the
 * OpenWeather "city,country" pair survives the trip.
 */
static void url_encode(const char *input, char *output, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    if (output == NULL || capacity == 0) {
        return;
    }
    for (const unsigned char *cursor = (const unsigned char *)input;
         cursor != NULL && *cursor != '\0' && used + 4 < capacity; ++cursor) {
        const unsigned char character = *cursor;
        const bool literal = (character >= 'A' && character <= 'Z') ||
                             (character >= 'a' && character <= 'z') ||
                             (character >= '0' && character <= '9') || character == '-' ||
                             character == '_' || character == '.' || character == '~' ||
                             character == ',';
        if (literal) {
            output[used++] = (char)character;
        } else {
            output[used++] = '%';
            output[used++] = hex[character >> 4];
            output[used++] = hex[character & 0x0F];
        }
    }
    output[used] = '\0';
}

/**
 * @brief "https://host/path?query" -> "host/path": logs must never carry the API key.
 * @param url URL to sanitize.
 * @param [out] output Destination buffer for the endpoint.
 * @param capacity Destination buffer capacity in bytes.
 */
static void url_endpoint(const char *url, char *output, size_t capacity)
{
    const char *start = strstr(url, "://");
    start = start != NULL ? start + 3 : url;
    const char *end = strchr(start, '?');
    const size_t length = end != NULL ? (size_t)(end - start) : strlen(start);
    const size_t copied = length < capacity - 1 ? length : capacity - 1;
    memcpy(output, start, copied);
    output[copied] = '\0';
}

static esp_err_t prefs_open(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    const esp_err_t ret = nvs_open(WEATHER_NVS_NAMESPACE, mode, handle);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "open namespace '%s' failed: %s", WEATHER_NVS_NAMESPACE,
                 esp_err_to_name(ret));
    }
    return ret;
}

static void store_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle = 0;
    if (prefs_open(NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (nvs_set_u8(handle, key, value) == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
}

static uint8_t load_u8(const char *key, uint8_t fallback)
{
    nvs_handle_t handle = 0;
    uint8_t value = fallback;
    if (prefs_open(NVS_READONLY, &handle) != ESP_OK) {
        return fallback;
    }
    if (nvs_get_u8(handle, key, &value) != ESP_OK) {
        value = fallback;
    }
    nvs_close(handle);
    return value;
}

static void store_text(const char *key, const char *value)
{
    nvs_handle_t handle = 0;
    if (prefs_open(NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (nvs_set_str(handle, key, value != NULL ? value : "") == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
}

static void load_text(const char *key, char *value, size_t capacity)
{
    nvs_handle_t handle = 0;
    size_t length = capacity;
    if (value == NULL || capacity == 0) {
        return;
    }
    value[0] = '\0';
    if (prefs_open(NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    if (nvs_get_str(handle, key, value, &length) != ESP_OK) {
        value[0] = '\0';
    }
    nvs_close(handle);
}

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool overflow;
} body_sink_t;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    body_sink_t *sink = (body_sink_t *)event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || sink == NULL || event->data_len <= 0) {
        return ESP_OK;
    }
    if (sink->length + (size_t)event->data_len + 1 > sink->capacity) {
        sink->overflow = true;
        return ESP_FAIL;
    }
    memcpy(sink->buffer + sink->length, event->data, (size_t)event->data_len);
    sink->length += (size_t)event->data_len;
    sink->buffer[sink->length] = '\0';
    return ESP_OK;
}

/** Fetch `url` into a heap buffer the caller frees. */
/**
 * @brief One attempt. The caller owns the buffer and retries; a failure only reports.
 * @param url URL to request.
 * @param [in,out] sink Destination for response body data.
 * @param [out] status_out Receives the HTTP status code after the request.
 * @return ESP_OK on successful transport, otherwise an HTTP client error.
 */
static esp_err_t http_get_attempt(const char *url, body_sink_t *sink, int *status_out)
{
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = WEATHER_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = sink,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 3,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t ret = esp_http_client_perform(client);
    *status_out = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return ret;
}

/**
 * @brief True for the failures a second attempt can clear: no connection yet, or a connection that stalled before the body was ready.
 * @param ret HTTP client result to inspect.
 * @return true for connection errors that may clear on retry.
 */
static bool http_retriable(esp_err_t ret)
{
    return ret == ESP_ERR_HTTP_CONNECT || ret == ESP_ERR_HTTP_EAGAIN;
}

static esp_err_t http_get(const char *url, char **body_out, size_t *length_out)
{
    char *body = heap_caps_malloc(WEATHER_BODY_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        body = malloc(WEATHER_BODY_CAPACITY);
    }
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /** Log the host and path without the API key. */
    char endpoint[96];
    url_endpoint(url, endpoint, sizeof(endpoint));

    for (int attempt = 1; attempt <= WEATHER_HTTP_ATTEMPTS; ++attempt) {
        body_sink_t sink = {
            .buffer = body,
            .capacity = WEATHER_BODY_CAPACITY,
        };
        body[0] = '\0';

        int status = 0;
        const esp_err_t ret = http_get_attempt(url, &sink, &status);
        if (ret == ESP_OK && status == 200 && !sink.overflow && sink.length != 0) {
            *body_out = body;
            if (length_out != NULL) {
                *length_out = sink.length;
            }
            return ESP_OK;
        }

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "GET %s failed (attempt %d/%d): %s", endpoint, attempt,
                     WEATHER_HTTP_ATTEMPTS, esp_err_to_name(ret));
        } else {
            ESP_LOGW(TAG, "GET %s answered status=%d overflow=%d bytes=%u body=%.160s", endpoint,
                     status, (int)sink.overflow, (unsigned)sink.length, body);
        }

        if (attempt < WEATHER_HTTP_ATTEMPTS && (ret == ESP_OK ? false : http_retriable(ret))) {
            vTaskDelay(pdMS_TO_TICKS(WEATHER_HTTP_RETRY_DELAY_MS));
            continue;
        }
        free(body);
        if (ret != ESP_OK) {
            return ret;
        }
        return status == 200 ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_RESPONSE;
    }
    free(body);
    return ESP_FAIL;
}

static uint8_t clamp_u8(int value, uint8_t maximum)
{
    if (value < 0) {
        return 0;
    }
    return (uint8_t)(value > (int)maximum ? maximum : value);
}

/**
 * @brief Parse numeric fields supplied as JSON numbers or numeric strings.
 * @param item JSON value to inspect.
 * @return true for a number or nonempty string.
 */
static bool json_value_present(const cJSON *item)
{
    if (cJSON_IsNumber(item)) {
        return true;
    }
    return cJSON_IsString(item) && item->valuestring[0] != '\0';
}

static double json_value_double(const cJSON *item)
{
    return cJSON_IsNumber(item) ? item->valuedouble : atof(item->valuestring);
}

/**
 * @brief Copy a string field; numbers are rendered so single-value fields still work.
 * @param [out] destination Buffer receiving the text.
 * @param capacity Destination capacity in bytes.
 * @param item JSON value to convert.
 */
static void copy_json_text(char *destination, size_t capacity, const cJSON *item)
{
    if (cJSON_IsString(item)) {
        copy_text(destination, capacity, item->valuestring);
    } else if (cJSON_IsNumber(item)) {
        snprintf(destination, capacity, "%g", item->valuedouble);
    } else {
        destination[0] = '\0';
    }
}

/**
 * @brief How a field arrived, for the diagnostics that only run when parsing failed.
 * @param item JSON value to inspect.
 * @return Static description of the value state.
 */
static const char *json_state(const cJSON *item)
{
    if (item == NULL) {
        return "missing";
    }
    if (cJSON_IsString(item)) {
        return item->valuestring[0] != '\0' ? "string" : "empty";
    }
    if (cJSON_IsNumber(item)) {
        return "number";
    }
    return "other";
}

/**
 * @brief Prefer the localized geocoding result over the provider place name.
 * @param body JSON response body.
 * @param [out] reading Receives the parsed weather snapshot.
 * @param preferred_place Localized place name to prefer, if available.
 * @return ESP_OK on success, otherwise a parsing error.
 */
static esp_err_t parse_openweather(const char *body, weather_snapshot_t *reading,
                                   const char *preferred_place)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *weather = cJSON_GetObjectItem(root, "weather");
    const cJSON *condition = cJSON_IsArray(weather) ? cJSON_GetArrayItem(weather, 0) : NULL;
    const cJSON *condition_id = condition != NULL ? cJSON_GetObjectItem(condition, "id") : NULL;
    const cJSON *main = cJSON_GetObjectItem(root, "main");
    const cJSON *temperature = main != NULL ? cJSON_GetObjectItem(main, "temp") : NULL;
    const cJSON *humidity = main != NULL ? cJSON_GetObjectItem(main, "humidity") : NULL;
    const cJSON *wind = cJSON_GetObjectItem(root, "wind");
    const cJSON *wind_speed = wind != NULL ? cJSON_GetObjectItem(wind, "speed") : NULL;
    const cJSON *wind_degrees = wind != NULL ? cJSON_GetObjectItem(wind, "deg") : NULL;
    const cJSON *place = cJSON_GetObjectItem(root, "name");

    esp_err_t ret = ESP_ERR_INVALID_RESPONSE;
    if (json_value_present(condition_id) && json_value_present(temperature)) {
        reading->condition =
            (uint8_t)weather_condition_from_openweather((int)json_value_double(condition_id));
        reading->temp_c = (int16_t)round_to_int(json_value_double(temperature));
        reading->humidity = json_value_present(humidity)
                                ? (int16_t)clamp_u8((int)json_value_double(humidity), 100)
                                : -1;
        reading->wind_dmps = json_value_present(wind_speed)
                                 ? (int16_t)weather_wind_dmps_from_mps_milli(
                                       round_to_int(json_value_double(wind_speed) * 1000.0))
                                 : -1;
        reading->wind_direction =
            json_value_present(wind_degrees)
                ? (uint8_t)weather_wind_dir_from_degrees((int)json_value_double(wind_degrees))
                : (uint8_t)WEATHER_WIND_NORTH;
        copy_json_text(reading->place, sizeof(reading->place), place);
        if (preferred_place != NULL) {
            copy_text(reading->place, sizeof(reading->place), preferred_place);
        }
        ret = ESP_OK;
    }
    cJSON_Delete(root);
    return ret;
}

static esp_err_t parse_seniverse(const char *body, weather_snapshot_t *reading)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    /** Seniverse answers {"results":[{"location":{...},"now":{...}}]} or an error object with status/status_code and no results at all. */
    const cJSON *results = cJSON_GetObjectItem(root, "results");
    const cJSON *first = cJSON_IsArray(results) ? cJSON_GetArrayItem(results, 0) : NULL;
    const cJSON *now = first != NULL ? cJSON_GetObjectItem(first, "now") : NULL;
    const cJSON *location = first != NULL ? cJSON_GetObjectItem(first, "location") : NULL;

    esp_err_t ret = ESP_ERR_INVALID_RESPONSE;
    if (cJSON_IsObject(now)) {
        const cJSON *text = cJSON_GetObjectItem(now, "text");
        const cJSON *code = cJSON_GetObjectItem(now, "code");
        const cJSON *temperature = cJSON_GetObjectItem(now, "temperature");
        const cJSON *humidity = cJSON_GetObjectItem(now, "humidity");
        const cJSON *wind_direction = cJSON_GetObjectItem(now, "wind_direction");
        const cJSON *wind_speed = cJSON_GetObjectItem(now, "wind_speed");

        char wording[32];
        copy_json_text(wording, sizeof(wording), text);
        weather_condition_t condition = weather_condition_from_text(wording);
        if (condition == WEATHER_CONDITION_UNKNOWN && json_value_present(code)) {
            condition = weather_condition_from_seniverse((int)json_value_double(code));
        }

        /** Only the temperature is required: a plan that omits humidity or wind still produces a reading, just a shorter bar. */
        if (json_value_present(temperature)) {
            reading->condition = (uint8_t)condition;
            reading->temp_c = (int16_t)round_to_int(json_value_double(temperature));
            reading->humidity =
                json_value_present(humidity)
                    ? (int16_t)clamp_u8((int)json_value_double(humidity), 100)
                    : -1;
            reading->wind_dmps =
                json_value_present(wind_speed)
                    ? (int16_t)weather_wind_dmps_from_kmh(round_to_int(json_value_double(wind_speed)))
                    : -1;
            char direction_text[32];
            copy_json_text(direction_text, sizeof(direction_text), wind_direction);
            reading->wind_direction = direction_text[0] != '\0'
                                          ? (uint8_t)weather_wind_dir_from_text(direction_text)
                                          : (uint8_t)WEATHER_WIND_NORTH;
            const cJSON *place = location != NULL ? cJSON_GetObjectItem(location, "name") : NULL;
            copy_json_text(reading->place, sizeof(reading->place), place);
            ret = ESP_OK;
        } else {
            ESP_LOGW(TAG,
                     "seniverse now lacks a temperature (text=%.12s code=%s humidity=%s "
                     "wind_speed=%s)",
                     wording, json_state(code), json_state(humidity), json_state(wind_speed));
        }
    }
    cJSON_Delete(root);
    return ret;
}

/**
 * @brief OpenWeather has no IP lookup of its own, so the IP mode asks a third-party city database.
 * @param [out] place Buffer receiving the display place.
 * @param place_capacity Display place buffer capacity in bytes.
 * @param [out] query Buffer receiving the weather query.
 * @param query_capacity Query buffer capacity in bytes.
 * @return ESP_OK on success, otherwise a lookup error.
 */
static esp_err_t resolve_openweather_ip(char *place, size_t place_capacity, char *query,
                                        size_t query_capacity)
{
    char url[160];
    snprintf(url, sizeof(url), WEATHER_IP_API_URL "&lang=%s", s_chinese ? "zh-CN" : "en");
    place[0] = '\0';
    query[0] = '\0';

    char *body = NULL;
    const esp_err_t ret = http_get(url, &body, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        free(body);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *status = cJSON_GetObjectItem(root, "status");
    const cJSON *city = cJSON_GetObjectItem(root, "city");
    const cJSON *country = cJSON_GetObjectItem(root, "countryCode");
    const cJSON *latitude = cJSON_GetObjectItem(root, "lat");
    const cJSON *longitude = cJSON_GetObjectItem(root, "lon");
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    if (cJSON_IsString(status) && strcmp(status->valuestring, "success") == 0) {
        if (cJSON_IsString(city)) {
            copy_text(place, place_capacity, city->valuestring);
        }
        if (cJSON_IsNumber(latitude) && cJSON_IsNumber(longitude)) {
            snprintf(query, query_capacity, "lat=%.4f&lon=%.4f", latitude->valuedouble,
                     longitude->valuedouble);
        } else if (cJSON_IsString(city) && cJSON_IsString(country)) {
            char encoded[WEATHER_SERVICE_PLACE_MAX * 3 + 4];
            char joined[WEATHER_SERVICE_PLACE_MAX + 8];
            snprintf(joined, sizeof(joined), "%s,%s", city->valuestring, country->valuestring);
            url_encode(joined, encoded, sizeof(encoded));
            snprintf(query, query_capacity, "q=%s", encoded);
        }
        result = query[0] != '\0' ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(root);
    free(body);
    return result;
}

/**
 * @brief Resolve localized place names and the canonical city/country query.
 * @param key API key for the geocoding request.
 * @param typed City name supplied by the user.
 * @param [out] place Buffer receiving the display place.
 * @param place_capacity Display place buffer capacity in bytes.
 * @param [out] query Buffer receiving the canonical query.
 * @param query_capacity Query buffer capacity in bytes.
 * @return ESP_OK on success, otherwise a lookup error.
 */
static esp_err_t resolve_openweather_city(const char *key, const char *typed, char *place,
                                          size_t place_capacity, char *query,
                                          size_t query_capacity)
{
    char encoded_key[WEATHER_SERVICE_KEY_MAX * 3 + 1];
    char encoded_city[WEATHER_SERVICE_PLACE_MAX * 3 + 1];
    char url[WEATHER_URL_CAPACITY];
    url_encode(key, encoded_key, sizeof(encoded_key));
    url_encode(typed, encoded_city, sizeof(encoded_city));
    snprintf(url, sizeof(url), WEATHER_GEOCODE_URL, encoded_city, encoded_key);
    place[0] = '\0';
    query[0] = '\0';

    char *body = NULL;
    const esp_err_t ret = http_get(url, &body, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        free(body);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *first = cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : NULL;
    const cJSON *local_names = first != NULL ? cJSON_GetObjectItem(first, "local_names") : NULL;
    const cJSON *localised_item =
        local_names != NULL
            ? cJSON_GetObjectItem(local_names, s_chinese ? "zh" : "en")
            : NULL;
    const cJSON *name = first != NULL ? cJSON_GetObjectItem(first, "name") : NULL;
    const cJSON *country = first != NULL ? cJSON_GetObjectItem(first, "country") : NULL;
    const cJSON *latitude = first != NULL ? cJSON_GetObjectItem(first, "lat") : NULL;
    const cJSON *longitude = first != NULL ? cJSON_GetObjectItem(first, "lon") : NULL;

    esp_err_t result = ESP_ERR_NOT_FOUND;
    if (cJSON_IsNumber(latitude) && cJSON_IsNumber(longitude)) {
        snprintf(query, query_capacity, "lat=%.4f&lon=%.4f", latitude->valuedouble,
                 longitude->valuedouble);
        result = ESP_OK;
    } else if (cJSON_IsString(name)) {
        char encoded[WEATHER_SERVICE_PLACE_MAX * 3 + 4];
        if (cJSON_IsString(country) && country->valuestring[0] != '\0') {
            char joined[WEATHER_SERVICE_PLACE_MAX + 8];
            snprintf(joined, sizeof(joined), "%s,%s", name->valuestring, country->valuestring);
            url_encode(joined, encoded, sizeof(encoded));
        } else {
            url_encode(name->valuestring, encoded, sizeof(encoded));
        }
        snprintf(query, query_capacity, "q=%s", encoded);
        result = ESP_OK;
    }
    if (cJSON_IsString(localised_item)) {
        /** The localised name is what the bar draws when the language has one. */
        copy_text(place, place_capacity, localised_item->valuestring);
    } else if (cJSON_IsString(name)) {
        copy_text(place, place_capacity, name->valuestring);
    }
    cJSON_Delete(root);
    free(body);
    return result;
}

static esp_err_t weather_fetch(const weather_config_t *config, weather_snapshot_t *reading)
{
    /** Every buffer below is sized so the longest possible URL is still provably inside `url`: three bytes per encoded input byte, plus the fixed template. */
    char location[WEATHER_SERVICE_PLACE_MAX + 1];
    char encoded[WEATHER_SERVICE_PLACE_MAX * 3 + 1];
    char query[WEATHER_QUERY_CAPACITY];
    char url[WEATHER_URL_CAPACITY];
    char *body = NULL;
    /** The name the bar draws, when a lookup answered a better one than the provider's own Latin spelling. */
    char preferred_place[WEATHER_SERVICE_PLACE_MAX + 1];
    const bool seniverse = config->provider == WEATHER_PROVIDER_SENIVERSE;

    location[0] = '\0';
    query[0] = '\0';
    preferred_place[0] = '\0';

    if (config->location_mode == WEATHER_LOCATION_IP) {
        if (seniverse) {
            /** Seniverse resolves the caller's public address itself. */
            copy_text(location, sizeof(location), "ip");
        } else {
            const esp_err_t ret = resolve_openweather_ip(preferred_place, sizeof(preferred_place),
                                                         query, sizeof(query));
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "IP location lookup failed: %s", esp_err_to_name(ret));
                return ret;
            }
        }
    } else {
        copy_text(location, sizeof(location), config->city);
        if (!seniverse &&
            resolve_openweather_city(config->key, location, preferred_place,
                                     sizeof(preferred_place), query, sizeof(query)) != ESP_OK) {
            /** A miss only costs the position: the typed string is queried as it is, so a lookup that cannot answer never blocks the weather. */
            url_encode(location, encoded, sizeof(encoded));
            snprintf(query, sizeof(query), "q=%s", encoded);
            ESP_LOGW(TAG, "city lookup for '%s' failed; querying with it as it is", location);
        }
    }

    char key[WEATHER_SERVICE_KEY_MAX * 3 + 1];
    url_encode(config->key, key, sizeof(key));
    if (seniverse) {
        if (location[0] == '\0') {
            return ESP_ERR_INVALID_STATE;
        }
        url_encode(location, encoded, sizeof(encoded));
        snprintf(url, sizeof(url),
                 "https://api.seniverse.com/v3/weather/now.json?key=%s&location=%s"
                 "&language=%s&unit=c",
                 key, encoded, s_chinese ? "zh-Hans" : "en");
    } else {
        if (query[0] == '\0') {
            return ESP_ERR_INVALID_STATE;
        }
        /** Built from the position when there is one, so a localised name can never be the thing the weather endpoint has to match. */
        snprintf(url, sizeof(url),
                 "https://api.openweathermap.org/data/2.5/weather?%s&appid=%s&units=metric"
                 "&lang=%s",
                 query, key, s_chinese ? "zh_cn" : "en");
    }

    size_t body_length = 0;
    const esp_err_t ret = http_get(url, &body, &body_length);
    if (ret != ESP_OK) {
        return ret;
    }
    const esp_err_t parse_ret = config->provider == WEATHER_PROVIDER_SENIVERSE
                                    ? parse_seniverse(body, reading)
                                    : parse_openweather(body, reading,
                                                        preferred_place[0] != '\0'
                                                            ? preferred_place
                                                            : NULL);
    if (parse_ret != ESP_OK) {

        ESP_LOGW(TAG, "%s body did not parse (%u bytes): %.400s", weather_provider_name(config->provider),
                 (unsigned)body_length, body);
    }
    free(body);
    return parse_ret;
}

/**
 * @brief Notify the listener after updating the in-memory weather snapshot.
 */
static void notify_listener(void)
{
    weather_service_listener_t listener = NULL;
    void *context = NULL;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        listener = s_listener;
        context = s_listener_context;
        xSemaphoreGive(s_lock);
    }
    if (listener != NULL) {
        listener(context);
    }
}

/**
 * @brief Apply the outcome of one refresh.
 * @param result Refresh result to apply.
 * @param reading Weather snapshot produced by the refresh.
 */
static void weather_apply_result(esp_err_t result, const weather_snapshot_t *reading)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (result == ESP_OK && reading != NULL) {
        s_snapshot = *reading;
        s_snapshot.valid = true;
        s_snapshot.last_error = ESP_OK;
    } else {
        s_snapshot.last_error = result;
    }
    xSemaphoreGive(s_lock);
    notify_listener();
}

static void weather_refresh_now(void)
{
    weather_config_t config;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    config = s_config;
    xSemaphoreGive(s_lock);

    if (config.key[0] == '\0' || config.provider >= WEATHER_PROVIDER_COUNT) {
        ESP_LOGI(TAG, "no provider key stored; nothing to refresh");
        weather_apply_result(ESP_ERR_INVALID_STATE, NULL);
        return;
    }

    weather_snapshot_t reading = {0};
    reading.updated_epoch = (int64_t)time(NULL);
    const esp_err_t ret = weather_fetch(&config, &reading);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "updated: %s %d C %u%% %s %d.%d m/s (%s)",
                 weather_condition_name((weather_condition_t)reading.condition), reading.temp_c,
                 reading.humidity, weather_wind_dir_name((weather_wind_dir_t)reading.wind_direction),
                 reading.wind_dmps / 10, reading.wind_dmps % 10, reading.place);
    } else {
        ESP_LOGW(TAG, "refresh failed: %s", esp_err_to_name(ret));
    }
    weather_apply_result(ret, ret == ESP_OK ? &reading : NULL);
}

static void weather_worker(void *argument)
{
    (void)argument;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WEATHER_REFRESH_PERIOD_MS));
        weather_refresh_now();
        /** The stack is sized for the TLS handshake, which is the deepest call in here; this line is what proves the headroom on a real board. */
        ESP_LOGI(TAG, "worker stack headroom: %u bytes",
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    }
}

static void network_event(void *argument, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)argument;
    (void)data;
    if (base == IP_EVENT &&
        (event_id == IP_EVENT_STA_GOT_IP || event_id == IP_EVENT_ETH_GOT_IP)) {
        ESP_LOGI(TAG, "network is up; refreshing");
        weather_service_request_refresh();
    }
}

void weather_service_default_config(weather_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->provider = WEATHER_PROVIDER_OPENWEATHER;
    config->location_mode = WEATHER_LOCATION_IP;
    config->fields = WEATHER_FIELD_ALL;
}

esp_err_t weather_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    weather_service_default_config(&s_config);
    s_config.provider = load_u8(WEATHER_NVS_PROVIDER, WEATHER_PROVIDER_OPENWEATHER);
    s_config.location_mode = load_u8(WEATHER_NVS_LOCATION, WEATHER_LOCATION_IP);
    s_config.fields = load_u8(WEATHER_NVS_FIELDS, WEATHER_FIELD_ALL);
    if (s_config.provider >= WEATHER_PROVIDER_COUNT) {
        s_config.provider = WEATHER_PROVIDER_OPENWEATHER;
    }
    if (s_config.location_mode >= WEATHER_LOCATION_COUNT) {
        s_config.location_mode = WEATHER_LOCATION_IP;
    }
    s_config.fields &= WEATHER_FIELD_ALL;
    load_text(WEATHER_NVS_KEY, s_config.key, sizeof(s_config.key));
    load_text(WEATHER_NVS_CITY, s_config.city, sizeof(s_config.city));

    /** The reading starts empty: the first fetch lands as soon as the link has an address, and the bar hides itself until then. */
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.condition = WEATHER_CONDITION_UNKNOWN;
    s_snapshot.wind_direction = WEATHER_WIND_NORTH;
    /** Nothing reported yet: an absent humidity or wind must not print a zero. */
    s_snapshot.humidity = -1;
    s_snapshot.wind_dmps = -1;

    const esp_err_t event_ret =
        esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, network_event, NULL);
    if (event_ret != ESP_OK) {
        ESP_LOGW(TAG, "IP event handler unavailable: %s", esp_err_to_name(event_ret));
    }

    /** Internal RAM on purpose; see WEATHER_WORKER_STACK above. */
    if (xTaskCreate(weather_worker, "weather", WEATHER_WORKER_STACK, NULL, WEATHER_WORKER_PRIORITY,
                    &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "refresh worker could not start");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "ready: provider=%s location=%s fields=0x%02x key=%s",
             weather_provider_name(s_config.provider), weather_location_mode_name(s_config.location_mode),
             s_config.fields, s_config.key[0] != '\0' ? "set" : "unset");
    return ESP_OK;
}

esp_err_t weather_service_get_config(weather_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    *config = s_config;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t weather_service_set_config(const weather_config_t *config)
{
    if (config == NULL || config->provider >= WEATHER_PROVIDER_COUNT ||
        config->location_mode >= WEATHER_LOCATION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_config = *config;
    s_config.fields &= WEATHER_FIELD_ALL;
    xSemaphoreGive(s_lock);

    store_u8(WEATHER_NVS_PROVIDER, s_config.provider);
    store_u8(WEATHER_NVS_LOCATION, s_config.location_mode);
    store_u8(WEATHER_NVS_FIELDS, s_config.fields);
    store_text(WEATHER_NVS_KEY, s_config.key);
    store_text(WEATHER_NVS_CITY, s_config.city);
    weather_service_request_refresh();
    return ESP_OK;
}

esp_err_t weather_service_get_snapshot(weather_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    *snapshot = s_snapshot;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void weather_service_request_refresh(void)
{
    if (s_worker != NULL) {
        xTaskNotifyGive(s_worker);
    }
}

void weather_service_set_language(bool chinese)
{
    if (s_chinese == chinese) {
        return;
    }
    s_chinese = chinese;
    ESP_LOGI(TAG, "providers will answer in %s from now on", chinese ? "Chinese" : "English");
    /** The place name already on the bar was answered in the previous language. */
    weather_service_request_refresh();
}

esp_err_t weather_service_register_listener(weather_service_listener_t listener, void *context)
{
    if (!s_initialized || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_listener = listener;
    s_listener_context = context;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

const char *weather_provider_name(uint8_t provider)
{
    return provider == WEATHER_PROVIDER_SENIVERSE ? "seniverse" : "openweather";
}

const char *weather_location_mode_name(uint8_t mode)
{
    return mode == WEATHER_LOCATION_CITY ? "city" : "ip";
}

uint8_t weather_provider_from_name(const char *name)
{
    if (name == NULL) {
        return WEATHER_PROVIDER_COUNT;
    }
    if (strcmp(name, "openweather") == 0) {
        return WEATHER_PROVIDER_OPENWEATHER;
    }
    if (strcmp(name, "seniverse") == 0) {
        return WEATHER_PROVIDER_SENIVERSE;
    }
    return WEATHER_PROVIDER_COUNT;
}

uint8_t weather_location_mode_from_name(const char *name)
{
    if (name == NULL) {
        return WEATHER_LOCATION_COUNT;
    }
    if (strcmp(name, "ip") == 0) {
        return WEATHER_LOCATION_IP;
    }
    if (strcmp(name, "city") == 0) {
        return WEATHER_LOCATION_CITY;
    }
    return WEATHER_LOCATION_COUNT;
}
