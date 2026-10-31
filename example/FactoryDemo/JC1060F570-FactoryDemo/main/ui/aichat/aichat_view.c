#include "aichat_view.h"

#include <stdio.h>
#include <stdlib.h>

#include "desktop_font.h"
#include "desktop_fullscreen_layout.h"
#include "desktop_language.h"

#define L(key) desktop_language_lookup(key)

#define AICHAT_COLOR_CANVAS lv_color_hex(0xDCE8F5)
#define AICHAT_COLOR_SURFACE lv_color_hex(0xF7FAFD)
#define AICHAT_COLOR_MUTED lv_color_hex(0xE2ECF6)
#define AICHAT_COLOR_INK lv_color_hex(0x122033)
#define AICHAT_COLOR_INK_MUTED lv_color_hex(0x53667B)
#define AICHAT_COLOR_ACCENT lv_color_hex(0x147EFB)
#define AICHAT_COLOR_ACCENT_SOFT lv_color_hex(0xD9EAFF)
#define AICHAT_COLOR_USER lv_color_hex(0xE8F1FF)
#define AICHAT_COLOR_AI lv_color_hex(0xE5F4EC)
#define AICHAT_COLOR_DIVIDER lv_color_hex(0xD9E0E7)

struct aichat_view;
typedef struct {
    struct aichat_view *view;
    aichat_view_intent_t intent;
} aichat_view_intent_context_t;

struct aichat_view {
    lv_obj_t *root;
    lv_obj_t *status;
    lv_obj_t *messages;
    lv_obj_t *listen_button;
    lv_obj_t *listen_label;
    lv_obj_t *retry_button;
    lv_obj_t *notice;
    aichat_app_t *app;
    aichat_view_intent_cb_t intent_cb;
    void *user_data;
    aichat_view_intent_context_t intent_contexts[4];
};

static void style_plain(lv_obj_t *obj, lv_color_t color, int radius)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static void style_surface(lv_obj_t *obj, lv_color_t color, int radius)
{
    style_plain(obj, color, radius);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, AICHAT_COLOR_DIVIDER, 0);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, lv_color_t color,
                       int x, int y, int width)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text == NULL ? "" : text);
    lv_obj_set_style_text_color(obj, color, 0);
    lv_obj_set_pos(obj, x, y);
    if (width > 0) {
        lv_obj_set_width(obj, width);
    }
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y,
                        int width, int height, lv_color_t color,
                        lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *obj = lv_button_create(parent);
    style_surface(obj, color, 12);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, AICHAT_COLOR_ACCENT_SOFT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, AICHAT_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(obj, 2, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t *caption = label(obj, text, AICHAT_COLOR_INK, 0, 0, width - 12);
    lv_obj_center(caption);
    return obj;
}

static void emit_intent(lv_event_t *event)
{
    aichat_view_intent_context_t *context = lv_event_get_user_data(event);
    if (context != NULL && context->view != NULL && context->view->intent_cb != NULL) {
        context->view->intent_cb(context->intent, context->view->user_data);
    }
}

static const char *state_label(aichat_state_t state)
{
    switch (state) {
        case AICHAT_STATE_STARTING: return L(DESKTOP_TEXT_AICHAT_STARTING);
        case AICHAT_STATE_IDLE: return L(DESKTOP_TEXT_AICHAT_WAITING);
        case AICHAT_STATE_CONNECTING: return L(DESKTOP_TEXT_AICHAT_CONNECTING);
        case AICHAT_STATE_LISTENING: return L(DESKTOP_TEXT_AICHAT_LISTENING);
        case AICHAT_STATE_WAITING_RESPONSE: return L(DESKTOP_TEXT_AICHAT_WAITING_RESPONSE);
        case AICHAT_STATE_SPEAKING: return L(DESKTOP_TEXT_AICHAT_SPEAKING);
        case AICHAT_STATE_ERROR: return L(DESKTOP_TEXT_AICHAT_ERROR);
        case AICHAT_STATE_STOPPING: return L(DESKTOP_TEXT_AICHAT_STOPPING);
        case AICHAT_STATE_CLOSED: return L(DESKTOP_TEXT_AICHAT_CLOSED);
        default: return L(DESKTOP_TEXT_AICHAT_ERROR);
    }
}

static void render_messages(aichat_view_t *view)
{
    const aichat_history_t *history = aichat_app_history_const(view->app);
    lv_obj_clean(view->messages);
    if (history == NULL || history->count == 0U) {
        lv_obj_t *empty = label(view->messages, L(DESKTOP_TEXT_AICHAT_EMPTY),
                                AICHAT_COLOR_INK_MUTED, 24, 118, 580);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    for (size_t index = 0U; index < history->count; ++index) {
        aichat_message_view_t message;
        if (!aichat_history_get(history, index, &message)) {
            continue;
        }
        const bool is_user = message.role == AICHAT_MESSAGE_USER;
        const int width = 470;
        const int x = is_user ? 16 : 164;
        const int y = (int)index * 58;
        lv_obj_t *bubble = lv_obj_create(view->messages);
        style_surface(bubble, is_user ? AICHAT_COLOR_USER : AICHAT_COLOR_AI, 14);
        lv_obj_set_pos(bubble, x, y);
        lv_obj_set_size(bubble, width, 50);
        lv_obj_t *role = label(bubble,
                               is_user ? L(DESKTOP_TEXT_AICHAT_USER) : L(DESKTOP_TEXT_AICHAT_AI),
                               is_user ? AICHAT_COLOR_ACCENT : lv_color_hex(0x198754),
                               14, 6, width - 28);
        lv_obj_set_style_text_font(role, desktop_font_get_primary(), 0);
        lv_obj_t *body = label(bubble, message.text, AICHAT_COLOR_INK, 76, 6, width - 92);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(body, 38);
    }
    lv_obj_scroll_to_y(view->messages, LV_COORD_MAX, LV_ANIM_OFF);
}

static const char *listen_text(aichat_state_t state)
{
    return state == AICHAT_STATE_LISTENING ? L(DESKTOP_TEXT_AICHAT_STOP) :
           L(DESKTOP_TEXT_AICHAT_START);
}

aichat_view_t *aichat_view_create(lv_obj_t *parent, aichat_app_t *app,
                                  aichat_view_intent_cb_t intent_cb, void *user_data)
{
    if (parent == NULL || app == NULL) {
        return NULL;
    }
    aichat_view_t *view = calloc(1, sizeof(*view));
    if (view == NULL) {
        return NULL;
    }
    view->app = app;
    view->intent_cb = intent_cb;
    view->user_data = user_data;
    view->root = lv_obj_create(parent);
    const desktop_fullscreen_layout_colors_t colors = {
        .canvas = AICHAT_COLOR_CANVAS,
        .surface = AICHAT_COLOR_SURFACE,
        .ink = AICHAT_COLOR_INK,
        .ink_muted = AICHAT_COLOR_INK_MUTED,
    };
    desktop_fullscreen_layout_t layout;
    if (!desktop_fullscreen_layout_create(view->root, &colors,
                                        "", "",
                                        state_label(aichat_app_state(app)), &layout)) {
        free(view);
        return NULL;
    }
    /** Display the current chat session state. */
    desktop_fullscreen_layout_remove_page_title(&layout);
    view->status = layout.status;
    lv_obj_set_style_text_color(view->status, AICHAT_COLOR_INK, 0);

    view->messages = desktop_fullscreen_layout_create_panel(
        layout.content, AICHAT_COLOR_SURFACE, 72, 16, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(view->messages, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(view->messages, LV_DIR_VER);
    lv_obj_set_style_pad_all(view->messages, 8, 0);

    lv_obj_t *controls = desktop_fullscreen_layout_create_panel(
        layout.content, AICHAT_COLOR_CANVAS, 27, 0, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(controls, 12, 0);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    view->intent_contexts[0] = (aichat_view_intent_context_t){
        .view = view, .intent = AICHAT_VIEW_INTENT_LISTEN,
    };
    view->intent_contexts[1] = (aichat_view_intent_context_t){
        .view = view, .intent = AICHAT_VIEW_INTENT_RETRY,
    };
    view->intent_contexts[2] = (aichat_view_intent_context_t){
        .view = view, .intent = AICHAT_VIEW_INTENT_CLEAR,
    };
    view->listen_button = button(controls, listen_text(aichat_app_state(app)), 0, 0,
                                 1, 58, AICHAT_COLOR_ACCENT_SOFT, emit_intent,
                                 &view->intent_contexts[0]);
    lv_obj_set_width(view->listen_button, LV_PCT(100));
    view->listen_label = lv_obj_get_child(view->listen_button, 0);
    view->retry_button = button(controls, L(DESKTOP_TEXT_AICHAT_RETRY), 0, 0,
                                1, 52, AICHAT_COLOR_MUTED, emit_intent,
                                &view->intent_contexts[1]);
    lv_obj_set_width(view->retry_button, LV_PCT(100));
    lv_obj_t *clear_button = button(controls, L(DESKTOP_TEXT_AICHAT_CLEAR), 0, 0,
                                    1, 52, AICHAT_COLOR_MUTED, emit_intent,
                                    &view->intent_contexts[2]);
    lv_obj_set_width(clear_button, LV_PCT(100));
    lv_obj_t *format_label = label(controls, L(DESKTOP_TEXT_AICHAT_AUDIO_FORMAT),
                                   AICHAT_COLOR_INK_MUTED, 0, 0, 1);
    lv_obj_set_width(format_label, LV_PCT(100));
    lv_obj_t *format_value = label(controls, "16000 Hz / 16 bit / 4 ch / MRMN",
                                   AICHAT_COLOR_INK, 0, 0, 1);
    lv_obj_set_width(format_value, LV_PCT(100));
    lv_label_set_long_mode(format_value, LV_LABEL_LONG_WRAP);
    view->notice = label(controls, "", lv_color_hex(0xB45309), 0, 0, 1);
    lv_obj_set_width(view->notice, LV_PCT(100));
    lv_label_set_long_mode(view->notice, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(view->notice, 72);
    aichat_view_render(view);
    return view;
}

bool aichat_view_set_notice(aichat_view_t *view, const char *notice)
{
    if (view == NULL || view->notice == NULL) {
        return false;
    }
    lv_label_set_text(view->notice, notice == NULL ? "" : notice);
    return true;
}

bool aichat_view_render(aichat_view_t *view)
{
    if (view == NULL || view->app == NULL || view->status == NULL) {
        return false;
    }
    const aichat_state_t state = aichat_app_state(view->app);
    lv_label_set_text(view->status, state_label(state));
    if (view->listen_label != NULL) {
        lv_label_set_text(view->listen_label, listen_text(state));
    }
    lv_obj_add_flag(view->retry_button, state == AICHAT_STATE_ERROR ? 0 : LV_OBJ_FLAG_HIDDEN);
    render_messages(view);
    return true;
}

void aichat_view_destroy(aichat_view_t *view)
{
    if (view == NULL) {
        return;
    }
    if (view->root != NULL) {
        lv_obj_del(view->root);
    }
    free(view);
}
