#include "provisioning_server.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_netif_ip_addr.h"
#include "net/if.h"
#include "desktop_language.h"
#include "wifi_service.h"

#define FORM_BUFFER_SIZE 512
#define PROVISIONING_FAILURE_RATE_LIMIT_US 500000LL
#define PROVISIONING_TOKEN_LOCK_TIMEOUT_MS 2000U
#define PROVISIONING_FAILURE_WINDOW_US 30000000LL
#define PROVISIONING_FAILURE_LOCKOUT_COUNT 5

extern const char provisioning_html_start[] asm("_binary_provisioning_html_start");
extern const char provisioning_html_end[] asm("_binary_provisioning_html_end");

static const char *TAG = "provisioning_http";
static httpd_handle_t s_server;
static char s_token[WIFI_SERVICE_PROVISIONING_TOKEN_LENGTH + 1];
static bool s_token_valid;
static bool s_token_consumed;
static bool s_token_reserved;
static int64_t s_last_failure_us;
static int64_t s_failure_window_start_us;
static int64_t s_lockout_until_us;
static unsigned int s_failure_count;
static StaticSemaphore_t s_token_lock_storage;
static SemaphoreHandle_t s_token_lock;
static provisioning_server_state_listener_t s_state_listener;
static void *s_state_listener_arg;

static void notify_state(bool active)
{
    if (s_state_listener != NULL) {
        s_state_listener(active, s_state_listener_arg);
    }
}

esp_err_t provisioning_server_register_state_listener(provisioning_server_state_listener_t listener,
                                                      void *user_data)
{
    s_state_listener = listener;
    s_state_listener_arg = listener != NULL ? user_data : NULL;
    return ESP_OK;
}

static bool ensure_token_lock(void)
{
    if (s_token_lock == NULL) {
        s_token_lock = xSemaphoreCreateMutexStatic(&s_token_lock_storage);
    }
    return s_token_lock != NULL;
}

static void token_clear_locked(void)
{
    memset(s_token, 0, sizeof(s_token));
    s_token_valid = false;
    s_token_consumed = false;
    s_token_reserved = false;
    s_last_failure_us = 0;
    s_failure_window_start_us = 0;
    s_lockout_until_us = 0;
    s_failure_count = 0;
}

static void token_clear(void)
{
    if (!ensure_token_lock()) {
        return;
    }
    if (xSemaphoreTake(s_token_lock, pdMS_TO_TICKS(PROVISIONING_TOKEN_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    token_clear_locked();
    xSemaphoreGive(s_token_lock);
}

static esp_err_t token_begin_session(const char *token)
{
    if (token == NULL || strlen(token) != WIFI_SERVICE_PROVISIONING_TOKEN_LENGTH || !ensure_token_lock()) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_token_lock, pdMS_TO_TICKS(PROVISIONING_TOKEN_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(s_token, token, sizeof(s_token));
    s_token_valid = true;
    s_token_consumed = false;
    s_token_reserved = false;
    s_last_failure_us = 0;
    s_failure_window_start_us = 0;
    s_lockout_until_us = 0;
    s_failure_count = 0;
    xSemaphoreGive(s_token_lock);
    return ESP_OK;
}

typedef enum {
    TOKEN_MATCH,
    TOKEN_MISSING_OR_WRONG,
    TOKEN_CONSUMED,
} token_match_result_t;

static token_match_result_t token_matches_and_reserve(const char *candidate)
{
    if (candidate == NULL || strlen(candidate) != WIFI_SERVICE_PROVISIONING_TOKEN_LENGTH ||
        !ensure_token_lock()) {
        return TOKEN_MISSING_OR_WRONG;
    }

    if (xSemaphoreTake(s_token_lock, pdMS_TO_TICKS(PROVISIONING_TOKEN_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return TOKEN_MISSING_OR_WRONG;
    }
    if (s_token_consumed) {
        xSemaphoreGive(s_token_lock);
        return TOKEN_CONSUMED;
    }
    unsigned int difference = s_token_valid ? 0U : 1U;
    for (size_t index = 0; index < WIFI_SERVICE_PROVISIONING_TOKEN_LENGTH; ++index) {
        difference |= (unsigned int)((unsigned char)s_token[index] ^ (unsigned char)candidate[index]);
    }
    if (difference != 0U || s_token_reserved) {
        xSemaphoreGive(s_token_lock);
        return TOKEN_MISSING_OR_WRONG;
    }
    s_token_reserved = true;
    xSemaphoreGive(s_token_lock);
    return TOKEN_MATCH;
}

static void token_release_reservation(void)
{
    if (!ensure_token_lock()) {
        return;
    }
    if (xSemaphoreTake(s_token_lock, pdMS_TO_TICKS(PROVISIONING_TOKEN_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    s_token_reserved = false;
    xSemaphoreGive(s_token_lock);
}

static void token_consume_reservation(void)
{
    if (!ensure_token_lock()) {
        return;
    }
    if (xSemaphoreTake(s_token_lock, pdMS_TO_TICKS(PROVISIONING_TOKEN_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    if (s_token_valid && s_token_reserved) {
        memset(s_token, 0, sizeof(s_token));
        s_token_valid = false;
        s_token_reserved = false;
        s_token_consumed = true;
        s_last_failure_us = 0;
        s_failure_window_start_us = 0;
        s_lockout_until_us = 0;
        s_failure_count = 0;
    }
    xSemaphoreGive(s_token_lock);
}

static bool token_copy(char *destination, size_t capacity)
{
    if (destination == NULL || capacity <= WIFI_SERVICE_PROVISIONING_TOKEN_LENGTH || !ensure_token_lock()) {
        return false;
    }

    bool valid;
    if (xSemaphoreTake(s_token_lock, pdMS_TO_TICKS(PROVISIONING_TOKEN_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    valid = s_token_valid;
    if (valid) {
        memcpy(destination, s_token, sizeof(s_token));
    }
    xSemaphoreGive(s_token_lock);
    return valid;
}

typedef enum {
    TOKEN_ATTEMPT_ALLOWED,
    TOKEN_ATTEMPT_RATE_LIMITED,
    TOKEN_ATTEMPT_LOCKED_OUT,
} token_attempt_result_t;

static token_attempt_result_t token_admit_attempt(void)
{
    if (!ensure_token_lock()) {
        return TOKEN_ATTEMPT_LOCKED_OUT;
    }
    const int64_t now_us = esp_timer_get_time();
    if (xSemaphoreTake(s_token_lock, pdMS_TO_TICKS(PROVISIONING_TOKEN_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return TOKEN_ATTEMPT_LOCKED_OUT;
    }
    if (s_lockout_until_us > now_us) {
        xSemaphoreGive(s_token_lock);
        return TOKEN_ATTEMPT_LOCKED_OUT;
    }
    if (s_lockout_until_us != 0) {
        s_lockout_until_us = 0;
        s_failure_count = 0;
        s_failure_window_start_us = now_us;
    }
    const bool rate_limited = s_last_failure_us != 0 &&
                              now_us - s_last_failure_us < PROVISIONING_FAILURE_RATE_LIMIT_US;
    xSemaphoreGive(s_token_lock);
    return rate_limited ? TOKEN_ATTEMPT_RATE_LIMITED : TOKEN_ATTEMPT_ALLOWED;
}

static void token_record_failure(void)
{
    if (!ensure_token_lock()) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    if (xSemaphoreTake(s_token_lock, pdMS_TO_TICKS(PROVISIONING_TOKEN_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    if (s_failure_window_start_us == 0 ||
        now_us - s_failure_window_start_us >= PROVISIONING_FAILURE_WINDOW_US) {
        s_failure_window_start_us = now_us;
        s_failure_count = 0;
    }
    s_last_failure_us = now_us;
    ++s_failure_count;
    if (s_failure_count >= PROVISIONING_FAILURE_LOCKOUT_COUNT) {
        s_lockout_until_us = now_us + PROVISIONING_FAILURE_WINDOW_US;
    }
    xSemaphoreGive(s_token_lock);
}

void provisioning_server_invalidate_token(void)
{
    token_clear();
}

static esp_err_t send_invalid_token(httpd_req_t *request)
{
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_status(request, "401 Unauthorized");
    return httpd_resp_sendstr(request, "{\"ok\":false,\"code\":\"invalid_token\",\"message\":\"invalid or expired provisioning token\"}");
}

static esp_err_t send_consumed_token(httpd_req_t *request)
{
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_status(request, "409 Conflict");
    return httpd_resp_sendstr(request, "{\"ok\":false,\"code\":\"token_consumed\",\"message\":\"provisioning token has already been consumed\"}");
}

static esp_err_t send_token_throttle(httpd_req_t *request, token_attempt_result_t result)
{
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_status(request, "429 Too Many Requests");
    if (result == TOKEN_ATTEMPT_LOCKED_OUT) {
        return httpd_resp_sendstr(request, "{\"ok\":false,\"code\":\"locked_out\",\"message\":\"too many failed provisioning attempts; try again later\"}");
    }
    return httpd_resp_sendstr(request, "{\"ok\":false,\"code\":\"rate_limited\",\"message\":\"please wait before retrying provisioning\"}");
}

static const char *find_embedded_placeholder(const char *begin, const char *end,
                                             const char *placeholder)
{
    const size_t placeholder_length = strlen(placeholder);
    if (placeholder_length == 0 || end < begin || (size_t)(end - begin) < placeholder_length) {
        return NULL;
    }
    for (const char *cursor = begin; cursor <= end - placeholder_length; ++cursor) {
        if (memcmp(cursor, placeholder, placeholder_length) == 0) {
            return cursor;
        }
    }
    return NULL;
}

static esp_err_t root_get_handler(httpd_req_t *request)
{
    char token[sizeof(s_token)] = {0};
    if (!token_copy(token, sizeof(token))) {
        return send_invalid_token(request);
    }

    const char *token_placeholder = "__PROVISIONING_TOKEN__";
    const char *language_placeholder = "__PROVISIONING_LANGUAGE__";
    const char *token_start = find_embedded_placeholder(provisioning_html_start,
                                                        provisioning_html_end,
                                                        token_placeholder);
    const char *language_start = token_start == NULL ? NULL
        : find_embedded_placeholder(token_start + strlen(token_placeholder),
                                    provisioning_html_end, language_placeholder);
    if (language_start == NULL) {
        memset(token, 0, sizeof(token));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "provisioning page is invalid");
    }

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send_chunk(request, provisioning_html_start,
                                          token_start - provisioning_html_start);
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(request, token, WIFI_SERVICE_PROVISIONING_TOKEN_LENGTH);
    }
    if (ret == ESP_OK) {
        const char *after_token = token_start + strlen(token_placeholder);
        ret = httpd_resp_send_chunk(request, after_token, language_start - after_token);
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_sendstr_chunk(request,
                                      desktop_language_current() == DESKTOP_LANGUAGE_CHINESE ? "zh" : "en");
    }
    if (ret == ESP_OK) {
        const char *after_language = language_start + strlen(language_placeholder);
        ret = httpd_resp_send_chunk(request, after_language,
                                    provisioning_html_end - after_language);
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(request, NULL, 0);
    }
    memset(token, 0, sizeof(token));
    return ret;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool url_decode(const char *encoded, size_t encoded_length, char *value, size_t capacity)
{
    size_t decoded_length = 0;
    for (size_t index = 0; index < encoded_length; ++index) {
        unsigned char decoded;
        if (encoded[index] == '+') {
            decoded = ' ';
        } else if (encoded[index] == '%') {
            if (index + 2 >= encoded_length) {
                return false;
            }
            const int high = hex_value(encoded[index + 1]);
            const int low = hex_value(encoded[index + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            decoded = (unsigned char)((high << 4) | low);
            index += 2;
        } else {
            decoded = (unsigned char)encoded[index];
        }
        if (decoded == '\0' || decoded_length + 1 >= capacity) {
            return false;
        }
        value[decoded_length++] = (char)decoded;
    }
    value[decoded_length] = '\0';
    return true;
}

static bool form_value(const char *body, const char *name, char *value, size_t capacity)
{
    const size_t name_length = strlen(name);
    const char *cursor = body;
    while (*cursor != '\0') {
        if (strncmp(cursor, name, name_length) == 0 && cursor[name_length] == '=') {
            cursor += name_length + 1;
            const char *end = strchr(cursor, '&');
            const size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
            return url_decode(cursor, length, value, capacity);
        }
        cursor = strchr(cursor, '&');
        if (cursor == NULL) {
            break;
        }
        ++cursor;
    }
    return false;
}

static bool form_has_field(const char *body, const char *name)
{
    const size_t name_length = strlen(name);
    const char *cursor = body;
    while (*cursor != '\0') {
        if (strncmp(cursor, name, name_length) == 0 && cursor[name_length] == '=') {
            return true;
        }
        cursor = strchr(cursor, '&');
        if (cursor == NULL) {
            break;
        }
        ++cursor;
    }
    return false;
}

static esp_err_t wifi_post_handler(httpd_req_t *request)
{
    char body[FORM_BUFFER_SIZE];
    if (request->content_len <= 0 || request->content_len >= sizeof(body)) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid request size");
        return ESP_OK;
    }
    size_t received = 0;
    while (received < request->content_len) {
        const int chunk = httpd_req_recv(request, body + received, request->content_len - received);
        if (chunk <= 0) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "incomplete request");
            return ESP_OK;
        }
        received += (size_t)chunk;
    }
    body[received] = '\0';

    char token[WIFI_SERVICE_PROVISIONING_TOKEN_LENGTH + 1];
    char ssid[33];
    char security[8];
    char password[WIFI_SERVICE_PASSWORD_MAX_LEN + 1];
    const token_attempt_result_t admission = token_admit_attempt();
    if (admission != TOKEN_ATTEMPT_ALLOWED) {
        return send_token_throttle(request, admission);
    }
    if (!form_value(body, "token", token, sizeof(token))) {
        token_record_failure();
        return send_invalid_token(request);
    }
    const token_match_result_t token_result = token_matches_and_reserve(token);
    if (token_result == TOKEN_CONSUMED) {
        memset(token, 0, sizeof(token));
        return send_consumed_token(request);
    }
    if (token_result != TOKEN_MATCH) {
        token_record_failure();
        memset(token, 0, sizeof(token));
        return send_invalid_token(request);
    }
    if (!form_value(body, "ssid", ssid, sizeof(ssid)) || !form_value(body, "security", security, sizeof(security))) {
        token_release_reservation();
        memset(token, 0, sizeof(token));
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "ssid and security are required");
        return ESP_OK;
    }
    wifi_service_security_t security_mode;
    if (strcmp(security, "open") == 0) {
        security_mode = WIFI_SERVICE_SECURITY_OPEN;
    } else if (strcmp(security, "psk") == 0) {
        security_mode = WIFI_SERVICE_SECURITY_PSK;
    } else {
        token_release_reservation();
        memset(token, 0, sizeof(token));
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid security");
        return ESP_OK;
    }
    const bool password_present = form_has_field(body, "password");
    if (!form_value(body, "password", password, sizeof(password))) {
        if (security_mode == WIFI_SERVICE_SECURITY_OPEN && !password_present) {
            password[0] = '\0';
        } else {
            token_release_reservation();
            memset(token, 0, sizeof(token));
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                password_present ? "invalid password encoding" : "password is required");
            return ESP_OK;
        }
    }
    const esp_err_t ret = wifi_service_connect(ssid, security_mode, password);
    if (ret == ESP_ERR_WIFI_PASSWORD) {
        token_release_reservation();
        memset(token, 0, sizeof(token));
        httpd_resp_set_status(request, "422 Unprocessable Entity");
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_sendstr(request,
                                  "{\"ok\":false,\"code\":\"invalid_password\",\"message\":\"protected Wi-Fi passwords must contain 8 to 63 bytes\"}");
    }
    if (ret != ESP_OK) {
        token_release_reservation();
        memset(token, 0, sizeof(token));
        httpd_resp_set_status(request, "422 Unprocessable Entity");
        httpd_resp_set_type(request, "application/json");
        httpd_resp_sendstr(request, "{\"ok\":false,\"message\":\"unable to start connection\"}");
        return ESP_OK;
    }

    token_consume_reservation();
    memset(token, 0, sizeof(token));

    httpd_resp_set_type(request, "application/json");
    httpd_resp_sendstr(request, "{\"ok\":true,\"message\":\"connection started\"}");
    return ESP_OK;
}

static bool status_state_payload(wifi_service_state_t state, const char **name,
                                 bool *terminal, const char **message)
{
    switch (state) {
    case WIFI_SERVICE_STATE_IDLE:
        *name = "idle";
        *terminal = false;
        *message = "waiting for Wi-Fi configuration";
        return true;
    case WIFI_SERVICE_STATE_SCANNING:
        *name = "scanning";
        *terminal = false;
        *message = "scanning for Wi-Fi networks";
        return true;
    case WIFI_SERVICE_STATE_READY:
        *name = "ready";
        *terminal = false;
        *message = "ready for Wi-Fi configuration";
        return true;
    case WIFI_SERVICE_STATE_CONNECTING:
        *name = "connecting";
        *terminal = false;
        *message = "connecting to Wi-Fi";
        return true;
    case WIFI_SERVICE_STATE_CONNECTED:
        *name = "connected";
        *terminal = true;
        *message = "Wi-Fi connected";
        return true;
    case WIFI_SERVICE_STATE_FAILED:
        *name = "failed";
        *terminal = true;
        *message = "Wi-Fi connection failed";
        return true;
    default:
        return false;
    }
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    const char *name = NULL;
    const char *message = NULL;
    bool terminal = false;
    if (!status_state_payload(wifi_service_get_state(), &name, &terminal, &message)) {
        httpd_resp_set_status(request, "500 Internal Server Error");
        httpd_resp_set_type(request, "application/json");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        return httpd_resp_sendstr(request,
                                  "{\"ok\":false,\"code\":\"unknown_state\",\"message\":\"unknown Wi-Fi service state\"}");
    }

    char response[192];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"state\":\"%s\",\"terminal\":%s,\"message\":\"%s\"}",
             name, terminal ? "true" : "false", message);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, response);
}

/**
 * @brief SSIDs are untrusted bytes. Escape JSON syntax and all control characters.
 * @param ssid SSID to escape.
 * @param [out] output Buffer receiving the escaped JSON string contents.
 */
static void escape_ssid_json(const char *ssid, char *output)
{
    static const char hex[] = "0123456789abcdef";
    for (const unsigned char *p = (const unsigned char *)ssid; *p != 0; ++p) {
        if (*p == '"' || *p == '\\') {
            *output++ = '\\';
            *output++ = (char)*p;
        } else if (*p < 0x20) {
            *output++ = '\\';
            *output++ = 'u';
            *output++ = '0';
            *output++ = '0';
            *output++ = hex[*p >> 4];
            *output++ = hex[*p & 0xf];
        } else {
            *output++ = (char)*p;
        }
    }
    *output = '\0';
}

static esp_err_t networks_get_handler(httpd_req_t *request)
{
    char query[32] = {0};
    char refresh[4] = {0};
    const bool rescan = httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "refresh", refresh, sizeof(refresh)) == ESP_OK &&
        strcmp(refresh, "1") == 0;
    wifi_service_ap_t *aps = calloc(WIFI_SERVICE_MAX_APS, sizeof(*aps));
    if (aps == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    bool scanning = false;
    size_t count = wifi_service_get_scan_snapshot(aps, WIFI_SERVICE_MAX_APS, &scanning);
    if (rescan && !scanning) {
        const esp_err_t ret = wifi_service_start_scan();
        count = wifi_service_get_scan_snapshot(aps, WIFI_SERVICE_MAX_APS, &scanning);
        if (ret != ESP_OK && !scanning) {
            free(aps);
            httpd_resp_set_status(request, "409 Conflict");
            httpd_resp_set_type(request, "application/json");
            httpd_resp_set_hdr(request, "Cache-Control", "no-store");
            return httpd_resp_sendstr(request,
                "{\"ok\":false,\"message\":\"Wi-Fi busy; retry scan later\"}");
        }
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_sendstr_chunk(request, scanning
        ? "{\"ok\":true,\"scanning\":true,\"networks\":["
        : "{\"ok\":true,\"scanning\":false,\"networks\":[");
    bool first = true;
    for (size_t i = 0; ret == ESP_OK && i < count; ++i) {
        if (aps[i].ssid[0] == '\0') {
            continue; /** Hidden SSIDs need the explicit manual-entry option. */
        }
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(aps[i].ssid, aps[j].ssid) == 0 && aps[i].authmode == aps[j].authmode) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        char escaped[WIFI_SERVICE_SSID_MAX_LEN * 6 + 1];
        char item[320];
        escape_ssid_json(aps[i].ssid, escaped);
        snprintf(item, sizeof(item),
                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"security\":\"%s\",\"authmode\":%d}",
                 first ? "" : ",", escaped, aps[i].rssi,
                 aps[i].authmode == WIFI_AUTH_OPEN ? "open" : "psk", (int)aps[i].authmode);
        ret = httpd_resp_sendstr_chunk(request, item);
        first = false;
    }
    free(aps);
    if (ret == ESP_OK) {
        ret = httpd_resp_sendstr_chunk(request, "]}");
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_sendstr_chunk(request, NULL);
    }
    return ret;
}

static esp_err_t configure_captive_portal_dhcp(esp_netif_t *ap_netif)
{
    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(ap_netif, &ip_info);
    if (ret != ESP_OK) {
        return ret;
    }
    static char portal_uri[sizeof("http://255.255.255.255/")];
    snprintf(portal_uri, sizeof(portal_uri), "http://" IPSTR "/", IP2STR(&ip_info.ip));

    ret = esp_netif_dhcps_stop(ap_netif);
    if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return ret;
    }
    ret = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                 (void *)portal_uri, strlen(portal_uri));
    if (ret != ESP_OK) {
        esp_netif_dhcps_start(ap_netif);
        return ret;
    }
    ret = esp_netif_dhcps_start(ap_netif);
    if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        return ret;
    }
    return ESP_OK;
}

static esp_err_t not_found_handler(httpd_req_t *request, httpd_err_code_t error)
{
    (void)error;
    return root_get_handler(request);
}

esp_err_t provisioning_server_start(esp_netif_t *ap_netif, const char *token)
{
    /** Notify listeners before taking ownership of port 80. */
    notify_state(true);
    if (ap_netif == NULL || token == NULL || strlen(token) != WIFI_SERVICE_PROVISIONING_TOKEN_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    struct ifreq if_name = {0};
    esp_err_t ret = esp_netif_get_netif_impl_name(ap_netif, if_name.ifr_name);
    if (ret != ESP_OK || if_name.ifr_name[0] == '\0') {
        provisioning_server_invalidate_token();
        ESP_LOGE(TAG, "obtain SoftAP interface name failed: %s", esp_err_to_name(ret));
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }

    if (!ensure_token_lock()) {
        return ESP_ERR_NO_MEM;
    }
    provisioning_server_invalidate_token();
    ret = configure_captive_portal_dhcp(ap_netif);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "configure captive portal DHCP failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = token_begin_session(token);
    if (ret != ESP_OK) {
        provisioning_server_invalidate_token();
        ESP_LOGE(TAG, "create provisioning token failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 6;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (strncmp(if_name.ifr_name, "lo", 2) == 0) {
        /** Skip binding the portal when the AP interface is loopback. */
        ESP_LOGW(TAG, "SoftAP netif reported loopback %s; HTTP server will listen on all interfaces",
                 if_name.ifr_name);
        config.if_name = NULL;
    } else {
        config.if_name = &if_name;
    }
    ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        provisioning_server_invalidate_token();
        ESP_LOGE(TAG, "start HTTP server failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t wifi_post = {
        .uri = "/api/wifi",
        .method = HTTP_POST,
        .handler = wifi_post_handler,
    };
    const httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    const httpd_uri_t networks = {
        .uri = "/api/networks",
        .method = HTTP_GET,
        .handler = networks_get_handler,
    };
    const httpd_uri_t portal = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    ret = httpd_register_uri_handler(s_server, &root);
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(s_server, &wifi_post);
    }
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(s_server, &status);
    }
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(s_server, &networks);
    }
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(s_server, &portal);
    }
    if (ret == ESP_OK) {
        ret = httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_handler);
    }
    if (ret != ESP_OK) {
        (void)httpd_stop(s_server);
        s_server = NULL;
        provisioning_server_invalidate_token();
        ESP_LOGE(TAG, "register provisioning HTTP routes failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_netif_ip_info_t ip_info;
    ret = esp_netif_get_ip_info(ap_netif, &ip_info);
    if (ret != ESP_OK) {
        (void)httpd_stop(s_server);
        s_server = NULL;
        provisioning_server_invalidate_token();
        ESP_LOGE(TAG, "obtain SoftAP IPv4 address for portal log failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "provisioning portal started at http://" IPSTR "/", IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t provisioning_server_stop(void)
{
    provisioning_server_invalidate_token();
    /** Port 80 is free again even when there was nothing to stop. */
    notify_state(false);
    if (s_server == NULL) {
        return ESP_OK;
    }
    const esp_err_t ret = httpd_stop(s_server);
    if (ret == ESP_OK) {
        s_server = NULL;
    }
    return ret;
}
