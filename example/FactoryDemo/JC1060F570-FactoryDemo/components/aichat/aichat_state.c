#include "aichat_state.h"

#include <stddef.h>

static int is_valid_state(aichat_state_t state)
{
    return state >= AICHAT_STATE_CLOSED && state < AICHAT_STATE_COUNT;
}

static int is_valid_event(aichat_event_t event)
{
    return event >= AICHAT_EVENT_UI_OPEN && event < AICHAT_EVENT_COUNT;
}

static aichat_state_result_t transition(aichat_state_t current,
                                        aichat_event_t event,
                                        aichat_state_t *next)
{
    switch (current) {
        case AICHAT_STATE_CLOSED:
            if (event == AICHAT_EVENT_UI_OPEN) *next = AICHAT_STATE_STARTING;
            else return AICHAT_STATE_RESULT_INVALID_TRANSITION;
            break;
        case AICHAT_STATE_STARTING:
            if (event == AICHAT_EVENT_STARTUP_READY) *next = AICHAT_STATE_IDLE;
            else if (event == AICHAT_EVENT_STARTUP_FAILED) *next = AICHAT_STATE_ERROR;
            else if (event == AICHAT_EVENT_UI_CLOSE) *next = AICHAT_STATE_STOPPING;
            else return AICHAT_STATE_RESULT_INVALID_TRANSITION;
            break;
        case AICHAT_STATE_IDLE:
            if (event == AICHAT_EVENT_WAKE_WORD)
                *next = AICHAT_STATE_CONNECTING;
            else if (event == AICHAT_EVENT_START_LISTENING)
                *next = AICHAT_STATE_LISTENING;
            else if (event == AICHAT_EVENT_UI_CLOSE) *next = AICHAT_STATE_STOPPING;
            else if (event == AICHAT_EVENT_TRANSPORT_ERROR) *next = AICHAT_STATE_ERROR;
            else return AICHAT_STATE_RESULT_INVALID_TRANSITION;
            break;
        case AICHAT_STATE_CONNECTING:
            if (event == AICHAT_EVENT_AUDIO_CHANNEL_OPENED) *next = AICHAT_STATE_LISTENING;
            else if (event == AICHAT_EVENT_TRANSPORT_ERROR || event == AICHAT_EVENT_AUDIO_CHANNEL_CLOSED)
                *next = AICHAT_STATE_ERROR;
            else if (event == AICHAT_EVENT_UI_CLOSE) *next = AICHAT_STATE_STOPPING;
            else return AICHAT_STATE_RESULT_INVALID_TRANSITION;
            break;
        case AICHAT_STATE_LISTENING:
            /** Accept TTS start before or after the local VAD-end event. */
            if (event == AICHAT_EVENT_TTS_START)
                *next = AICHAT_STATE_SPEAKING;
            else if (event == AICHAT_EVENT_STOP_LISTENING || event == AICHAT_EVENT_VAD_END ||
                     event == AICHAT_EVENT_TURN_COMPLETE)
                *next = AICHAT_STATE_WAITING_RESPONSE;
            else if (event == AICHAT_EVENT_WAKE_WORD_INTERRUPT)
                *next = AICHAT_STATE_LISTENING;
            else if (event == AICHAT_EVENT_SERVER_GOODBYE)
                *next = AICHAT_STATE_IDLE;
            else if (event == AICHAT_EVENT_TRANSPORT_ERROR || event == AICHAT_EVENT_AUDIO_CHANNEL_CLOSED)
                *next = AICHAT_STATE_ERROR;
            else if (event == AICHAT_EVENT_UI_CLOSE) *next = AICHAT_STATE_STOPPING;
            else return AICHAT_STATE_RESULT_INVALID_TRANSITION;
            break;
        case AICHAT_STATE_WAITING_RESPONSE:
            if (event == AICHAT_EVENT_TTS_START) *next = AICHAT_STATE_SPEAKING;
            else if (event == AICHAT_EVENT_TTS_STOP)
                *next = AICHAT_STATE_LISTENING;
            else if (event == AICHAT_EVENT_WAKE_WORD_INTERRUPT)
                *next = AICHAT_STATE_LISTENING;
            else if (event == AICHAT_EVENT_SERVER_GOODBYE)
                *next = AICHAT_STATE_IDLE;
            else if (event == AICHAT_EVENT_TRANSPORT_ERROR) *next = AICHAT_STATE_ERROR;
            else if (event == AICHAT_EVENT_UI_CLOSE) *next = AICHAT_STATE_STOPPING;
            else return AICHAT_STATE_RESULT_INVALID_TRANSITION;
            break;
        case AICHAT_STATE_SPEAKING:
            /** A completed reply immediately returns to hands-free listening. */
            if (event == AICHAT_EVENT_TTS_STOP) *next = AICHAT_STATE_LISTENING;
            else if (event == AICHAT_EVENT_WAKE_WORD_INTERRUPT)
                *next = AICHAT_STATE_LISTENING;
            else if (event == AICHAT_EVENT_SERVER_GOODBYE)
                *next = AICHAT_STATE_IDLE;
            else if (event == AICHAT_EVENT_TRANSPORT_ERROR) *next = AICHAT_STATE_ERROR;
            else if (event == AICHAT_EVENT_UI_CLOSE) *next = AICHAT_STATE_STOPPING;
            else return AICHAT_STATE_RESULT_INVALID_TRANSITION;
            break;
        case AICHAT_STATE_ERROR:
            if (event == AICHAT_EVENT_RETRY) *next = AICHAT_STATE_STARTING;
            else if (event == AICHAT_EVENT_UI_CLOSE) *next = AICHAT_STATE_STOPPING;
            else return AICHAT_STATE_RESULT_INVALID_TRANSITION;
            break;
        case AICHAT_STATE_STOPPING:
            if (event == AICHAT_EVENT_SHUTDOWN_COMPLETE) *next = AICHAT_STATE_CLOSED;
            else return AICHAT_STATE_RESULT_INVALID_TRANSITION;
            break;
        default:
            return AICHAT_STATE_RESULT_INVALID_ARGUMENT;
    }
    return AICHAT_STATE_RESULT_OK;
}

aichat_state_result_t aichat_state_reduce(aichat_state_t current,
                                          aichat_event_t event,
                                          aichat_state_t *next)
{
    if (next == NULL || !is_valid_state(current) || !is_valid_event(event))
        return AICHAT_STATE_RESULT_INVALID_ARGUMENT;
    *next = current;
    return transition(current, event, next);
}

const char *aichat_state_name(aichat_state_t state)
{
    static const char *const names[] = {
        "closed", "starting", "idle", "connecting", "listening",
        "waiting_response", "speaking", "error", "stopping",
    };
    return is_valid_state(state) ? names[state] : "unknown";
}
