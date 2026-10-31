#include "aichat_history.h"

#include <stdlib.h>
#include <string.h>

void aichat_history_init(aichat_history_t *history)
{
    if (history != NULL) memset(history, 0, sizeof(*history));
}

void aichat_history_clear(aichat_history_t *history)
{
    if (history == NULL) return;
    for (size_t i = 0; i < history->count; ++i) {
        free(history->messages[i].text);
        history->messages[i].text = NULL;
    }
    history->count = 0U;
}

void aichat_history_deinit(aichat_history_t *history)
{
    aichat_history_clear(history);
}

aichat_history_result_t aichat_history_append(aichat_history_t *history,
                                              aichat_message_role_t role,
                                              const char *text)
{
    if (history == NULL || text == NULL || text[0] == '\0' ||
        (role != AICHAT_MESSAGE_USER && role != AICHAT_MESSAGE_AI))
        return AICHAT_HISTORY_RESULT_INVALID_ARGUMENT;
    char *copy = malloc(strlen(text) + 1U);
    if (copy == NULL) return AICHAT_HISTORY_RESULT_NO_MEMORY;
    strcpy(copy, text);
    if (history->count == AICHAT_HISTORY_CAPACITY) {
        free(history->messages[0].text);
        memmove(&history->messages[0], &history->messages[1],
                (AICHAT_HISTORY_CAPACITY - 1U) * sizeof(history->messages[0]));
        history->count = AICHAT_HISTORY_CAPACITY - 1U;
    }
    history->messages[history->count++] = (aichat_message_t){ .role = role, .text = copy };
    return AICHAT_HISTORY_RESULT_OK;
}

bool aichat_history_get(const aichat_history_t *history,
                        size_t index,
                        aichat_message_view_t *view)
{
    if (history == NULL || view == NULL || index >= history->count) return false;
    view->role = history->messages[index].role;
    view->text = history->messages[index].text;
    return true;
}
