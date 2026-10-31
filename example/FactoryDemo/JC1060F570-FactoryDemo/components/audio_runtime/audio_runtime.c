#include "audio_runtime.h"

#include <string.h>

#include "audio_gmf.h"
#include "audio_input.h"
#include "audio_output.h"
#include "audio_volume.h"
#include "audio_runtime_internal.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_log.h"

#define TAG "AudioRuntime"

typedef struct {
    bool initialized;
    uint32_t users;
    dev_audio_codec_handles_t *dac;
    dev_audio_codec_handles_t *adc;
    esp_gmf_pool_handle_t pool;
    esp_gmf_io_handle_t sd_file_io;
    esp_gmf_io_handle_t output_io;
    esp_gmf_io_handle_t bt_io;
} audio_runtime_state_t;

static audio_runtime_state_t s_runtime;

static esp_err_t get_board_codec(const char *name, dev_audio_codec_handles_t **out)
{
    void *handle = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle(name, &handle);
    if (ret != ESP_OK || handle == NULL) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    *out = handle;
    if ((*out)->codec_dev == NULL) {
        *out = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

bool audio_runtime_ready(void)
{
    return s_runtime.initialized;
}

void audio_runtime_set_gmf_resources(esp_gmf_pool_handle_t pool,
                                     esp_gmf_io_handle_t sd_file_io,
                                     esp_gmf_io_handle_t output_io,
                                     esp_gmf_io_handle_t bt_io)
{
    s_runtime.pool = pool;
    s_runtime.sd_file_io = sd_file_io;
    s_runtime.output_io = output_io;
    s_runtime.bt_io = bt_io;
}

void audio_runtime_clear_gmf_resources(void)
{
    s_runtime.pool = NULL;
    s_runtime.sd_file_io = NULL;
    s_runtime.output_io = NULL;
    s_runtime.bt_io = NULL;
}

dev_audio_codec_handles_t *audio_runtime_dac_handles(void)
{
    return s_runtime.dac;
}

dev_audio_codec_handles_t *audio_runtime_adc_handles(void)
{
    return s_runtime.adc;
}

esp_gmf_pool_handle_t audio_runtime_pool(void)
{
    return s_runtime.pool;
}

esp_gmf_io_handle_t audio_runtime_sd_file_io(void)
{
    return s_runtime.sd_file_io;
}

esp_gmf_io_handle_t audio_runtime_output_io(void)
{
    return s_runtime.output_io;
}

esp_gmf_io_handle_t audio_runtime_bt_io(void)
{
    return s_runtime.bt_io;
}

esp_err_t audio_runtime_init(void)
{
    if (s_runtime.initialized) {
        return ESP_OK;
    }

    esp_err_t ret = get_board_codec(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, &s_runtime.dac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio_dac is unavailable: %s", esp_err_to_name(ret));
        return ret;
    }
    if (esp_board_manager_check_name(ESP_BOARD_DEVICE_NAME_AUDIO_ADC)) {
        ret = get_board_codec(ESP_BOARD_DEVICE_NAME_AUDIO_ADC, &s_runtime.adc);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "audio_adc exists but is unavailable: %s", esp_err_to_name(ret));
            s_runtime.adc = NULL;
        }
    }

    s_runtime.initialized = true;
    ret = audio_gmf_init();
    if (ret != ESP_OK) {
        s_runtime.initialized = false;
        memset(&s_runtime, 0, sizeof(s_runtime));
        return ret;
    }
    ret = audio_output_open(NULL);
    if (ret != ESP_OK) {
        s_runtime.initialized = false;
        (void)audio_gmf_deinit();
        memset(&s_runtime, 0, sizeof(s_runtime));
        return ret;
    }
    ret = audio_volume_set(AUDIO_VOLUME_DEFAULT);
    if (ret != ESP_OK) {
        s_runtime.initialized = false;
        (void)audio_output_close();
        (void)audio_gmf_deinit();
        memset(&s_runtime, 0, sizeof(s_runtime));
        return ret;
    }
    ESP_LOGI(TAG, "global audio runtime initialized (ADC=%s)", s_runtime.adc != NULL ? "available" : "unavailable");
    return ESP_OK;
}

bool audio_runtime_is_initialized(void)
{
    return s_runtime.initialized;
}

esp_err_t audio_runtime_acquire(void)
{
    if (!s_runtime.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ++s_runtime.users;
    return ESP_OK;
}

esp_err_t audio_runtime_release(void)
{
    if (!s_runtime.initialized || s_runtime.users == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    --s_runtime.users;
    return ESP_OK;
}

esp_err_t audio_runtime_deinit(void)
{
    if (!s_runtime.initialized && s_runtime.pool == NULL) {
        return ESP_OK;
    }
    if (s_runtime.users != 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime.initialized) {
        esp_err_t ret = audio_output_close();
        if (ret != ESP_OK) {
            return ret;
        }
        s_runtime.initialized = false;
    }
    esp_err_t ret = audio_gmf_deinit();
    if (ret == ESP_OK) {
        memset(&s_runtime, 0, sizeof(s_runtime));
    }
    return ret;
}
