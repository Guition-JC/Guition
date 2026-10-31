#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Conversation lifecycle state. */
typedef enum {
    AICHAT_STATE_CLOSED = 0,
    AICHAT_STATE_STARTING,
    AICHAT_STATE_IDLE,
    AICHAT_STATE_CONNECTING,
    AICHAT_STATE_LISTENING,
    AICHAT_STATE_WAITING_RESPONSE,
    AICHAT_STATE_SPEAKING,
    AICHAT_STATE_ERROR,
    AICHAT_STATE_STOPPING,
    AICHAT_STATE_COUNT,
} aichat_state_t;

/** @brief Input event for the conversation state machine. */
typedef enum {
    AICHAT_EVENT_UI_OPEN = 0,
    AICHAT_EVENT_STARTUP_READY,
    AICHAT_EVENT_STARTUP_FAILED,
    AICHAT_EVENT_RETRY,
    AICHAT_EVENT_CONNECTED,
    AICHAT_EVENT_AUDIO_CHANNEL_OPENED,
    AICHAT_EVENT_AUDIO_CHANNEL_CLOSED,
    AICHAT_EVENT_WAKE_WORD,
    AICHAT_EVENT_START_LISTENING,
    AICHAT_EVENT_STOP_LISTENING,
    AICHAT_EVENT_VAD_END,
    AICHAT_EVENT_WAKE_WORD_INTERRUPT,
    AICHAT_EVENT_TTS_START,
    AICHAT_EVENT_TTS_STOP,
    AICHAT_EVENT_TURN_COMPLETE,
    AICHAT_EVENT_SERVER_GOODBYE,
    AICHAT_EVENT_TRANSPORT_ERROR,
    AICHAT_EVENT_UI_CLOSE,
    AICHAT_EVENT_SHUTDOWN_COMPLETE,
    AICHAT_EVENT_COUNT,
} aichat_event_t;

/** @brief Conversation transition result. */
typedef enum {
    AICHAT_STATE_RESULT_OK = 0,
    AICHAT_STATE_RESULT_INVALID_ARGUMENT,
    AICHAT_STATE_RESULT_INVALID_TRANSITION,
} aichat_state_result_t;

/**
 * @brief Pure state transition reducer. `next` is unchanged when the transition is rejected.
 * @param current Current value.
 * @param event Event to process.
 * @param next Destination for the next state.
 * @return Transition result; next is unchanged on rejection.
 */
aichat_state_result_t aichat_state_reduce(aichat_state_t current,
                                          aichat_event_t event,
                                          aichat_state_t *next);
/**
 * @brief Return the readable name of a conversation state.
 * @param state State used by the operation.
 * @return Readable name of the conversation state.
 */
const char *aichat_state_name(aichat_state_t state);

#ifdef __cplusplus
}
#endif
