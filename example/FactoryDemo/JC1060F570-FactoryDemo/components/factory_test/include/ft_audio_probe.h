/** SPDX-License-Identifier: GPL-2.0-only */
/** Record five seconds in PSRAM and play it back on a dedicated audio worker. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FT_AUDIO_RECORD_SECONDS 5U
#define FT_AUDIO_SAMPLE_RATE 16000U
#define FT_AUDIO_BITS 16U
/** @brief The board ADC is a four-slot TDM input; slot 0 is the first microphone. */
#define FT_AUDIO_ADC_CHANNELS 4U
#define FT_AUDIO_DAC_CHANNELS 2U
#define FT_AUDIO_MONO_FRAMES (FT_AUDIO_SAMPLE_RATE * FT_AUDIO_RECORD_SECONDS)

/** @brief Audio diagnostic worker state. */
typedef enum {
    FT_AUDIO_IDLE = 0,
    FT_AUDIO_RECORDING,
    FT_AUDIO_PLAYING,
} ft_audio_probe_state_t;

/**
 * @brief Start a fresh five second recording. Only valid from IDLE.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t ft_audio_probe_record_start(void);

/**
 * @brief Play the current take. Only valid from IDLE, and only with a take present.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t ft_audio_probe_play_start(void);

/**
 * @brief Ask the worker to stop early; it finishes its own codec cleanup.
 */
void ft_audio_probe_stop(void);

/**
 * @brief Stop the worker with a bounded wait, then release its codec, runtime reference and recording.
 */
void ft_audio_probe_cleanup(void);

/**
 * @brief true once a complete five second take is stored in PSRAM.
 * @return True once a complete five second take is stored in PSRAM.
 */
bool ft_audio_probe_has_recording(void);

/**
 * @brief Current worker state; the page polls this from an LVGL timer.
 * @return Current worker state; the page polls this from an LVGL timer.
 */
ft_audio_probe_state_t ft_audio_probe_state(void);

/**
 * @brief Result of the last finished worker run (ESP_OK when it completed).
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t ft_audio_probe_last_error(void);

#ifdef __cplusplus
}
#endif
