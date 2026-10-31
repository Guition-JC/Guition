#include "wifi_credentials_nvs.h"

#include <string.h>

#include "nvs.h"

#define NVS_NAMESPACE "wifi_target"
#define NVS_KEY_VALID "valid"
#define NVS_KEY_VERSION "version"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"
#define NVS_KEY_SECURITY "security"
#define NVS_RECORD_VERSION 1U
#define NVS_RECORD_VALID 1U
#define NVS_RECORD_INVALID 0U

static bool credentials_are_valid(const wifi_credentials_t *credentials)
{
    if (credentials == NULL ||
        (credentials->security != WIFI_SERVICE_SECURITY_OPEN &&
         credentials->security != WIFI_SERVICE_SECURITY_PSK)) {
        return false;
    }

    const size_t ssid_length = strnlen(credentials->ssid, sizeof(credentials->ssid));
    const size_t password_length = strnlen(credentials->password, sizeof(credentials->password));
    if (ssid_length == 0 || ssid_length >= sizeof(credentials->ssid) ||
        password_length >= sizeof(credentials->password)) {
        return false;
    }

    return (credentials->security == WIFI_SERVICE_SECURITY_OPEN && password_length == 0) ||
           (credentials->security == WIFI_SERVICE_SECURITY_PSK && password_length >= 8 &&
            password_length <= WIFI_SERVICE_PASSWORD_MAX_LEN);
}

static esp_err_t read_bounded_string(nvs_handle_t handle, const char *key, char *value, size_t capacity)
{
    size_t required_length = 0;
    esp_err_t ret = nvs_get_str(handle, key, NULL, &required_length);
    if (ret != ESP_OK) {
        return ret;
    }
    if (required_length == 0 || required_length > capacity) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return nvs_get_str(handle, key, value, &required_length);
}

esp_err_t wifi_credentials_nvs_load(wifi_credentials_t *credentials, bool *found)
{
    if (credentials == NULL || found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(credentials, 0, sizeof(*credentials));
    *found = false;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t valid = NVS_RECORD_INVALID;
    ret = nvs_get_u8(handle, NVS_KEY_VALID, &valid);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
        goto cleanup;
    }
    if (ret != ESP_OK) {
        goto cleanup;
    }
    if (valid == NVS_RECORD_INVALID) {
        ret = ESP_OK;
        goto cleanup;
    }
    if (valid != NVS_RECORD_VALID) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    uint8_t version = 0;
    uint8_t security = 0;
    ret = nvs_get_u8(handle, NVS_KEY_VERSION, &version);
    if (ret != ESP_OK) {
        ret = ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_INVALID_RESPONSE : ret;
        goto cleanup;
    }
    ret = nvs_get_u8(handle, NVS_KEY_SECURITY, &security);
    if (ret != ESP_OK) {
        ret = ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_INVALID_RESPONSE : ret;
        goto cleanup;
    }
    if (version != NVS_RECORD_VERSION ||
        (security != WIFI_SERVICE_SECURITY_OPEN && security != WIFI_SERVICE_SECURITY_PSK)) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    ret = read_bounded_string(handle, NVS_KEY_SSID, credentials->ssid, sizeof(credentials->ssid));
    if (ret != ESP_OK) {
        ret = ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_INVALID_RESPONSE : ret;
        goto cleanup;
    }
    ret = read_bounded_string(handle, NVS_KEY_PASSWORD, credentials->password,
                              sizeof(credentials->password));
    if (ret != ESP_OK) {
        ret = ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_INVALID_RESPONSE : ret;
        goto cleanup;
    }

    credentials->security = (wifi_service_security_t)security;
    if (!credentials_are_valid(credentials)) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    *found = true;

cleanup:
    nvs_close(handle);
    if (ret != ESP_OK) {
        memset(credentials, 0, sizeof(*credentials));
    }
    return ret;
}

esp_err_t wifi_credentials_nvs_save(const wifi_credentials_t *credentials)
{
    if (!credentials_are_valid(credentials)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(handle, NVS_KEY_VALID, NVS_RECORD_INVALID);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, NVS_KEY_VERSION, NVS_RECORD_VERSION);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, NVS_KEY_SSID, credentials->ssid);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, NVS_KEY_PASSWORD, credentials->password);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, NVS_KEY_SECURITY, (uint8_t)credentials->security);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, NVS_KEY_VALID, NVS_RECORD_VALID);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t wifi_credentials_nvs_clear(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_all(handle);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}
