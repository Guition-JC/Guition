#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VIDEO_PLAYER_STATUS_IDLE = 0,
    VIDEO_PLAYER_STATUS_READY,
    VIDEO_PLAYER_STATUS_PLAYING,
    VIDEO_PLAYER_STATUS_PAUSED,
    VIDEO_PLAYER_STATUS_ENDED,
    VIDEO_PLAYER_STATUS_ERROR,
} video_player_status_t;

typedef enum {
    VIDEO_PLAYER_EVENT_MEDIA_READY = 0,
    VIDEO_PLAYER_EVENT_PLAY,
    VIDEO_PLAYER_EVENT_PAUSE,
    VIDEO_PLAYER_EVENT_STOP,
    VIDEO_PLAYER_EVENT_EOS,
    VIDEO_PLAYER_EVENT_ERROR,
} video_player_event_t;

/** @brief Playback state with borrowed media name. */
typedef struct {
    video_player_status_t status;
    const char *name;
    uint32_t duration_ms;
    uint32_t position_ms;
} video_player_state_t;

/** @brief Decoded frame dimensions are aligned to 16-pixel JPEG blocks on both axes. */
#define VIDEO_PLAYER_FRAME_ALIGN 16U

/**
 * @brief Round one decoded frame dimension up to the hardware JPEG block size.
 * @param value Value used by the operation.
 * @return Dimension rounded up to the JPEG block size.
 */
uint16_t video_player_align_dimension(uint16_t value);
/**
 * @brief Round both decoded frame dimensions up to the hardware JPEG block size.
 * @param[in,out] width Frame width in pixels, updated to the aligned value.
 * @param[in,out] height Frame height in pixels, updated to the aligned value.
 */
void video_player_align_resolution(uint16_t *width, uint16_t *height);

/**
 * @brief Check whether the decoded frame fits the panel limits without scaling.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param limit_width Maximum allowed width.
 * @param limit_height Maximum allowed height.
 * @return true if the decoded frame fits the panel limits.
 */
bool video_player_dimensions_supported(uint16_t width, uint16_t height,
                                       uint16_t limit_width, uint16_t limit_height);

/**
 * @brief Check for a nonempty filename with a case-insensitive .mp4 extension.
 * @param path Path to use.
 * @return true for a nonempty filename with a case-insensitive .mp4 extension.
 */
bool video_player_path_is_mp4(const char *path);
/**
 * @brief Initialize the player state to idle.
 * @param state State used by the operation.
 */
void video_player_state_init(video_player_state_t *state);
/**
 * @brief Set selected media metadata; the caller retains ownership of name.
 * @param state State used by the operation.
 * @param name Name to use.
 * @param duration_ms Duration in milliseconds.
 */
void video_player_state_set_media(video_player_state_t *state, const char *name, uint32_t duration_ms);
/**
 * @brief Update playback position in milliseconds.
 * @param state State used by the operation.
 * @param position_ms Playback position in milliseconds.
 */
void video_player_state_set_position(video_player_state_t *state, uint32_t position_ms);
/**
 * @brief Apply a supported player event and report whether it was accepted.
 * @param state State used by the operation.
 * @param event Event to process.
 * @return true if the event was accepted.
 */
bool video_player_state_apply(video_player_state_t *state, video_player_event_t event);
/**
 * @brief Check whether playback can begin or resume.
 * @param state State used by the operation.
 * @return true if playback can begin or resume; false otherwise.
 */
bool video_player_state_can_play(const video_player_state_t *state);
/**
 * @brief Check whether playback can be paused.
 * @param state State used by the operation.
 * @return true if playback can be paused; false otherwise.
 */
bool video_player_state_can_pause(const video_player_state_t *state);

#ifdef __cplusplus
}
#endif
