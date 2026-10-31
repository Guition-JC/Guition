#include "video_player_state.h"

#include <stddef.h>
#include <string.h>

static bool ascii_equal_ignore_case(char left, char right)
{
    if (left >= 'A' && left <= 'Z') {
        left = (char)(left - 'A' + 'a');
    }
    if (right >= 'A' && right <= 'Z') {
        right = (char)(right - 'A' + 'a');
    }
    return left == right;
}

uint16_t video_player_align_dimension(uint16_t value)
{
    if (value == 0U) {
        return 0U;
    }
    const uint32_t aligned = ((uint32_t)value + (VIDEO_PLAYER_FRAME_ALIGN - 1U)) &
                             ~(uint32_t)(VIDEO_PLAYER_FRAME_ALIGN - 1U);
    /** Clamp to the largest aligned dimension representable by uint16_t. */
    return aligned > UINT16_MAX
               ? (uint16_t)((uint32_t)UINT16_MAX & ~(uint32_t)(VIDEO_PLAYER_FRAME_ALIGN - 1U))
               : (uint16_t)aligned;
}

void video_player_align_resolution(uint16_t *width, uint16_t *height)
{
    if (width != NULL) {
        *width = video_player_align_dimension(*width);
    }
    if (height != NULL) {
        *height = video_player_align_dimension(*height);
    }
}

bool video_player_dimensions_supported(uint16_t width, uint16_t height,
                                       uint16_t limit_width, uint16_t limit_height)
{
    return width > 0U && height > 0U &&
           limit_width > 0U && limit_height > 0U &&
           width <= limit_width && height <= limit_height;
}

bool video_player_path_is_mp4(const char *path)
{
    if (path == NULL) {
        return false;
    }

    const size_t length = strlen(path);
    if (length <= 4U || path[length - 5U] == '/') {
        return false;
    }
    return ascii_equal_ignore_case(path[length - 4U], '.') &&
           ascii_equal_ignore_case(path[length - 3U], 'm') &&
           ascii_equal_ignore_case(path[length - 2U], 'p') &&
           ascii_equal_ignore_case(path[length - 1U], '4');
}

void video_player_state_init(video_player_state_t *state)
{
    if (state == NULL) {
        return;
    }
    *state = (video_player_state_t) {
        .status = VIDEO_PLAYER_STATUS_IDLE,
    };
}

void video_player_state_set_media(video_player_state_t *state, const char *name, uint32_t duration_ms)
{
    if (state == NULL) {
        return;
    }
    state->name = name;
    state->duration_ms = duration_ms;
    state->position_ms = 0U;
    state->status = name == NULL ? VIDEO_PLAYER_STATUS_IDLE : VIDEO_PLAYER_STATUS_READY;
}

void video_player_state_set_position(video_player_state_t *state, uint32_t position_ms)
{
    if (state == NULL) {
        return;
    }
    state->position_ms = state->duration_ms > 0U && position_ms > state->duration_ms
                             ? state->duration_ms
                             : position_ms;
}

bool video_player_state_can_play(const video_player_state_t *state)
{
    return state != NULL && state->name != NULL &&
           (state->status == VIDEO_PLAYER_STATUS_READY || state->status == VIDEO_PLAYER_STATUS_PAUSED ||
            state->status == VIDEO_PLAYER_STATUS_ENDED);
}

bool video_player_state_can_pause(const video_player_state_t *state)
{
    return state != NULL && state->status == VIDEO_PLAYER_STATUS_PLAYING;
}

bool video_player_state_apply(video_player_state_t *state, video_player_event_t event)
{
    if (state == NULL) {
        return false;
    }

    switch (event) {
    case VIDEO_PLAYER_EVENT_MEDIA_READY:
        if (state->name == NULL) return false;
        state->status = VIDEO_PLAYER_STATUS_READY;
        state->position_ms = 0U;
        return true;
    case VIDEO_PLAYER_EVENT_PLAY:
        if (!video_player_state_can_play(state)) return false;
        if (state->status == VIDEO_PLAYER_STATUS_ENDED) state->position_ms = 0U;
        state->status = VIDEO_PLAYER_STATUS_PLAYING;
        return true;
    case VIDEO_PLAYER_EVENT_PAUSE:
        if (!video_player_state_can_pause(state)) return false;
        state->status = VIDEO_PLAYER_STATUS_PAUSED;
        return true;
    case VIDEO_PLAYER_EVENT_STOP:
        if (state->name == NULL) return false;
        state->status = VIDEO_PLAYER_STATUS_READY;
        state->position_ms = 0U;
        return true;
    case VIDEO_PLAYER_EVENT_EOS:
        if (state->status != VIDEO_PLAYER_STATUS_PLAYING) return false;
        state->position_ms = state->duration_ms;
        state->status = VIDEO_PLAYER_STATUS_ENDED;
        return true;
    case VIDEO_PLAYER_EVENT_ERROR:
        state->status = VIDEO_PLAYER_STATUS_ERROR;
        return true;
    default:
        return false;
    }
}
