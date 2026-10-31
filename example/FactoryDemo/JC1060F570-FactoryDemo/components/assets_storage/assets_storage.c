#include "assets_storage.h"
#include "assets_decoder_compat.h"

#include <stddef.h>

#include "esp_log.h"
#include "esp_lv_decoder.h"
#include "esp_spiffs.h"

#define ASSETS_BASE_PATH "/assets"
#define ASSETS_PARTITION_LABEL "assets"
#define ASSETS_MAX_OPEN_FILES 12

static const char *const TAG = "assets_storage";
static bool s_mounted;
static esp_lv_decoder_handle_t s_decoder;

esp_err_t assets_storage_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    const esp_vfs_spiffs_conf_t config = {
        .base_path = ASSETS_BASE_PATH,
        .partition_label = ASSETS_PARTITION_LABEL,
        .max_files = ASSETS_MAX_OPEN_FILES,
        .format_if_mount_failed = false,
    };

    const esp_err_t ret = esp_vfs_spiffs_register(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS partition '%s' at %s: %s",
                 ASSETS_PARTITION_LABEL, ASSETS_BASE_PATH, esp_err_to_name(ret));
        return ret;
    }

    size_t total_bytes = 0;
    size_t used_bytes = 0;
    const esp_err_t info_ret = esp_spiffs_info(ASSETS_PARTITION_LABEL, &total_bytes, &used_bytes);
    if (info_ret == ESP_OK) {
        ESP_LOGI(TAG, "Mounted SPIFFS assets at %s (%u/%u bytes)", ASSETS_BASE_PATH,
                 (unsigned)used_bytes, (unsigned)total_bytes);
    } else {
        ESP_LOGW(TAG, "SPIFFS assets mounted but usage query failed: %s",
                 esp_err_to_name(info_ret));
    }

    s_mounted = true;
    return ESP_OK;
}

esp_err_t assets_storage_init_lvgl_decoder(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_decoder != NULL) {
        return ESP_OK;
    }

    const esp_err_t ret = esp_lv_decoder_init(&s_decoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LVGL image decoder: %s", esp_err_to_name(ret));
        s_decoder = NULL;
        return ret;
    }

    if (!assets_decoder_compat_install((lv_image_decoder_t *)s_decoder)) {
        ESP_LOGE(TAG, "LVGL decoder file compatibility setup failed");
        esp_lv_decoder_deinit(s_decoder);
        s_decoder = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "LVGL image decoder ready for JPEG/PNG assets (file lifetime fix)");
    return ESP_OK;
}

bool assets_storage_icons_ready(void)
{
    return s_mounted && s_decoder != NULL;
}
