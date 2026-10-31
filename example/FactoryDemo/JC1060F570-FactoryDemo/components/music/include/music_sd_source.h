#ifndef MUSIC_SD_SOURCE_H
#define MUSIC_SD_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dev_audio_codec.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_task.h"
#include "music_player_state.h"
#include "music_sd_catalog.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Report playback events with the generation assigned when the track started. */
typedef void (*music_sd_source_event_cb_t)(void *context,
                                            music_player_error_t error,
                                            uint32_t generation,
                                            const char *detail);

/** @brief Local playback pipeline and borrowed audio runtime resources. */
typedef struct {
    esp_gmf_pool_handle_t pool; /**< Borrowed from audio_runtime. */
    dev_audio_codec_handles_t *dac; /**< Borrowed from audio_runtime. */
    music_sd_catalog_t catalog;
    music_sd_source_event_cb_t event_cb;
    void *event_context;
    esp_gmf_pipeline_handle_t pipeline;
    esp_gmf_task_handle_t task;
    size_t current_index;
    uint32_t generation;
    bool sd_owned;
    bool opened;
    bool running;
} music_sd_source_t;

/**
 * @brief Open local roots; NULL/0 selects SD then built-in SPIFFS. Earlier roots override duplicate names.
 * @param source Source to use.
 * @param pool GMF element pool.
 * @param dac DAC configuration.
 * @param roots Root paths.
 * @param root_count Number of root paths.
 * @param event_cb Event callback.
 * @param event_context Context passed to the event callback.
 * @return The operation status.
 */
music_player_error_t music_sd_source_open(music_sd_source_t *source,
                                           esp_gmf_pool_handle_t pool,
                                           dev_audio_codec_handles_t *dac,
                                           const char *const *roots,
                                           size_t root_count,
                                           music_sd_source_event_cb_t event_cb,
                                           void *event_context);
/**
 * @brief Start the catalog track at the given zero-based index.
 * @param source Source to use.
 * @param index Index of the item.
 * @return The operation status.
 */
music_player_error_t music_sd_source_start(music_sd_source_t *source, size_t index);
/**
 * @brief Pause the active local playback pipeline.
 * @param source Source to use.
 * @return The operation status.
 */
music_player_error_t music_sd_source_pause(music_sd_source_t *source);
/**
 * @brief Resume a paused local pipeline that has not ended or failed.
 * @param source Source to use.
 * @return The operation status.
 */
music_player_error_t music_sd_source_resume(music_sd_source_t *source);
/**
 * @brief Stop the active local playback pipeline.
 * @param source Source to use.
 * @return The operation status.
 */
music_player_error_t music_sd_source_stop(music_sd_source_t *source);
/**
 * @brief Release the local source pipeline and catalog state.
 * @param source Source to use.
 * @return The operation status.
 */
music_player_error_t music_sd_source_close(music_sd_source_t *source);
/**
 * @brief Borrow the catalog retained by the local source.
 * @param source Source to use.
 * @return Borrowed catalog retained by the local source.
 */
const music_sd_catalog_t *music_sd_source_catalog(const music_sd_source_t *source);

#ifdef __cplusplus
}
#endif

#endif
