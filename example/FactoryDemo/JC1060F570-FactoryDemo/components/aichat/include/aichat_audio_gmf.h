#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_gmf_payload.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Owned microphone and speech-processing pipeline. */
typedef struct aichat_audio_gmf aichat_audio_gmf_t;
typedef void (*aichat_audio_pcm_cb_t)(const uint8_t *data, size_t len, void *context);
typedef void (*aichat_audio_afe_event_cb_t)(int event_type, void *event_data, void *context);

/**
 * @brief Create an audio capture pipeline with PCM and AFE callbacks.
 * @param pcm_cb PCM callback.
 * @param afe_cb Callback receiving AFE events.
 * @param context Caller-supplied context.
 * @return Created audio capture instance.
 */
aichat_audio_gmf_t *aichat_audio_gmf_create(aichat_audio_pcm_cb_t pcm_cb,
                                             aichat_audio_afe_event_cb_t afe_cb,
                                             void *context);
/**
 * @brief Start microphone capture, wake detection and the audio pipeline.
 * @param audio Audio data or configuration.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_audio_gmf_start(aichat_audio_gmf_t *audio);
/**
 * @brief Enable or suspend encoded audio uplink for the current turn.
 * @param audio Audio data or configuration.
 * @param enabled Whether the feature is enabled.
 */
void aichat_audio_gmf_set_uplink_enabled(aichat_audio_gmf_t *audio, bool enabled);
/**
 * @brief Select speech-processing settings for listening or wake detection.
 * @param audio Audio data or configuration.
 * @param listening Whether listening is active.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_audio_gmf_set_listening_profile(aichat_audio_gmf_t *audio, bool listening);
/**
 * @brief End the current AFE wake session.
 * @param audio Audio data or configuration.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_audio_gmf_end_wake_session(aichat_audio_gmf_t *audio);
/**
 * @brief Stop the audio capture pipeline.
 * @param audio Audio data or configuration.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_audio_gmf_stop(aichat_audio_gmf_t *audio);
/**
 * @brief Stop capture and release the audio instance resources.
 * @param audio Audio data or configuration.
 */
void aichat_audio_gmf_destroy(aichat_audio_gmf_t *audio);
/**
 * @brief Report whether the audio capture pipeline is running.
 * @param audio Audio data or configuration.
 * @return true if the audio capture pipeline is running; false otherwise.
 */
bool aichat_audio_gmf_is_running(const aichat_audio_gmf_t *audio);

#ifdef __cplusplus
}
#endif
