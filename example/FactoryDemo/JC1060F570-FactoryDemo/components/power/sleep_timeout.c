/** SPDX-License-Identifier: GPL-2.0-only */
#include "sleep_timeout.h"

#include "esp_log.h"
#include "nvs.h"

#include "power_idle.h"

#define TAG "SleepTimeout"
#define NVS_NAMESPACE "power_sleep"
#define NVS_KEY_TIMEOUT "timeout_s"

/** Effective value for this boot; starts at the documented one-minute default. */
static uint32_t s_timeout_seconds = POWER_IDLE_DEFAULT_TIMEOUT_SECONDS;

static esp_err_t open_namespace(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    const esp_err_t ret = nvs_open(NVS_NAMESPACE, mode, handle);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "open namespace '%s' failed: %s", NVS_NAMESPACE, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t sleep_timeout_restore(void)
{
    /** The default is always in effect; a missing or unusable preference is not an error the caller has to handle. */
    s_timeout_seconds = POWER_IDLE_DEFAULT_TIMEOUT_SECONDS;

    nvs_handle_t handle = 0;
    const esp_err_t ret = open_namespace(NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t stored = 0U;
    const esp_err_t read_ret = nvs_get_u16(handle, NVS_KEY_TIMEOUT, &stored);
    nvs_close(handle);
    if (read_ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (read_ret != ESP_OK) {
        return read_ret;
    }
    if (!power_idle_timeout_supported((uint32_t)stored)) {
        ESP_LOGW(TAG, "stored timeout %u s is not a supported step; using %u s",
                 (unsigned)stored, (unsigned)POWER_IDLE_DEFAULT_TIMEOUT_SECONDS);
        return ESP_OK;
    }
    s_timeout_seconds = (uint32_t)stored;
    return ESP_OK;
}

uint32_t sleep_timeout_get(void)
{
    return s_timeout_seconds;
}

esp_err_t sleep_timeout_set(uint32_t timeout_seconds)
{
    const uint32_t sanitized = power_idle_sanitize_timeout(timeout_seconds);

    nvs_handle_t handle = 0;
    esp_err_t ret = open_namespace(NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u16(handle, NVS_KEY_TIMEOUT, (uint16_t)sanitized);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "storing timeout %u s failed: %s", (unsigned)sanitized,
                 esp_err_to_name(ret));
    } else {
        s_timeout_seconds = sanitized;
    }
    return ret;
}
