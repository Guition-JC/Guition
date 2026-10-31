#include "music_volume_store.h"

#include "esp_log.h"
#include "nvs.h"

#define TAG "MusicVolumeStore"
#define NVS_NAMESPACE "music_volume"
#define NVS_KEY_LEVEL "level"
#define NVS_KEY_MUTED "muted"

static esp_err_t open_namespace(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    const esp_err_t ret = nvs_open(NVS_NAMESPACE, mode, handle);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "open namespace '%s' failed: %s", NVS_NAMESPACE, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t music_volume_store_load(uint8_t *level, bool *muted)
{
    if (level == NULL || muted == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t ret = open_namespace(NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t stored_level = 0U;
    uint8_t stored_muted = 0U;
    const esp_err_t level_ret = nvs_get_u8(handle, NVS_KEY_LEVEL, &stored_level);
    const esp_err_t muted_ret = nvs_get_u8(handle, NVS_KEY_MUTED, &stored_muted);
    nvs_close(handle);

    /** A half-written preference is treated as no preference at all. */
    if (level_ret != ESP_OK || muted_ret != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    *level = stored_level;
    *muted = stored_muted != 0U;
    return ESP_OK;
}

esp_err_t music_volume_store_save(uint8_t level, bool muted)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = open_namespace(NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(handle, NVS_KEY_LEVEL, level);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, NVS_KEY_MUTED, muted ? 1U : 0U);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "storing volume %u (muted=%d) failed: %s", (unsigned)level,
                 muted ? 1 : 0, esp_err_to_name(ret));
    }
    return ret;
}
