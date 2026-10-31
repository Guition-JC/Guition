#ifndef MUSIC_VOLUME_STORE_H
#define MUSIC_VOLUME_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load persisted volume and the separate mute preference.
 * @param[out] level Stored volume level, conventionally in the 0..100 range.
 * @param[out] muted Stored mute flag.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for a null output, or an NVS error.
 */
esp_err_t music_volume_store_load(uint8_t *level, bool *muted);
/**
 * @brief Persist volume and the separate mute flag.
 * @param level Volume level, conventionally in the 0..100 range.
 * @param muted Mute flag to persist.
 * @return ESP_OK on success, or an NVS error.
 */
esp_err_t music_volume_store_save(uint8_t level, bool muted);

#ifdef __cplusplus
}
#endif

#endif
