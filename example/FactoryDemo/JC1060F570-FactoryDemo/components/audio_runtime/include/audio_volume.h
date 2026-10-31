#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_VOLUME_MIN 0U
#define AUDIO_VOLUME_MAX 100U
#define AUDIO_VOLUME_DEFAULT 60U
#define AUDIO_VOLUME_STEP 10U

/** @brief Application volume level and mute state. */
typedef struct {
    uint8_t level;
    bool muted;
} audio_volume_state_t;

/** @brief Source of a shared volume update. */
typedef enum {
    AUDIO_VOLUME_ORIGIN_LOCAL = 0,
    AUDIO_VOLUME_ORIGIN_BT_REMOTE,
    AUDIO_VOLUME_ORIGIN_SYSTEM,
} audio_volume_origin_t;

typedef void (*audio_volume_listener_t)(const audio_volume_state_t *state,
                                         audio_volume_origin_t origin,
                                         void *context);

/**
 * @brief Set the local output volume in the 0..100 application range.
 * @param level Level to use.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_volume_set(uint8_t level);
/**
 * @brief Increase or decrease local volume by the configured step.
 * @param increase Whether to increase the value.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_volume_adjust(bool increase);
/**
 * @brief Set local mute state without discarding the stored level.
 * @param muted Mute state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_volume_set_mute(bool muted);
/**
 * @brief Copy the current shared volume and mute state.
 * @param[out] state Receives the current volume and mute state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_volume_get(audio_volume_state_t *state);
/**
 * @brief Apply a remote volume and mute update.
 * @param level Level to use.
 * @param muted Mute state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_volume_apply_remote(uint8_t level, bool muted);
/**
 * @brief Adjust volume by one step and identify the update as remote.
 * @param increase Whether to increase the value.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_volume_adjust_remote(bool increase);
/**
 * @brief Register a volume listener with its caller-owned context.
 * @param listener Listener callback.
 * @param context Caller-supplied context.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_volume_subscribe(audio_volume_listener_t listener, void *context);
/**
 * @brief Remove the matching volume listener and context.
 * @param listener Listener callback.
 * @param context Caller-supplied context.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_volume_unsubscribe(audio_volume_listener_t listener, void *context);

#ifdef __cplusplus
}
#endif
