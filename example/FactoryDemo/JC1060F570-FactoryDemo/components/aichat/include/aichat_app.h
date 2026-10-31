#pragma once

#include "aichat_history.h"
#include "aichat_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Conversation application state and owned history. */
typedef struct aichat_app aichat_app_t;

/** @brief Application operation result. */
typedef enum {
    AICHAT_APP_RESULT_OK = 0,
    AICHAT_APP_RESULT_INVALID_ARGUMENT,
    AICHAT_APP_RESULT_INVALID_STATE,
    AICHAT_APP_RESULT_NO_MEMORY,
} aichat_app_result_t;

/**
 * @brief Create an application instance with empty history and a closed state.
 * @param out_app Receives the created application.
 * @return Application creation status.
 */
aichat_app_result_t aichat_app_create(aichat_app_t **out_app);
/**
 * @brief Release the application instance and all owned history text.
 * @param app Application instance.
 */
void aichat_app_destroy(aichat_app_t *app);
/**
 * @brief Apply an event to the application state machine.
 * @param app Application instance.
 * @param event Event to process.
 * @return State-machine dispatch status.
 */
aichat_app_result_t aichat_app_dispatch(aichat_app_t *app, aichat_event_t event);
/**
 * @brief Copy a user or assistant message into application history.
 * @param app Application instance.
 * @param role Message sender role.
 * @param text Text to use.
 * @return Message append status.
 */
aichat_app_result_t aichat_app_add_message(aichat_app_t *app,
                                           aichat_message_role_t role,
                                           const char *text);
/**
 * @brief Release all messages retained by the application.
 * @param app Application instance.
 * @return History clearing status.
 */
aichat_app_result_t aichat_app_clear_history(aichat_app_t *app);
/**
 * @brief Return the current state, or AICHAT_STATE_COUNT for a NULL instance.
 * @param app Application instance.
 * @return The current state, or AICHAT_STATE_COUNT for a NULL instance.
 */
aichat_state_t aichat_app_state(const aichat_app_t *app);
/**
 * @brief Borrow the application history; the application retains ownership.
 * @param app Application instance.
 * @return Borrowed application history; the application retains ownership.
 */
const aichat_history_t *aichat_app_history_const(const aichat_app_t *app);

#ifdef __cplusplus
}
#endif
