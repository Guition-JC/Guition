/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "app_clock_prefs.h"

#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"

#define TAG "ClockPrefs"
#define CLOCK_NVS_NAMESPACE "clock_ui"
#define CLOCK_NVS_KEY_STYLE "style"
#define CLOCK_NVS_KEY_HINT "hint"

static esp_err_t clock_prefs_open(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    const esp_err_t ret = nvs_open(CLOCK_NVS_NAMESPACE, mode, handle);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "open namespace '%s' failed: %s", CLOCK_NVS_NAMESPACE,
                 esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief Read a byte and normalize an absent preference to ESP_ERR_NOT_FOUND.
 * @param key NVS key to read.
 * @param[out] value Receives the stored byte on success.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if absent, or an NVS error.
 */
static esp_err_t clock_prefs_read_u8(const char *key, uint8_t *value)
{
    nvs_handle_t handle = 0;
    const esp_err_t ret = clock_prefs_open(NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    const esp_err_t read = nvs_get_u8(handle, key, value);
    nvs_close(handle);
    return read == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : read;
}

/**
 * @brief Write and commit a byte before closing the NVS handle.
 * @param key NVS key to write.
 * @param value Byte to persist.
 * @return ESP_OK on success or an NVS error.
 */
static esp_err_t clock_prefs_write_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = clock_prefs_open(NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(handle, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "store %s failed: %s", key, esp_err_to_name(ret));
    }
    return ret;
}

bool app_clock_style_is_valid(app_clock_style_t style)
{
    return style == APP_CLOCK_STYLE_ANALOG || style == APP_CLOCK_STYLE_FLIP ||
           style == APP_CLOCK_STYLE_SEVEN_SEGMENT || style == APP_CLOCK_STYLE_IMAGE;
}

/**
 * @brief Use the default renderer for unsupported stored style values.
 * @param[out] style Receives the loaded or default style.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if absent, or another error.
 */
esp_err_t app_clock_prefs_load_style(app_clock_style_t *style)
{
    if (style == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t stored = 0U;
    const esp_err_t ret = clock_prefs_read_u8(CLOCK_NVS_KEY_STYLE, &stored);
    if (ret != ESP_OK) {
        return ret;
    }

    *style = app_clock_style_is_valid((app_clock_style_t)stored)
                 ? (app_clock_style_t)stored
                 : APP_CLOCK_STYLE_DEFAULT;
    return ESP_OK;
}

esp_err_t app_clock_prefs_save_style(app_clock_style_t style)
{
    if (!app_clock_style_is_valid(style)) {
        return ESP_ERR_INVALID_ARG;
    }
    return clock_prefs_write_u8(CLOCK_NVS_KEY_STYLE, (uint8_t)style);
}

esp_err_t app_clock_prefs_hint_seen(bool *seen)
{
    if (seen == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t stored = 0U;
    const esp_err_t ret = clock_prefs_read_u8(CLOCK_NVS_KEY_HINT, &stored);
    if (ret == ESP_ERR_NOT_FOUND) {
        *seen = false;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    *seen = stored != 0U;
    return ESP_OK;
}

esp_err_t app_clock_prefs_mark_hint_seen(void)
{
    return clock_prefs_write_u8(CLOCK_NVS_KEY_HINT, 1U);
}

esp_err_t app_clock_prefs_load_face(char *id, size_t capacity)
{
    if (!id || !capacity) return ESP_ERR_INVALID_ARG;
    id[0] = '\0';
    nvs_handle_t handle;
    esp_err_t ret = clock_prefs_open(NVS_READONLY, &handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_get_str(handle, "face", id, &capacity);
    nvs_close(handle);
    return ret;
}

esp_err_t app_clock_prefs_save_face(const char *id)
{
    if (!id || !*id) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t ret = clock_prefs_open(NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_str(handle, "face", id);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}
