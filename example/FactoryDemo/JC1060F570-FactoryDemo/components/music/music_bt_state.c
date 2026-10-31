#include "music_bt_state.h"

#include <string.h>

static void copy_bounded(char *destination, size_t capacity, const char *source, const char *fallback)
{
    const char *value = (source != NULL && source[0] != '\0') ? source : fallback;
    size_t length = 0U;

    if (destination == NULL || capacity == 0U) {
        return;
    }
    if (value == NULL) {
        destination[0] = '\0';
        return;
    }
    while (length + 1U < capacity && value[length] != '\0') {
        destination[length] = value[length];
        ++length;
    }
    destination[length] = '\0';
}

static bool copy_if_changed(char *destination,
                            size_t capacity,
                            const char *source,
                            const char *fallback)
{
    char before[capacity];

    (void)memcpy(before, destination, capacity);
    copy_bounded(destination, capacity, source, fallback);
    return memcmp(before, destination, capacity) != 0;
}

static bool metadata_changed(music_bt_state_t *state, const music_bt_event_t *event)
{
    bool changed = false;

    if (event->device_name != NULL && event->device_name[0] != '\0') {
        changed = copy_if_changed(state->metadata.device_name,
                                  sizeof(state->metadata.device_name),
                                  event->device_name,
                                  "Unknown device") || changed;
    }
    if (event->title != NULL && event->title[0] != '\0') {
        changed = copy_if_changed(state->metadata.title,
                                  sizeof(state->metadata.title),
                                  event->title,
                                  "Unknown title") || changed;
    }
    if (event->artist != NULL && event->artist[0] != '\0') {
        changed = copy_if_changed(state->metadata.artist,
                                  sizeof(state->metadata.artist),
                                  event->artist,
                                  "Unknown artist") || changed;
    }
    if (event->album != NULL && event->album[0] != '\0') {
        changed = copy_if_changed(state->metadata.album,
                                  sizeof(state->metadata.album),
                                  event->album,
                                  "Unknown album") || changed;
    }
    return changed;
}

void music_bt_state_init(music_bt_state_t *state)
{
    if (state != NULL) {
        (void)memset(state, 0, sizeof(*state));
        state->connection = MUSIC_BT_CONNECTION_CLOSED;
        state->stream = MUSIC_BT_STREAM_STOPPED;
    }
}

uint32_t music_bt_state_begin(music_bt_state_t *state)
{
    if (state == NULL) {
        return 0U;
    }
    ++state->generation;
    if (state->generation == 0U) {
        ++state->generation;
    }
    state->open = true;
    state->connection = MUSIC_BT_CONNECTION_CLOSED;
    state->stream = MUSIC_BT_STREAM_STOPPED;
    state->playing = false;
    state->playback_status_known = false;
    state->playback_paused_latched = false;
    state->playback_start_pending = false;
    state->playback_stop_pending = false;
    state->delivered_events = 0U;
    copy_bounded(state->metadata.device_name, sizeof(state->metadata.device_name), NULL,
                 "Unknown device");
    copy_bounded(state->metadata.title, sizeof(state->metadata.title), NULL, "Unknown title");
    copy_bounded(state->metadata.artist, sizeof(state->metadata.artist), NULL, "Unknown artist");
    copy_bounded(state->metadata.album, sizeof(state->metadata.album), NULL, "Unknown album");
    return state->generation;
}

void music_bt_state_close(music_bt_state_t *state)
{
    if (state == NULL) {
        return;
    }
    state->open = false;
    state->connection = MUSIC_BT_CONNECTION_CLOSED;
    state->stream = MUSIC_BT_STREAM_STOPPED;
    state->playing = false;
    state->playback_status_known = false;
    state->playback_paused_latched = false;
    state->playback_start_pending = false;
    state->playback_stop_pending = false;
    ++state->generation;
    if (state->generation == 0U) {
        ++state->generation;
    }
}

music_bt_event_result_t music_bt_state_apply_event(music_bt_state_t *state,
                                                    const music_bt_event_t *event)
{
    bool changed = false;

    if (state == NULL || event == NULL || !state->open || event->generation != state->generation) {
        return MUSIC_BT_EVENT_DROPPED;
    }

    switch (event->type) {
        case MUSIC_BT_EVENT_CONNECTING:
            changed = state->connection != MUSIC_BT_CONNECTION_CONNECTING;
            state->connection = MUSIC_BT_CONNECTION_CONNECTING;
            break;
        case MUSIC_BT_EVENT_CONNECTED:
            changed = state->connection != MUSIC_BT_CONNECTION_CONNECTED;
            state->connection = MUSIC_BT_CONNECTION_CONNECTED;
            changed = metadata_changed(state, event) || changed;
            break;
        case MUSIC_BT_EVENT_DISCONNECTED:
            changed = state->connection != MUSIC_BT_CONNECTION_CLOSED ||
                      state->stream != MUSIC_BT_STREAM_STOPPED || state->playing ||
                      state->playback_stop_pending;
            state->connection = MUSIC_BT_CONNECTION_CLOSED;
            state->stream = MUSIC_BT_STREAM_STOPPED;
            state->playing = false;
            state->playback_status_known = false;
            state->playback_paused_latched = false;
            state->playback_start_pending = false;
            state->playback_stop_pending = false;
            break;
        case MUSIC_BT_EVENT_STREAM_STARTED:
            changed = state->stream != MUSIC_BT_STREAM_STARTED || state->playback_start_pending;
            state->stream = MUSIC_BT_STREAM_STARTED;
            if (state->playback_start_pending ||
                (!state->playback_paused_latched && !state->playback_status_known && !state->playing)) {
                /** A pending PLAYING status is committed only when A2DP confirms that the replacement stream is live. */
                state->playing = true;
                changed = true;
            }
            state->playback_start_pending = false;
            break;
        case MUSIC_BT_EVENT_STREAM_STOPPED:
            changed = state->stream != MUSIC_BT_STREAM_STOPPED || state->playing ||
                      state->playback_start_pending || state->playback_stop_pending;
            state->stream = MUSIC_BT_STREAM_STOPPED;
            state->playing = false;
            state->playback_start_pending = false;
            state->playback_stop_pending = false;
            break;
        case MUSIC_BT_EVENT_TRACK_CHANGED:
            copy_bounded(state->metadata.device_name, sizeof(state->metadata.device_name), NULL,
                         "Unknown device");
            copy_bounded(state->metadata.title, sizeof(state->metadata.title), NULL, "Unknown title");
            copy_bounded(state->metadata.artist, sizeof(state->metadata.artist), NULL, "Unknown artist");
            copy_bounded(state->metadata.album, sizeof(state->metadata.album), NULL, "Unknown album");
            changed = true;
            break;
        case MUSIC_BT_EVENT_METADATA:
            changed = metadata_changed(state, event);
            break;
        case MUSIC_BT_EVENT_PLAYBACK_STATUS:
            switch (event->playback_status) {
                case MUSIC_BT_PLAYBACK_STATUS_PLAYING:
                    state->playback_status_known = true;
                    if (state->stream == MUSIC_BT_STREAM_STARTED || !state->playback_paused_latched) {
                        changed = state->playing != true || state->playback_start_pending ||
                                  state->playback_stop_pending;
                        state->playback_paused_latched = false;
                        state->playback_start_pending = state->stream != MUSIC_BT_STREAM_STARTED;
                        state->playback_stop_pending = false;
                        state->playing = true;
                    } else {
                        /** Hold remote PLAYING intent until A2DP confirms the stream has started. */
                        changed = state->playing || !state->playback_start_pending;
                        state->playback_start_pending = true;
                        state->playback_stop_pending = false;
                        state->playing = false;
                    }
                    break;
                case MUSIC_BT_PLAYBACK_STATUS_PAUSED:
                    state->playback_status_known = true;
                    changed = state->playing != false || state->playback_start_pending ||
                              state->playback_stop_pending || !state->playback_paused_latched;
                    state->playback_paused_latched = true;
                    state->playback_start_pending = false;
                    state->playback_stop_pending = false;
                    state->playing = false;
                    break;
                case MUSIC_BT_PLAYBACK_STATUS_STOPPED:
                    state->playback_status_known = true;
                    state->playback_paused_latched = true;
                    state->playback_start_pending = false;
                    if (state->stream == MUSIC_BT_STREAM_STARTED) {
                        changed = !state->playback_stop_pending;
                        state->playback_stop_pending = true;
                    } else {
                        changed = state->playing != false || state->playback_stop_pending;
                        state->playing = false;
                        state->playback_stop_pending = false;
                    }
                    break;
                default:
                    state->playback_status_known = true;
                    state->playback_stop_pending = false;
                    if (event->playing && state->stream != MUSIC_BT_STREAM_STARTED &&
                        state->playback_paused_latched) {
                        changed = state->playing || !state->playback_start_pending;
                        state->playback_start_pending = false;
                        state->playing = false;
                    } else {
                        changed = state->playing != event->playing || state->playback_start_pending;
                        if (event->playing) {
                            state->playback_paused_latched = false;
                        }
                        state->playback_start_pending = false;
                        state->playing = event->playing;
                    }
                    break;
            }
            break;
        default:
            return MUSIC_BT_EVENT_DROPPED;
    }

    if (!changed) {
        return MUSIC_BT_EVENT_NOOP;
    }
    ++state->delivered_events;
    return MUSIC_BT_EVENT_APPLIED;
}

void music_bt_state_set_playing(music_bt_state_t *state, bool playing)
{
    if (state != NULL && state->open) {
        state->playing = playing;
    }
}
