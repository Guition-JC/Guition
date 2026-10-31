#include "music_bt_source.h"

#include <string.h>

static music_bt_source_error_t call_lifecycle(music_bt_source_lifecycle_fn_t function,
                                               void *context)
{
    return function == NULL ? MUSIC_PLAYER_ERROR_BACKEND : function(context);
}

static music_bt_source_error_t call_command(music_bt_source_command_fn_t function,
                                             void *context)
{
    return function == NULL ? MUSIC_PLAYER_ERROR_BACKEND : function(context);
}

static void record_first(music_bt_source_error_t *first,
                         music_bt_source_error_t result)
{
    if (*first == MUSIC_PLAYER_ERROR_OK && result != MUSIC_PLAYER_ERROR_OK) {
        *first = result;
    }
}

static music_bt_source_error_t rollback_open(music_bt_source_t *source,
                                              music_bt_source_error_t first)
{
    if (source->bt_audio_initialized) {
        record_first(&first, call_lifecycle(source->backend->bt_audio_deinit,
                                             source->backend_context));
        source->bt_audio_initialized = false;
    }
    if (source->host_initialized) {
        record_first(&first, call_lifecycle(source->backend->host_deinit,
                                             source->backend_context));
        source->host_initialized = false;
    }
    return first;
}

music_bt_source_error_t music_bt_source_init(music_bt_source_t *source,
                                               const music_bt_source_config_t *config)
{
    if (source == NULL || config == NULL || config->backend == NULL ||
        config->backend->host_init == NULL || config->backend->bt_audio_init == NULL ||
        config->backend->pipeline_io_start == NULL || config->backend->stream_stop == NULL ||
        config->backend->pipeline_io_destroy == NULL || config->backend->bt_audio_deinit == NULL ||
        config->backend->host_deinit == NULL) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }

    (void)memset(source, 0, sizeof(*source));
    music_bt_state_init(&source->state);
    source->backend = config->backend;
    source->backend_context = config->backend_context;
    source->event_cb = config->event_cb;
    source->event_context = config->event_context;
    source->configured = true;
    return MUSIC_PLAYER_ERROR_OK;
}

music_bt_source_error_t music_bt_source_open(music_bt_source_t *source)
{
    music_bt_source_error_t result;

    if (source == NULL || !source->configured) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }
    if (source->state.open) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }

    result = call_lifecycle(source->backend->host_init, source->backend_context);
    if (result != MUSIC_PLAYER_ERROR_OK) {
        return result;
    }
    source->host_initialized = true;

    result = call_lifecycle(source->backend->bt_audio_init, source->backend_context);
    if (result != MUSIC_PLAYER_ERROR_OK) {
        return rollback_open(source, result);
    }
    source->bt_audio_initialized = true;

    result = call_lifecycle(source->backend->pipeline_io_start, source->backend_context);
    if (result != MUSIC_PLAYER_ERROR_OK) {
        return rollback_open(source, result);
    }
    source->pipeline_io_started = true;
    (void)music_bt_state_begin(&source->state);
    return MUSIC_PLAYER_ERROR_OK;
}

music_bt_source_error_t music_bt_source_close(music_bt_source_t *source)
{
    music_bt_source_error_t first = MUSIC_PLAYER_ERROR_OK;

    if (source == NULL) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }
    if (!source->state.open && !source->host_initialized &&
        !source->bt_audio_initialized && !source->pipeline_io_started) {
        return MUSIC_PLAYER_ERROR_OK;
    }

    music_bt_state_close(&source->state);
    if (source->pipeline_io_started) {
        record_first(&first, call_lifecycle(source->backend->stream_stop,
                                             source->backend_context));
        record_first(&first, call_lifecycle(source->backend->pipeline_io_destroy,
                                             source->backend_context));
        source->pipeline_io_started = false;
    }
    if (source->bt_audio_initialized) {
        record_first(&first, call_lifecycle(source->backend->bt_audio_deinit,
                                             source->backend_context));
        source->bt_audio_initialized = false;
    }
    if (source->host_initialized) {
        record_first(&first, call_lifecycle(source->backend->host_deinit,
                                             source->backend_context));
        source->host_initialized = false;
    }
    return first;
}

music_bt_source_error_t music_bt_source_on_event(music_bt_source_t *source,
                                                   const music_bt_event_t *event,
                                                   music_bt_event_result_t *result)
{
    music_bt_event_result_t applied;

    if (result != NULL) {
        *result = MUSIC_BT_EVENT_DROPPED;
    }
    if (source == NULL || event == NULL) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }
    if (!source->state.open) {
        return MUSIC_PLAYER_ERROR_NOT_OPEN;
    }

    applied = music_bt_state_apply_event(&source->state, event);
    if (result != NULL) {
        *result = applied;
    }
    if (applied == MUSIC_BT_EVENT_APPLIED && source->event_cb != NULL) {
        source->event_cb(source->event_context, event, &source->state);
    }
    return MUSIC_PLAYER_ERROR_OK;
}

music_bt_source_error_t music_bt_source_command(music_bt_source_t *source,
                                                 music_bt_avrcp_command_t command)
{

    if (source == NULL) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }
    if (!source->state.open) {
        return MUSIC_PLAYER_ERROR_NOT_OPEN;
    }

    switch (command) {
        case MUSIC_BT_AVRCP_PREVIOUS:
            return call_command(source->backend->playback_prev, source->backend_context);
        case MUSIC_BT_AVRCP_PLAY_PAUSE:
            if (source->state.playing) {
                music_bt_source_error_t result =
                    call_command(source->backend->playback_pause, source->backend_context);
                if (result == MUSIC_PLAYER_ERROR_OK) {
                    source->state.playback_paused_latched = true;
                }
                return result;
            }
            music_bt_source_error_t result =
                call_command(source->backend->playback_play, source->backend_context);
            if (result == MUSIC_PLAYER_ERROR_OK) {
                source->state.playback_paused_latched = false;
                source->state.playback_start_pending = true;
            }
            return result;
        case MUSIC_BT_AVRCP_NEXT:
            return call_command(source->backend->playback_next, source->backend_context);
        default:
            return MUSIC_PLAYER_ERROR_INVALID_COMMAND;
    }
}

uint32_t music_bt_source_generation(const music_bt_source_t *source)
{
    return source == NULL ? 0U : source->state.generation;
}
