#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Owned encoded-audio playback pipeline. */
typedef struct aichat_audio_playback aichat_audio_playback_t;

/**
 * @brief Allocate a playback instance.
 * @return Allocated playback instance.
 */
aichat_audio_playback_t *aichat_audio_playback_create(void);
/**
 * @brief Start Opus playback.
 * @param sample_rate Encoded audio sample rate in Hz.
 * @param frame_duration_ms Encoded frame duration in milliseconds.
 */
esp_err_t aichat_audio_playback_start(aichat_audio_playback_t *playback,
                                      uint32_t sample_rate,
                                      uint32_t frame_duration_ms);
/**
 * @brief Copy an encoded packet into the playback queue.
 * @param len Packet length in bytes.
 * @return ESP_ERR_TIMEOUT when the queue is full.
 */
esp_err_t aichat_audio_playback_enqueue(aichat_audio_playback_t *playback,
                                        const uint8_t *data,
                                        size_t len);
/**
 * @brief Stop playback and release its pipeline resources.
 * @param playback Playback state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_audio_playback_stop(aichat_audio_playback_t *playback);
/**
 * @brief Stop playback and free its instance.
 * @param playback Playback state.
 */
void aichat_audio_playback_destroy(aichat_audio_playback_t *playback);

#ifdef __cplusplus
}
#endif
