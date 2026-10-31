#include "music_player_state.h"
#include "music_player_internal.h"

#include <string.h>

static void copy_bounded(char *destination, size_t capacity, const char *source)
{
    size_t length = 0U;

    if (destination == NULL || capacity == 0U) {
        return;
    }
    if (source != NULL) {
        while (length + 1U < capacity && source[length] != '\0') {
            ++length;
        }
        if (length > 0U) {
            memcpy(destination, source, length);
        }
    }
    destination[length] = '\0';
}

static void set_detail(music_player_state_t *state, const char *detail)
{
    copy_bounded(state->detail, sizeof(state->detail), detail);
}

static void clear_media(music_player_state_t *state)
{
    state->track_title[0] = '\0';
    state->artist[0] = '\0';
    state->album[0] = '\0';
    state->current_lyric[0] = '\0';
    state->next_lyric[0] = '\0';
    state->elapsed_ms = 0U;
    state->duration_ms = 0U;
    state->position_known = false;
}

static void copy_if_present(char *destination, size_t capacity, const char *source)
{
    if (source != NULL && source[0] != '\0') {
        copy_bounded(destination, capacity, source);
    }
}

static bool valid_source(music_player_source_t source)
{
    return source >= MUSIC_PLAYER_SOURCE_SD_CARD && source < MUSIC_PLAYER_SOURCE_COUNT;
}

static void advance_revision(music_player_state_t *state)
{
    ++state->revision;
}

void music_player_state_reset(music_player_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->source = MUSIC_PLAYER_SOURCE_SD_CARD;
    state->playback = MUSIC_PLAYER_PLAYBACK_CLOSED;
    state->last_error = MUSIC_PLAYER_ERROR_OK;
    set_detail(state, "closed");
}

music_player_error_t music_player_state_record_error(music_player_state_t *state,
                                                      music_player_error_t error,
                                                      const char *detail)
{
    if (state == NULL) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }
    state->last_error = error;
    set_detail(state, detail);
    advance_revision(state);
    return error;
}

music_player_error_t music_player_state_reduce(music_player_state_t *state,
                                                const music_player_state_event_t *event)
{
    if (state == NULL || event == NULL) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }

    switch (event->type) {
        case MUSIC_PLAYER_STATE_EVENT_OPEN:
            state->open = true;
            state->playback = MUSIC_PLAYER_PLAYBACK_PAUSED;
            state->last_error = MUSIC_PLAYER_ERROR_OK;
            set_detail(state, "ready");
            break;
        case MUSIC_PLAYER_STATE_EVENT_CLOSE:
            state->open = false;
            state->source = MUSIC_PLAYER_SOURCE_SD_CARD;
            state->playback = MUSIC_PLAYER_PLAYBACK_CLOSED;
            state->track_index = 0U;
            state->last_error = MUSIC_PLAYER_ERROR_OK;
            clear_media(state);
            set_detail(state, "closed");
            break;
        case MUSIC_PLAYER_STATE_EVENT_SOURCE_SELECTED:
            if (!state->open) {
                return MUSIC_PLAYER_ERROR_NOT_OPEN;
            }
            if (!valid_source(event->source)) {
                return MUSIC_PLAYER_ERROR_INVALID_SOURCE;
            }
            state->source = event->source;
            state->track_index = 0U;
            clear_media(state);
            state->playback = MUSIC_PLAYER_PLAYBACK_PAUSED;
            state->last_error = MUSIC_PLAYER_ERROR_OK;
            set_detail(state, "source selected");
            break;
        case MUSIC_PLAYER_STATE_EVENT_TRACK_SELECTED:
            if (!state->open) {
                return MUSIC_PLAYER_ERROR_NOT_OPEN;
            }
            state->track_index = event->track_index;
            clear_media(state);
            state->playback = MUSIC_PLAYER_PLAYBACK_PAUSED;
            state->last_error = MUSIC_PLAYER_ERROR_OK;
            set_detail(state, "track selected");
            break;
        case MUSIC_PLAYER_STATE_EVENT_TRACK_CHANGED:
            if (!state->open) {
                return MUSIC_PLAYER_ERROR_NOT_OPEN;
            }
            /** Track-change notifications carry no replacement media payload. Keep the previous media and position until valid title/lyric/time data arrives. */
            state->track_index = event->track_index;
            state->last_error = MUSIC_PLAYER_ERROR_OK;
            set_detail(state, "track changed");
            break;
        case MUSIC_PLAYER_STATE_EVENT_MEDIA_RESET:
            if (!state->open) {
                return MUSIC_PLAYER_ERROR_NOT_OPEN;
            }
            clear_media(state);
            state->last_error = MUSIC_PLAYER_ERROR_OK;
            set_detail(state, "media reset");
            break;
        case MUSIC_PLAYER_STATE_EVENT_PLAY:
            if (!state->open) {
                return MUSIC_PLAYER_ERROR_NOT_OPEN;
            }
            state->playback = MUSIC_PLAYER_PLAYBACK_PLAYING;
            state->last_error = MUSIC_PLAYER_ERROR_OK;
            set_detail(state, "playing");
            break;
        case MUSIC_PLAYER_STATE_EVENT_PAUSE:
            if (!state->open) {
                return MUSIC_PLAYER_ERROR_NOT_OPEN;
            }
            state->playback = MUSIC_PLAYER_PLAYBACK_PAUSED;
            state->last_error = MUSIC_PLAYER_ERROR_OK;
            set_detail(state, "paused");
            break;
        case MUSIC_PLAYER_STATE_EVENT_METADATA:
            if (!state->open) {
                return MUSIC_PLAYER_ERROR_NOT_OPEN;
            }
            if (event->media == NULL) {
                return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
            }
            /** NULL and empty fields are omitted; explicit resets use dedicated events. */
            copy_if_present(state->track_title, sizeof(state->track_title), event->media->title);
            copy_if_present(state->artist, sizeof(state->artist), event->media->artist);
            copy_if_present(state->album, sizeof(state->album), event->media->album);
            copy_if_present(state->current_lyric, sizeof(state->current_lyric), event->media->current_lyric);
            copy_if_present(state->next_lyric, sizeof(state->next_lyric), event->media->next_lyric);
            if (event->media->update_position) {
                state->elapsed_ms = event->media->elapsed_ms;
                state->position_known = true;
            }
            if (event->media->update_duration && event->media->duration_ms != 0U) {
                state->duration_ms = event->media->duration_ms;
            }
            if (state->duration_ms != 0U && state->elapsed_ms > state->duration_ms) {
                state->elapsed_ms = state->duration_ms;
            }
            state->last_error = MUSIC_PLAYER_ERROR_OK;
            set_detail(state, "metadata updated");
            break;
        case MUSIC_PLAYER_STATE_EVENT_PROGRESS:
            if (!state->open) {
                return MUSIC_PLAYER_ERROR_NOT_OPEN;
            }
            state->elapsed_ms = event->elapsed_ms;
            state->position_known = true;
            if (event->duration_ms != 0U) {
                state->duration_ms = event->duration_ms;
            }
            if (state->duration_ms != 0U && state->elapsed_ms > state->duration_ms) {
                state->elapsed_ms = state->duration_ms;
            }
            advance_revision(state);
            return MUSIC_PLAYER_ERROR_OK;
        case MUSIC_PLAYER_STATE_EVENT_BACKEND_ERROR:
            if (!state->open) {
                return MUSIC_PLAYER_ERROR_NOT_OPEN;
            }
            state->playback = MUSIC_PLAYER_PLAYBACK_ERROR;
            state->last_error = event->error == MUSIC_PLAYER_ERROR_OK ? MUSIC_PLAYER_ERROR_BACKEND : event->error;
            set_detail(state, event->detail == NULL ? "backend error" : event->detail);
            break;
        case MUSIC_PLAYER_STATE_EVENT_BACKEND_END:
            if (!state->open) {
                return MUSIC_PLAYER_ERROR_NOT_OPEN;
            }
            state->playback = MUSIC_PLAYER_PLAYBACK_ENDED;
            state->last_error = MUSIC_PLAYER_ERROR_END_OF_TRACK;
            set_detail(state, event->detail == NULL ? "track ended" : event->detail);
            break;
        default:
            return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }

    advance_revision(state);
    return MUSIC_PLAYER_ERROR_OK;
}

void music_player_state_snapshot(const music_player_state_t *state,
                                 music_player_snapshot_t *snapshot)
{
    if (state == NULL || snapshot == NULL) {
        return;
    }
    snapshot->initialized = state->initialized;
    snapshot->open = state->open;
    snapshot->source = state->source;
    snapshot->playback = state->playback;
    snapshot->last_error = state->last_error;
    snapshot->track_index = state->track_index;
    snapshot->revision = state->revision;
    snapshot->elapsed_ms = state->elapsed_ms;
    snapshot->duration_ms = state->duration_ms;
    snapshot->position_known = state->position_known;
    copy_bounded(snapshot->source_name, sizeof(snapshot->source_name), music_player_source_name(state->source));
    copy_bounded(snapshot->track_title, sizeof(snapshot->track_title), state->track_title);
    copy_bounded(snapshot->artist, sizeof(snapshot->artist), state->artist);
    copy_bounded(snapshot->album, sizeof(snapshot->album), state->album);
    copy_bounded(snapshot->current_lyric, sizeof(snapshot->current_lyric), state->current_lyric);
    copy_bounded(snapshot->next_lyric, sizeof(snapshot->next_lyric), state->next_lyric);
    copy_bounded(snapshot->detail, sizeof(snapshot->detail), state->detail);
    memset(&snapshot->cover_art, 0, sizeof(snapshot->cover_art));
}

const char *music_player_source_name(music_player_source_t source)
{
    switch (source) {
        case MUSIC_PLAYER_SOURCE_SD_CARD:
            return "sd-card";
        case MUSIC_PLAYER_SOURCE_CLASSIC_BT:
            return "classic-bt";
        default:
            return "invalid";
    }
}
