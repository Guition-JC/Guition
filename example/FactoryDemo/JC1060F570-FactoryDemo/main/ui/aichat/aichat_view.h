#pragma once

#include <stdbool.h>

#include "aichat_app.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque chat page context. */
typedef struct aichat_view aichat_view_t;

/** @brief User actions delivered to the chat controller. */
typedef enum {
    AICHAT_VIEW_INTENT_LISTEN = 0,
    AICHAT_VIEW_INTENT_STOP,
    AICHAT_VIEW_INTENT_RETRY,
    AICHAT_VIEW_INTENT_CLEAR,
} aichat_view_intent_t;

/**
 * @brief Receive an action from the chat page.
 * @param intent Action requested by the user.
 * @param user_data Context supplied when the view was created.
 */
typedef void (*aichat_view_intent_cb_t)(aichat_view_intent_t intent, void *user_data);

/**
 * @brief Create the chat page and its controls.
 * @param parent Parent LVGL object for the page.
 * @param app Chat session used to render state and messages.
 * @param intent_cb Callback for user actions, or NULL to ignore them.
 * @param user_data Context passed to the callback.
 * @return The created view, or NULL for invalid arguments or creation failure.
 */
aichat_view_t *aichat_view_create(lv_obj_t *parent,
                                  aichat_app_t *app,
                                  aichat_view_intent_cb_t intent_cb,
                                  void *user_data);
/**
 * @brief Refresh session state and messages.
 * @param view Chat page to update.
 * @return true on success, or false for an unavailable view.
 */
bool aichat_view_render(aichat_view_t *view);
/**
 * @brief Update the notice label.
 * @param view Chat page to update.
 * @param notice Notice text; NULL clears the label.
 * @return true on success, or false for an unavailable view.
 */
bool aichat_view_set_notice(aichat_view_t *view, const char *notice);
/**
 * @brief Delete the owned widget tree and release the view context.
 * @param view Chat page to destroy.
 */
void aichat_view_destroy(aichat_view_t *view);

#ifdef __cplusplus
}
#endif
