#pragma once

#include <stdbool.h>
#include "esp_codec_dev.h"
#include "dev_audio_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Report whether input codec handles are available.
 * @return true if input codec handles are available; false otherwise.
 */
bool audio_input_is_available(void);
/**
 * @brief Borrow the initialized runtime input codec handles.
 * @param handles Resource handles.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_input_get_handles(dev_audio_codec_handles_t **handles);
/**
 * @brief Borrow the input codec device, or return NULL if unavailable.
 * @return Borrowed input codec device, or NULL if unavailable.
 */
esp_codec_dev_handle_t audio_input_get_codec_dev(void);
/**
 * @brief Open capture with the requested format and default microphone gain.
 * @param format Data format.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_input_open(const esp_codec_dev_sample_info_t *format);
/**
 * @brief Close input and clear its active sample format.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_input_close(void);
/**
 * @brief Report whether the input codec is open.
 * @return true if the input codec is open; false otherwise.
 */
bool audio_input_is_open(void);
/**
 * @brief Copy the active input sample format; input must be open.
 * @param[out] format Receives the active input sample format.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_input_get_format(esp_codec_dev_sample_info_t *format);

#ifdef __cplusplus
}
#endif
