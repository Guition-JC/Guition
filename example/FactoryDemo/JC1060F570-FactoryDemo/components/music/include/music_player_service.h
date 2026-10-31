#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "music_player_state.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enter the Music app and acquire its app-scoped SD/Classic-Bluetooth resources.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_player_service_enter(void);

/**
 * @brief Exit the Music app and release app-scoped resources while retaining the global GMF pool.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_player_service_exit(void);

/**
 * @brief Return to the desktop, safely handing a Bluetooth source back to SD before exit.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_player_service_return_home(void);

/**
 * @brief Select the local SD-card source or the Classic-Bluetooth A2DP source path.
 * @param source Source to use.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_player_service_select_source(music_player_source_t source);

/**
 * @brief Return the currently selected source.
 * @return The currently selected source.
 */
music_player_source_t music_player_service_get_source(void);

/**
 * @brief Set Bluetooth power and host/profile availability; disabling first hands playback back to SD.
 * @param enabled Whether the feature is enabled.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_player_service_set_bt_power(bool enabled);

/**
 * @brief Return whether the Classic Bluetooth controller is currently running.
 * @return true if the Classic Bluetooth controller is currently running; false otherwise.
 */
bool music_player_service_get_bt_power(void);

/**
 * @brief Return whether a Classic Bluetooth peer (phone) is currently connected.
 * @return true if a Classic Bluetooth peer (phone) is currently connected; false otherwise.
 */
bool music_player_service_bt_connected(void);

/**
 * @brief Apply the stored Bluetooth power intent once during boot.
 */
void music_player_service_restore_bt_power(void);

/**
 * @brief Start or resume the selected source.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_player_service_play(void);

/**
 * @brief Pause the selected source.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_player_service_pause(void);

/**
 * @brief Select the next track on the selected source.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_player_service_next(void);

/**
 * @brief Select the previous track on the selected source.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_player_service_previous(void);

/**
 * @brief Return the last confirmed local playback state.
 * @return true if local playback is currently confirmed playing.
 */
bool music_player_service_is_playing(void);

/**
 * @brief Set the global audio output volume in the application range 0..100.
 * @param level Level to use.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_player_service_set_volume(uint8_t level);

/**
 * @brief Read the global audio output volume and mute state.
 * @param[out] level Receives the current volume level.
 * @param[out] muted Receives the mute state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_player_service_get_volume(uint8_t *level, bool *muted);
/**
 * @brief Set the global audio output mute state.
 * @param muted Mute state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_player_service_set_mute(bool muted);
/**
 * @brief Apply the volume and mute preference stored for the settings page.
 */
void music_player_service_restore_volume(void);
/**
 * @brief Persist the current volume and mute state after a user edit.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_player_service_save_volume(void);

#ifdef __cplusplus
}
#endif
