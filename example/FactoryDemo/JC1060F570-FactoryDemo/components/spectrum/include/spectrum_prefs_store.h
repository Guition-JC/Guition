#pragma once

#include "esp_err.h"
#include "spectrum_bars.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Persist spectrum grouping, display style, borders and the one-time style hint. */

/**
 * @brief Read the stored grouping. ESP_ERR_NOT_FOUND means "never stored".
 * @param[out] group Receives the stored grouping.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t spectrum_prefs_load_group(spectrum_group_t *group);

/**
 * @brief Persist the grouping so the next launch starts on it.
 * @param group Frequency grouping mode.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t spectrum_prefs_save_group(spectrum_group_t group);

/**
 * @brief Read the stored display style. ESP_ERR_NOT_FOUND means "never stored".
 * @param[out] style Receives the stored display style.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t spectrum_prefs_load_style(spectrum_style_t *style);

/**
 * @brief Persist the display style so the next launch starts on it.
 * @param style Rendering style.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t spectrum_prefs_save_style(spectrum_style_t style);

/**
 * @brief Report whether the "long-press" hint has already been shown once.
 * @param seen Seen state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t spectrum_prefs_hint_seen(bool *seen);

/**
 * @brief Remember that the hint has been shown, so it never comes back.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t spectrum_prefs_mark_hint_seen(void);

/**
 * @brief Read whether the block grid draws its cell borders. ESP_ERR_NOT_FOUND means "never stored", in which case *visible stays true.
 * @param[out] visible Receives whether cell borders are visible.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t spectrum_prefs_load_lines(bool *visible);

/**
 * @brief Persist the cell-border preference (the Settings app owns the control).
 * @param visible Whether the item is visible.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t spectrum_prefs_save_lines(bool visible);

#ifdef __cplusplus
}
#endif
