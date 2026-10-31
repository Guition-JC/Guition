#pragma once

#include <stdbool.h>
#include "esp_codec_dev.h"
#include "dev_audio_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Borrow the initialized runtime output codec handles.
 * @param handles Resource handles.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_output_get_handles(dev_audio_codec_handles_t **handles);
/**
 * @brief Borrow the output codec device, or return NULL if unavailable.
 * @return Borrowed output codec device, or NULL if unavailable.
 */
esp_codec_dev_handle_t audio_output_get_codec_dev(void);
/**
 * @brief Open output; NULL format selects 48 kHz, stereo, 16-bit audio.
 * @param format Data format.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_output_open(const esp_codec_dev_sample_info_t *format);
/**
 * @brief Apply a new output format and attempt to restore the old format on failure.
 * @param format Data format.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_output_reconfigure(const esp_codec_dev_sample_info_t *format);
/**
 * @brief Copy the active output sample format; output must be open.
 * @param[out] format Receives the active output sample format.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_output_get_format(esp_codec_dev_sample_info_t *format);
/**
 * @brief Close output and clear its active sample format.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_output_close(void);
/**
 * @brief Report whether the output codec is open.
 * @return true if the output codec is open; false otherwise.
 */
bool audio_output_is_open(void);

#ifdef __cplusplus
}
#endif
