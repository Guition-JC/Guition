#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AICHAT_HISTORY_CAPACITY 50U

/** @brief Origin of a conversation message. */
typedef enum {
    AICHAT_MESSAGE_USER = 0,
    AICHAT_MESSAGE_AI,
} aichat_message_role_t;

/** @brief History update result. */
typedef enum {
    AICHAT_HISTORY_RESULT_OK = 0,
    AICHAT_HISTORY_RESULT_INVALID_ARGUMENT,
    AICHAT_HISTORY_RESULT_NO_MEMORY,
} aichat_history_result_t;

/** @brief History message with owned text storage. */
typedef struct {
    aichat_message_role_t role;
    char *text;
} aichat_message_t;

/** @brief Bounded chronological conversation history. */
typedef struct {
    aichat_message_t messages[AICHAT_HISTORY_CAPACITY];
    size_t count;
} aichat_history_t;

/** @brief Read-only message view with borrowed text. */
typedef struct {
    aichat_message_role_t role;
    const char *text;
} aichat_message_view_t;

/**
 * @brief Initialize an empty message history.
 * @param history History instance.
 */
void aichat_history_init(aichat_history_t *history);
/**
 * @brief Free all message text and reset the history count.
 * @param history History instance.
 */
void aichat_history_clear(aichat_history_t *history);
/**
 * @brief Release all history-owned messages.
 * @param history History instance.
 */
void aichat_history_deinit(aichat_history_t *history);
/**
 * @brief Copy a message into history, dropping the oldest when capacity is reached.
 * @param history History instance.
 * @param role Message sender role.
 * @param text Text to use.
 * @return History append status.
 */
aichat_history_result_t aichat_history_append(aichat_history_t *history,
                                              aichat_message_role_t role,
                                              const char *text);
/**
 * @brief Borrow a message by index.
 * @param view Receives text valid until the message is evicted or history is cleared.
 * @return false for an invalid index or argument.
 */
bool aichat_history_get(const aichat_history_t *history,
                        size_t index,
                        aichat_message_view_t *view);

#ifdef __cplusplus
}
#endif
