#pragma once

#include "esp_gmf_io.h"
#include "esp_gmf_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Decoder tags for encoded frames and continuous local-file streams. */
#define AUDIO_GMF_FRAME_DECODER_TAG "aud_dec"
#define AUDIO_GMF_FILE_DECODER_TAG "aud_dec_file"

/**
 * @brief Borrow the runtime-owned GMF element pool.
 * @return Borrowed runtime-owned GMF element pool.
 */
esp_gmf_pool_handle_t audio_gmf_get_pool(void);
/**
 * @brief Borrow the runtime-owned local-file input IO.
 * @return Borrowed runtime-owned local-file input IO.
 */
esp_gmf_io_handle_t audio_gmf_get_sd_file_io(void);
/**
 * @brief Borrow the runtime-owned codec output IO.
 * @return Borrowed runtime-owned codec output IO.
 */
esp_gmf_io_handle_t audio_gmf_get_output_io(void);
/**
 * @brief Borrow the runtime-owned Bluetooth stream IO.
 * @return Borrowed runtime-owned Bluetooth stream IO.
 */
esp_gmf_io_handle_t audio_gmf_get_bt_io(void);
/**
 * @brief Create the shared GMF pool, decoders and IO resources.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_gmf_init(void);
/**
 * @brief Release the shared GMF resources after their users have stopped.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t audio_gmf_deinit(void);

#ifdef __cplusplus
}
#endif
