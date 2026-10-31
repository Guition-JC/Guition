/** SPDX-License-Identifier: GPL-2.0-only */
#include "ft_audio_probe.h"

#include <string.h>

#include "audio_input.h"
#include "audio_output.h"
#include "audio_runtime.h"
#include "esp_attr.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define FT_AUDIO_TAG "FactoryTestAudio"

/** @brief One codec transaction per chunk keeps the worker responsive to the stop flag. */
#define FT_AUDIO_CHUNK_FRAMES 1024U
#define FT_AUDIO_WORKER_STACK 4096U
#define FT_AUDIO_WORKER_PRIORITY 5
#define FT_AUDIO_SETTLE_TIMEOUT_MS 3000U
#define FT_AUDIO_SETTLE_STEP_MS 10U

/** Allocate the 16 kHz, 16-bit mono recording and staging buffers in PSRAM for the page lifetime. */
static int16_t *s_take;
static int16_t *s_capture_chunk;
static int16_t *s_play_chunk;

static void buffers_release(void)
{
    heap_caps_free(s_take);
    heap_caps_free(s_capture_chunk);
    heap_caps_free(s_play_chunk);
    s_take = NULL;
    s_capture_chunk = NULL;
    s_play_chunk = NULL;
}

/**
 * @brief Free partially allocated buffers only when no worker is running.
 * @return true if the required buffers are available.
 */
static bool buffers_ensure(void)
{
    if (s_take == NULL) {
        s_take = heap_caps_malloc(FT_AUDIO_MONO_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    }
    if (s_capture_chunk == NULL) {
        s_capture_chunk = heap_caps_malloc(FT_AUDIO_CHUNK_FRAMES * FT_AUDIO_ADC_CHANNELS *
                                           sizeof(int16_t),
                                           MALLOC_CAP_SPIRAM);
    }
    if (s_play_chunk == NULL) {
        s_play_chunk = heap_caps_malloc(FT_AUDIO_CHUNK_FRAMES * FT_AUDIO_DAC_CHANNELS *
                                        sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM);
    }
    if (s_take == NULL || s_capture_chunk == NULL || s_play_chunk == NULL) {
        ESP_LOGE(FT_AUDIO_TAG, "audio buffers unavailable (%u bytes wanted)",
                 (unsigned)((FT_AUDIO_MONO_FRAMES + FT_AUDIO_CHUNK_FRAMES *
                             (FT_AUDIO_ADC_CHANNELS + FT_AUDIO_DAC_CHANNELS)) *
                            sizeof(int16_t)));
        buffers_release();
        return false;
    }
    return true;
}

static TaskHandle_t s_worker;
static volatile bool s_stop_requested;
static volatile bool s_have_take;
static volatile ft_audio_probe_state_t s_state = FT_AUDIO_IDLE;
static volatile esp_err_t s_last_error = ESP_OK;

static size_t audio_chunk_frames(size_t remaining)
{
    return remaining > FT_AUDIO_CHUNK_FRAMES ? FT_AUDIO_CHUNK_FRAMES : remaining;
}

/** @brief Match the DAC to the 16 kHz capture rate while the shared I2S input is open; restore its format afterward. */
typedef struct {
    bool runtime_acquired;
    bool output_was_open;
    bool output_reconfigured;
    esp_codec_dev_sample_info_t previous_output_format;
} audio_session_t;

static esp_err_t session_begin(audio_session_t *session)
{
    memset(session, 0, sizeof(*session));

    if (audio_runtime_acquire() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    session->runtime_acquired = true;

    session->output_was_open = audio_output_is_open();
    if (!session->output_was_open) {
        return ESP_OK;
    }
    if (audio_output_get_format(&session->previous_output_format) != ESP_OK) {
        return ESP_FAIL;
    }
    const esp_codec_dev_sample_info_t capture_peer = {
        .sample_rate = FT_AUDIO_SAMPLE_RATE,
        .channel = FT_AUDIO_DAC_CHANNELS,
        .bits_per_sample = FT_AUDIO_BITS,
    };
    if (audio_output_reconfigure(&capture_peer) != ESP_OK) {
        return ESP_FAIL;
    }
    session->output_reconfigured = true;
    return ESP_OK;
}

static void session_end(audio_session_t *session, bool input_opened)
{
    if (input_opened) {
        (void)audio_input_close();
    }
    if (session->output_reconfigured) {
        const esp_err_t ret = session->output_was_open
                                  ? audio_output_reconfigure(&session->previous_output_format)
                                  : audio_output_close();
        if (ret != ESP_OK) {
            ESP_LOGW(FT_AUDIO_TAG, "output format restore failed: %s", esp_err_to_name(ret));
        }
    }
    if (session->runtime_acquired) {
        (void)audio_runtime_release();
    }
}

static void worker_finish(esp_err_t result)
{
    s_last_error = result;
    s_state = FT_AUDIO_IDLE;
    s_worker = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void record_worker(void *argument)
{
    (void)argument;
    audio_session_t session;
    esp_err_t result = session_begin(&session);
    bool input_opened = false;
    size_t captured = 0U;

    if (result == ESP_OK) {
        const esp_codec_dev_sample_info_t capture_format = {
            .sample_rate = FT_AUDIO_SAMPLE_RATE,
            .channel = FT_AUDIO_ADC_CHANNELS,
            .bits_per_sample = FT_AUDIO_BITS,
        };
        if (audio_input_open(&capture_format) != ESP_OK) {
            result = ESP_FAIL;
        } else {
            input_opened = true;
        }
    }

    esp_codec_dev_handle_t codec = input_opened ? audio_input_get_codec_dev() : NULL;
    if (input_opened && codec == NULL) {
        result = ESP_ERR_INVALID_STATE;
    }

    while (result == ESP_OK && captured < FT_AUDIO_MONO_FRAMES && !s_stop_requested) {
        const size_t frames = audio_chunk_frames(FT_AUDIO_MONO_FRAMES - captured);
        const int read_ret = esp_codec_dev_read(
            codec, s_capture_chunk,
            (int)(frames * FT_AUDIO_ADC_CHANNELS * sizeof(int16_t)));
        if (read_ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(FT_AUDIO_TAG, "codec read failed: %d", read_ret);
            result = ESP_FAIL;
            break;
        }
        for (size_t index = 0U; index < frames; ++index) {
            s_take[captured + index] = s_capture_chunk[index * FT_AUDIO_ADC_CHANNELS];
        }
        captured += frames;
    }

    if (result == ESP_OK) {
        if (captured == FT_AUDIO_MONO_FRAMES) {
            s_have_take = true;
            ESP_LOGI(FT_AUDIO_TAG, "recorded %u frames", (unsigned)captured);
        } else {
            /** Aborted by the page: the partial take is not a usable recording. */
            s_have_take = false;
            result = ESP_ERR_INVALID_STATE;
        }
    }

    session_end(&session, input_opened);
    worker_finish(result);
}

static void playback_worker(void *argument)
{
    (void)argument;
    audio_session_t session;
    esp_err_t result = session_begin(&session);
    if (result == ESP_OK) {
        const esp_codec_dev_sample_info_t playback_format = {
            .sample_rate = FT_AUDIO_SAMPLE_RATE,
            .channel = FT_AUDIO_DAC_CHANNELS,
            .bits_per_sample = FT_AUDIO_BITS,
        };
        if (audio_output_reconfigure(&playback_format) != ESP_OK) {
            result = ESP_FAIL;
        } else {
            /** Mark the restored shared-codec format for playback-session restoration. */
            session.output_reconfigured = true;
        }
    }

    esp_codec_dev_handle_t codec = result == ESP_OK ? audio_output_get_codec_dev() : NULL;
    if (result == ESP_OK && codec == NULL) {
        result = ESP_ERR_INVALID_STATE;
    }

    size_t played = 0U;
    while (result == ESP_OK && played < FT_AUDIO_MONO_FRAMES && !s_stop_requested) {
        const size_t frames = audio_chunk_frames(FT_AUDIO_MONO_FRAMES - played);
        for (size_t index = 0U; index < frames; ++index) {
            const int16_t sample = s_take[played + index];
            s_play_chunk[index * FT_AUDIO_DAC_CHANNELS] = sample;
            s_play_chunk[index * FT_AUDIO_DAC_CHANNELS + 1U] = sample;
        }
        const int write_ret = esp_codec_dev_write(
            codec, s_play_chunk,
            (int)(frames * FT_AUDIO_DAC_CHANNELS * sizeof(int16_t)));
        if (write_ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(FT_AUDIO_TAG, "codec write failed: %d", write_ret);
            result = ESP_FAIL;
            break;
        }
        played += frames;
    }

    session_end(&session, false);
    worker_finish(result);
}

static esp_err_t worker_start(TaskFunction_t entry, ft_audio_probe_state_t state)
{
    if (s_worker != NULL || s_state != FT_AUDIO_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    s_stop_requested = false;
    s_last_error = ESP_OK;
    s_state = state;
    /** Allocate the worker stack in PSRAM. */
    if (xTaskCreatePinnedToCoreWithCaps(entry, "ft_audio", FT_AUDIO_WORKER_STACK, NULL,
                                        FT_AUDIO_WORKER_PRIORITY, &s_worker, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_worker = NULL;
        s_state = FT_AUDIO_IDLE;
        s_last_error = ESP_ERR_NO_MEM;
        ESP_LOGE(FT_AUDIO_TAG, "audio worker task allocation failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ft_audio_probe_record_start(void)
{
    /** A new take supersedes the previous one, so nothing stale can be played. */
    s_have_take = false;
    if (s_worker == NULL && !buffers_ensure()) {
        return ESP_ERR_NO_MEM;
    }
    return worker_start(record_worker, FT_AUDIO_RECORDING);
}

esp_err_t ft_audio_probe_play_start(void)
{
    if (!s_have_take) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_worker == NULL && !buffers_ensure()) {
        return ESP_ERR_NO_MEM;
    }
    return worker_start(playback_worker, FT_AUDIO_PLAYING);
}

void ft_audio_probe_stop(void)
{
    s_stop_requested = true;
}

void ft_audio_probe_cleanup(void)
{
    s_stop_requested = true;

    /** Wait a bounded time for worker-owned codec teardown and runtime release. */
    for (uint32_t waited = 0U; waited < FT_AUDIO_SETTLE_TIMEOUT_MS && s_worker != NULL;
         waited += FT_AUDIO_SETTLE_STEP_MS) {
        vTaskDelay(pdMS_TO_TICKS(FT_AUDIO_SETTLE_STEP_MS));
    }
    if (s_worker != NULL) {
        ESP_LOGE(FT_AUDIO_TAG, "audio worker did not stop; leaving it to finish");
    }

    s_have_take = false;
    s_state = FT_AUDIO_IDLE;
    /** Keep recording buffers allocated until the worker stops using them. */
    if (s_worker == NULL) {
        buffers_release();
    }
}

bool ft_audio_probe_has_recording(void)
{
    return s_have_take;
}

ft_audio_probe_state_t ft_audio_probe_state(void)
{
    return s_state;
}

esp_err_t ft_audio_probe_last_error(void)
{
    return s_last_error;
}
