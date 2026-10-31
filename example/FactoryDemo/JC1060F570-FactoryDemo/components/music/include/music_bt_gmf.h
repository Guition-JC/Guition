#ifndef MUSIC_BT_GMF_H
#define MUSIC_BT_GMF_H

#include <stdbool.h>

#include "esp_err.h"

#include "dev_audio_codec.h"
#include "esp_bt_audio_stream.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bluetooth playback pipeline and borrowed runtime resources. */
typedef struct {
    esp_gmf_pool_handle_t pool;
    dev_audio_codec_handles_t *dac;
    esp_gmf_pipeline_handle_t pipeline;
    esp_gmf_task_handle_t task;
    esp_bt_audio_stream_handle_t stream;
    bool running;
} music_bt_gmf_t;

/**
 * @brief Initialize Bluetooth playback using borrowed pool and DAC handles.
 * @param source Source to use.
 * @param pool GMF element pool.
 * @param dac DAC configuration.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_bt_gmf_init(music_bt_gmf_t *source,
                            esp_gmf_pool_handle_t pool,
                            dev_audio_codec_handles_t *dac);
/**
 * @brief Apply an A2DP stream transition to the playback pipeline.
 * @param source Source to use.
 * @param stream Stream instance.
 * @param state State used by the operation.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_bt_gmf_on_stream_state(music_bt_gmf_t *source,
                                        esp_bt_audio_stream_handle_t stream,
                                        esp_bt_audio_stream_state_t state);
/**
 * @brief Stop Bluetooth playback and release its GMF pipeline.
 * @param source Source to use.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t music_bt_gmf_deinit(music_bt_gmf_t *source);

#ifdef __cplusplus
}
#endif

#endif
