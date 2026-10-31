#include "aichat_app.h"

#include <stdlib.h>

struct aichat_app {
    aichat_state_t state;
    aichat_history_t history;
};

aichat_app_result_t aichat_app_create(aichat_app_t **out_app)
{
    if (out_app == NULL) return AICHAT_APP_RESULT_INVALID_ARGUMENT;
    *out_app = calloc(1, sizeof(**out_app));
    if (*out_app == NULL) return AICHAT_APP_RESULT_NO_MEMORY;
    (*out_app)->state = AICHAT_STATE_CLOSED;
    aichat_history_init(&(*out_app)->history);
    return AICHAT_APP_RESULT_OK;
}

void aichat_app_destroy(aichat_app_t *app)
{
    if (app == NULL) return;
    aichat_history_deinit(&app->history);
    free(app);
}

aichat_app_result_t aichat_app_dispatch(aichat_app_t *app, aichat_event_t event)
{
    if (app == NULL) return AICHAT_APP_RESULT_INVALID_ARGUMENT;
    aichat_state_t next;
    const aichat_state_result_t result = aichat_state_reduce(app->state, event, &next);
    if (result == AICHAT_STATE_RESULT_INVALID_ARGUMENT) return AICHAT_APP_RESULT_INVALID_ARGUMENT;
    if (result == AICHAT_STATE_RESULT_INVALID_TRANSITION) {
        return AICHAT_APP_RESULT_INVALID_STATE;
    }
    app->state = next;
    return AICHAT_APP_RESULT_OK;
}

aichat_app_result_t aichat_app_add_message(aichat_app_t *app,
                                           aichat_message_role_t role,
                                           const char *text)
{
    if (app == NULL) return AICHAT_APP_RESULT_INVALID_ARGUMENT;
    const aichat_history_result_t result = aichat_history_append(&app->history, role, text);
    if (result == AICHAT_HISTORY_RESULT_INVALID_ARGUMENT) return AICHAT_APP_RESULT_INVALID_ARGUMENT;
    if (result == AICHAT_HISTORY_RESULT_NO_MEMORY) return AICHAT_APP_RESULT_NO_MEMORY;
    return AICHAT_APP_RESULT_OK;
}

aichat_app_result_t aichat_app_clear_history(aichat_app_t *app)
{
    if (app == NULL) return AICHAT_APP_RESULT_INVALID_ARGUMENT;
    aichat_history_clear(&app->history);
    return AICHAT_APP_RESULT_OK;
}

aichat_state_t aichat_app_state(const aichat_app_t *app)
{
    return app == NULL ? AICHAT_STATE_COUNT : app->state;
}

const aichat_history_t *aichat_app_history_const(const aichat_app_t *app)
{
    return app == NULL ? NULL : &app->history;
}
