#include "audio_output.h"

#include <string.h>

#include "audio_runtime_internal.h"

#define AUDIO_OUTPUT_SAMPLE_RATE 48000U
#define AUDIO_OUTPUT_CHANNELS 2U
#define AUDIO_OUTPUT_BITS 16U

static bool s_open;
static esp_codec_dev_sample_info_t s_format;

static bool format_equal(const esp_codec_dev_sample_info_t *left,
                         const esp_codec_dev_sample_info_t *right)
{
    return left != NULL && right != NULL && memcmp(left, right, sizeof(*left)) == 0;
}

static esp_err_t open_codec(const esp_codec_dev_sample_info_t *format)
{
    esp_codec_dev_handle_t codec = audio_output_get_codec_dev();
    if (codec == NULL || format == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_codec_dev_sample_info_t requested = *format;
    if (esp_codec_dev_open(codec, &requested) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    s_format = requested;
    s_open = true;
    return ESP_OK;
}

esp_err_t audio_output_get_handles(dev_audio_codec_handles_t **handles)
{
    if (handles == NULL || !audio_runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    *handles = audio_runtime_dac_handles();
    return *handles == NULL ? ESP_ERR_NOT_FOUND : ESP_OK;
}

esp_codec_dev_handle_t audio_output_get_codec_dev(void)
{
    dev_audio_codec_handles_t *handles = audio_runtime_dac_handles();
    return handles == NULL ? NULL : handles->codec_dev;
}

esp_err_t audio_output_open(const esp_codec_dev_sample_info_t *format)
{
    esp_codec_dev_sample_info_t default_format = {
        .sample_rate = AUDIO_OUTPUT_SAMPLE_RATE,
        .channel = AUDIO_OUTPUT_CHANNELS,
        .bits_per_sample = AUDIO_OUTPUT_BITS,
    };
    const esp_codec_dev_sample_info_t *requested = format == NULL ? &default_format : format;
    if (audio_output_get_codec_dev() == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_open) {
        return format_equal(&s_format, requested) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    return open_codec(requested);
}

esp_err_t audio_output_reconfigure(const esp_codec_dev_sample_info_t *format)
{
    if (format == NULL || audio_output_get_codec_dev() == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_open) {
        return open_codec(format);
    }
    if (format_equal(&s_format, format)) {
        return ESP_OK;
    }

    const esp_codec_dev_sample_info_t previous = s_format;
    esp_codec_dev_handle_t codec = audio_output_get_codec_dev();
    if (esp_codec_dev_close(codec) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    s_open = false;
    memset(&s_format, 0, sizeof(s_format));

    if (open_codec(format) == ESP_OK) {
        return ESP_OK;
    }
    if (open_codec(&previous) != ESP_OK) {
        memset(&s_format, 0, sizeof(s_format));
        s_open = false;
    }
    return ESP_FAIL;
}

esp_err_t audio_output_get_format(esp_codec_dev_sample_info_t *format)
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

esp_err_t audio_output_close(void)
{
    esp_codec_dev_handle_t codec = audio_output_get_codec_dev();
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

bool audio_output_is_open(void)
{
    return s_open;
}
