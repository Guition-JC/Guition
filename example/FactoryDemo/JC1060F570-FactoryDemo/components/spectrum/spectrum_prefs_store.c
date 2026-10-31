#include "spectrum_prefs_store.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"

#define TAG "SpectrumPrefsStore"
#define SPECTRUM_NVS_NAMESPACE "spectrum"
#define SPECTRUM_NVS_KEY_GROUP "group"
#define SPECTRUM_NVS_KEY_STYLE "style"
#define SPECTRUM_NVS_KEY_HINT "hint"
#define SPECTRUM_NVS_KEY_LINES "lines"

static esp_err_t spectrum_prefs_open(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    const esp_err_t ret = nvs_open(SPECTRUM_NVS_NAMESPACE, mode, handle);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "open namespace '%s' failed: %s", SPECTRUM_NVS_NAMESPACE,
                 esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief Callers only need to tell "nothing stored" from a real failure.
 * @param key NVS key to read.
 * @param [out] value Receives the stored value.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if absent, or an NVS error.
 */
static esp_err_t spectrum_prefs_read_u8(const char *key, uint8_t *value)
{
    nvs_handle_t handle = 0;
    const esp_err_t ret = spectrum_prefs_open(NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    const esp_err_t read = nvs_get_u8(handle, key, value);
    nvs_close(handle);
    return read == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : read;
}

static esp_err_t spectrum_prefs_write_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = spectrum_prefs_open(NVS_READWRITE, &handle);
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

esp_err_t spectrum_prefs_load_group(spectrum_group_t *group)
{
    if (group == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *group = SPECTRUM_GROUP_LINEAR;

    uint8_t stored = 0U;
    const esp_err_t read = spectrum_prefs_read_u8(SPECTRUM_NVS_KEY_GROUP, &stored);
    if (read != ESP_OK) {
        return read;
    }
    /** Fall back to linear grouping for unsupported values. */
    *group = stored < (uint8_t)SPECTRUM_GROUP_COUNT ? (spectrum_group_t)stored
                                                   : SPECTRUM_GROUP_LINEAR;
    return ESP_OK;
}

esp_err_t spectrum_prefs_save_group(spectrum_group_t group)
{
    if (group >= SPECTRUM_GROUP_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return spectrum_prefs_write_u8(SPECTRUM_NVS_KEY_GROUP, (uint8_t)group);
}

esp_err_t spectrum_prefs_load_style(spectrum_style_t *style)
{
    if (style == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *style = SPECTRUM_STYLE_BARS;

    uint8_t stored = 0U;
    const esp_err_t read = spectrum_prefs_read_u8(SPECTRUM_NVS_KEY_STYLE, &stored);
    if (read != ESP_OK) {
        return read;
    }
    /** Fall back to the solid-bar style for unsupported values. */
    *style = spectrum_style_sanitize(stored);
    return ESP_OK;
}

esp_err_t spectrum_prefs_save_style(spectrum_style_t style)
{
    if (style >= SPECTRUM_STYLE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return spectrum_prefs_write_u8(SPECTRUM_NVS_KEY_STYLE, (uint8_t)style);
}

esp_err_t spectrum_prefs_hint_seen(bool *seen)
{
    if (seen == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *seen = false;

    uint8_t stored = 0U;
    const esp_err_t read = spectrum_prefs_read_u8(SPECTRUM_NVS_KEY_HINT, &stored);
    if (read != ESP_OK) {
        return read;
    }
    *seen = stored != 0U;
    return ESP_OK;
}

esp_err_t spectrum_prefs_mark_hint_seen(void)
{
    return spectrum_prefs_write_u8(SPECTRUM_NVS_KEY_HINT, 1U);
}

esp_err_t spectrum_prefs_load_lines(bool *visible)
{
    if (visible == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /** Borders are on out of the box: the block grid is meant to read as a grid, and the Settings switch is there to turn them off. */
    *visible = true;

    uint8_t stored = 0U;
    const esp_err_t read = spectrum_prefs_read_u8(SPECTRUM_NVS_KEY_LINES, &stored);
    if (read != ESP_OK) {
        return read;
    }
    *visible = stored != 0U;
    return ESP_OK;
}

esp_err_t spectrum_prefs_save_lines(bool visible)
{
    return spectrum_prefs_write_u8(SPECTRUM_NVS_KEY_LINES, visible ? 1U : 0U);
}
