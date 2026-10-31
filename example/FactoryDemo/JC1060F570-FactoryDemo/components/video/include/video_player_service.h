#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "video_player_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VIDEO_PLAYER_NAME_MAX 96U
#define VIDEO_PLAYER_PATH_MAX 256U
#define VIDEO_PLAYER_MAX_ENTRIES 32U

/** @brief Video catalog filename and playback path. */
typedef struct {
    char name[VIDEO_PLAYER_NAME_MAX];
    char path[VIDEO_PLAYER_PATH_MAX];
} video_player_catalog_entry_t;

/** @brief Current media metadata and playback position. */
typedef struct {
    video_player_status_t status;
    char name[VIDEO_PLAYER_NAME_MAX];
    uint32_t duration_ms;
    uint32_t position_ms;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    bool has_media;
    bool audio_available;
    bool video_available;
    esp_err_t last_error;
} video_player_snapshot_t;

/**
 * @brief Report a decoded frame without blocking or accessing LVGL.
 * The decode task invokes this callback; the view retains accepted buffers until release.
 * @param stride Row stride in bytes.
 * @param pts_ms Presentation timestamp in milliseconds.
 * @return true when the view accepts the frame.
 */
typedef bool (*video_player_frame_cb_t)(uint8_t buffer_index, uint16_t width, uint16_t height,
                                        uint32_t stride, uint32_t pts_ms, void *context);

/**
 * @brief Open playback and scan MP4 files in /assets/video before /sdcard/video.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t video_player_service_enter(void);
/**
 * @brief Open playback and scan only /assets/video for the boot clip.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t video_player_service_enter_assets(void);
/**
 * @brief Enter playback without opening the audio runtime; audio frames are discarded.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t video_player_service_enter_silent(void);
/**
 * @brief Read the same MP4 roots as the player without entering the service.
 * @param[out] entries Receives the catalog entries.
 * @param capacity Capacity of the destination buffer.
 * @return Number of catalog entries copied.
 */
size_t video_player_service_list(video_player_catalog_entry_t *entries, size_t capacity);
/**
 * @brief Select one scanned media path before playback starts.
 * @param path Path to use.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t video_player_service_select_path(const char *path);
/**
 * @brief Stop the decoder task and release extractor, decoders and audio runtime.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t video_player_service_exit(void);
/**
 * @brief Set panel resolution limits; frames must fit and are rendered without scaling.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t video_player_service_set_display_size(uint16_t width, uint16_t height);
/**
 * @brief Provide two caller-owned RGB565 decode buffers.
 * @param capacity Byte capacity of each buffer.
 */
esp_err_t video_player_service_set_output_buffers(uint8_t *first, uint8_t *second, uint32_t capacity);
/**
 * @brief Return a displayed buffer to the decoder once LVGL no longer reads it.
 * @param buffer_index Buffer index.
 */
void video_player_service_release_output_buffer(uint8_t buffer_index);
/**
 * @brief Return whether the VideoPlayer service owns its playback resources.
 * @return true if the VideoPlayer service owns its playback resources; false otherwise.
 */
bool video_player_service_is_entered(void);
/**
 * @brief Begin or resume the selected MP4. At EOS, starts the selected file from its beginning.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t video_player_service_play(void);
/**
 * @brief Pause active playback.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t video_player_service_pause(void);
/**
 * @brief Stop playback and reset its position.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t video_player_service_stop(void);
/**
 * @brief Select adjacent media. During playback, stop the old task and start the selected file after cleanup.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t video_player_service_next(void);
/**
 * @brief Select the previous media item and restart active playback after cleanup.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t video_player_service_previous(void);
/**
 * @brief Copy the current media and playback state.
 * @param[out] snapshot Receives the copied state.
 */
void video_player_service_get_snapshot(video_player_snapshot_t *snapshot);
/**
 * @brief Preload the selected file into PSRAM, rejecting files above max_bytes. The copy remains
 * owned by the service until exit or explicit release.
 */
esp_err_t video_player_service_preload_selected(size_t max_bytes);
/**
 * @brief Release the preloaded copy early; harmless when nothing was preloaded.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t video_player_service_release_preload(void);
/**
 * @brief Register a decode-task callback; it must not block or access LVGL directly.
 * @param callback Callback to invoke.
 * @param context Caller-supplied context.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t video_player_service_set_frame_callback(video_player_frame_cb_t callback, void *context);

#ifdef __cplusplus
}
#endif
