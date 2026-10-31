#include "audio_input.h"

#include <string.h>

#include "audio_runtime_internal.h"
#include "esp_log.h"

#define TAG "AudioInput"
#define AUDIO_INPUT_DEFAULT_GAIN_DB 18.0f

static bool s_open;
static esp_codec_dev_sample_info_t s_format;

bool audio_input_is_available(void)
{
    return audio_runtime_adc_handles() != NULL;
}

esp_err_t audio_input_get_handles(dev_audio_codec_handles_t **handles)
{
    if (handles == NULL || !audio_runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    *handles = audio_runtime_adc_handles();
    return *handles == NULL ? ESP_ERR_NOT_FOUND : ESP_OK;
}

esp_codec_dev_handle_t audio_input_get_codec_dev(void)
{
    dev_audio_codec_handles_t *handles = audio_runtime_adc_handles();
    return handles == NULL ? NULL : handles->codec_dev;
}

esp_err_t audio_input_open(const esp_codec_dev_sample_info_t *format)
{
    esp_codec_dev_handle_t codec = audio_input_get_codec_dev();
    if (codec == NULL || format == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_open) {
        return memcmp(&s_format, format, sizeof(s_format)) == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (esp_codec_dev_open(codec, (esp_codec_dev_sample_info_t *)format) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_in_gain(codec, AUDIO_INPUT_DEFAULT_GAIN_DB) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to set default microphone gain to %.1f dB",
                 AUDIO_INPUT_DEFAULT_GAIN_DB);
        (void)esp_codec_dev_close(codec);
        return ESP_FAIL;
    }
    s_format = *format;
    s_open = true;
    ESP_LOGI(TAG, "Default microphone gain set to %.1f dB", AUDIO_INPUT_DEFAULT_GAIN_DB);
    return ESP_OK;
}

esp_err_t audio_input_close(void)
{
    esp_codec_dev_handle_t codec = audio_input_get_codec_dev();
    if (!s_open) {
        return ESP_OK;
    }
    if (codec == NULL || esp_codec_dev_close(codec) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    s_open = false;
    memset(&s_format, 0, sizeof(s_format));
    return ESP_OK;
}

bool audio_input_is_open(void)
{
    return s_open;
}

esp_err_t audio_input_get_format(esp_codec_dev_sample_info_t *format)
{
    if (format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    *format = s_format;
    return ESP_OK;
}
