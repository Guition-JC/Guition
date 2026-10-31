#pragma once

#include <stdbool.h>
#include "dev_audio_codec.h"
#include "esp_gmf_io.h"
#include "esp_gmf_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Borrow the runtime-owned output codec handles.
 * @return Borrowed runtime-owned output codec handles.
 */
dev_audio_codec_handles_t *audio_runtime_dac_handles(void);
/**
 * @brief Borrow the runtime-owned input codec handles.
 * @return Borrowed runtime-owned input codec handles.
 */
dev_audio_codec_handles_t *audio_runtime_adc_handles(void);
/**
 * @brief Borrow the shared GMF element pool.
 * @return Borrowed shared GMF element pool.
 */
esp_gmf_pool_handle_t audio_runtime_pool(void);
/**
 * @brief Borrow the shared local-file input IO.
 * @return Borrowed shared local-file input IO.
 */
esp_gmf_io_handle_t audio_runtime_sd_file_io(void);
/**
 * @brief Borrow the shared codec output IO.
 * @return Borrowed shared codec output IO.
 */
esp_gmf_io_handle_t audio_runtime_output_io(void);
/**
 * @brief Borrow the shared Bluetooth stream IO.
 * @return Borrowed shared Bluetooth stream IO.
 */
esp_gmf_io_handle_t audio_runtime_bt_io(void);
/**
 * @brief Report whether the audio runtime is initialized.
 * @return true when the audio runtime is initialized.
 */
bool audio_runtime_ready(void);
/**
 * @brief Register the shared GMF pool and IO handles in the runtime.
 * @param pool GMF element pool.
 * @param sd_file_io SD file input IO.
 * @param output_io Output IO.
 * @param bt_io Bluetooth stream IO.
 */
void audio_runtime_set_gmf_resources(esp_gmf_pool_handle_t pool,
                                     esp_gmf_io_handle_t sd_file_io,
                                     esp_gmf_io_handle_t output_io,
                                     esp_gmf_io_handle_t bt_io);
/**
 * @brief Clear registered GMF resource references.
 */
void audio_runtime_clear_gmf_resources(void);

#ifdef __cplusplus
}
#endif
