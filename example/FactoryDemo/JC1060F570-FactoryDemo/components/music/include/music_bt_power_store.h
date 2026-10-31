#ifndef MUSIC_BT_POWER_STORE_H
#define MUSIC_BT_POWER_STORE_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load persisted Bluetooth power intent; audio pipeline ownership remains with the music app.
 * @param[out] enabled Receives the stored Bluetooth power intent.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_bt_power_store_load(bool *enabled);
/**
 * @brief Persist the requested Bluetooth power state.
 * @param enabled Whether the feature is enabled.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_bt_power_store_save(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
