#include "music_bt_power_store.h"

#include "esp_log.h"
#include "nvs.h"

#define TAG "MusicBtPowerStore"
#define NVS_NAMESPACE "music_bt"
#define NVS_KEY_POWER "power"

static esp_err_t open_namespace(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    const esp_err_t ret = nvs_open(NVS_NAMESPACE, mode, handle);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "open namespace '%s' failed: %s", NVS_NAMESPACE, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t music_bt_power_store_load(bool *enabled)
{
    if (enabled == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t ret = open_namespace(NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t stored = 0U;
    ret = nvs_get_u8(handle, NVS_KEY_POWER, &stored);
    nvs_close(handle);
    if (ret == ESP_OK) {
        *enabled = stored != 0U;
        return ESP_OK;
    }
    /** Callers only need to tell "no stored intent" from a real failure. */
    return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : ret;
}

esp_err_t music_bt_power_store_save(bool enabled)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = open_namespace(NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(handle, NVS_KEY_POWER, enabled ? 1U : 0U);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "store Bluetooth power %d failed: %s", enabled ? 1 : 0, esp_err_to_name(ret));
    }
    return ret;
}
