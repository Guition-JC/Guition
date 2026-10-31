#include "app_serial_ui.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "desktop_language.h"
#include "desktop_fullscreen_layout.h"
#include "sdkconfig.h"
#include "serial_terminal.h"

#define L(key) desktop_language_lookup(key)

#define C_SURFACE     lv_color_hex(0xF7FAFD)
#define C_MUTED       lv_color_hex(0xE2ECF6)
#define C_INK         lv_color_hex(0x122033)
#define C_SYSTEM      lv_color_hex(0x101A2B)
#define C_SYSTEM_TEXT lv_color_hex(0xF8FBFF)
#define C_ACCENT      lv_color_hex(0x147EFB)
#define C_ACCENT_SOFT lv_color_hex(0xD9EAFF)
#define C_DIVIDER     lv_color_hex(0xD9E0E7)

#define C_SERIAL_RX  lv_color_hex(0x4CC9F0)
#define C_SERIAL_TX  lv_color_hex(0xF4A261)

#define SERIAL_INPUT_HEIGHT 96

typedef enum {
    SERIAL_COMMAND_STATUS,
    SERIAL_COMMAND_TOUCH,
    SERIAL_COMMAND_NETWORK,
    SERIAL_COMMAND_SEND,
    SERIAL_COMMAND_CLEAR,
} serial_command_id_t;

static lv_obj_t *s_terminal;
static lv_obj_t *s_log;
static lv_obj_t *s_input;
static lv_obj_t *s_keyboard;
static lv_timer_t *s_refresh_timer;
static unsigned int s_history_revision;

/** @brief Page-owned PSRAM history buffer, accessed only by the LVGL task. */
static char *s_history_text;
static app_serial_toast_cb_t s_show_toast;

static void style_surface(lv_obj_t *obj, lv_color_t bg, int radius)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, C_DIVIDER, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                             lv_color_t bg, lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    style_surface(button, bg, 12);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, h);
    lv_obj_set_style_bg_color(button, C_ACCENT_SOFT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(button, 2, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, C_INK, 0);
    lv_obj_center(label);
    return button;
}

/**
 * @brief Refresh terminal text when the history revision changes.
 * @param timer LVGL history refresh timer.
 */
static void serial_history_refresh(lv_timer_t *timer)
{
    (void)timer;
    if (s_terminal == NULL) {
        return;
    }
    unsigned int revision = serial_terminal_history_revision();
    if (revision == s_history_revision) {
        return;
    }
    s_history_revision = revision;
    lv_obj_clean(s_log);
    if (s_history_text == NULL ||
        serial_terminal_history_format(s_history_text, SERIAL_TERMINAL_HISTORY_TEXT_SIZE) == 0) {
        lv_obj_t *label = lv_label_create(s_log);
        lv_label_set_text(label, L(DESKTOP_TEXT_SERIAL_EMPTY));
        lv_obj_set_style_text_color(label, C_SYSTEM_TEXT, 0);
        lv_obj_set_width(label, LV_PCT(100));
        lv_obj_set_height(s_log, 24);
    } else {
        size_t y = 0;
        char *line = s_history_text;
        while (*line != '\0') {
            char *line_end = strchr(line, '\n');
            if (line_end != NULL) {
                *line_end = '\0';
            }
            lv_obj_t *label = lv_label_create(s_log);
            lv_label_set_text(label, line);
            lv_obj_set_style_text_color(label, strncmp(line, "[RX] ", 5) == 0
                                                 ? C_SERIAL_RX
                                                 : (strncmp(line, "[TX] ", 5) == 0 ? C_SERIAL_TX
                                                                                   : C_SYSTEM_TEXT),
                                       0);
            lv_obj_set_width(label, LV_PCT(100));
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, (int32_t)y);
            lv_obj_update_layout(label);
            y += (size_t)lv_obj_get_height(label) + 4;
            if (line_end == NULL) {
                break;
            }
            line = line_end + 1;
        }
        lv_obj_set_width(s_log, LV_PCT(100));
        lv_obj_set_height(s_log, (int32_t)y);
    }
    lv_obj_scroll_to_y(s_terminal, LV_COORD_MAX, LV_ANIM_OFF);
}

static void serial_keyboard_show(void)
{
    lv_keyboard_set_textarea(s_keyboard, s_input);
    lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
}

static void serial_input_focus_cb(lv_event_t *event)
{
    (void)event;
    serial_keyboard_show();
}

/**
 * @brief Ignore empty input and send the entered command to the terminal service.
 */
static void serial_send_input(void)
{
    const char *text = lv_textarea_get_text(s_input);

    if (text == NULL || text[0] == '\0') {
        s_show_toast(L(DESKTOP_TEXT_SERIAL_INPUT_REQUIRED));
        return;
    }
    esp_err_t err = serial_terminal_send(text);
    if (err != ESP_OK) {
        s_show_toast(L(DESKTOP_TEXT_SERIAL_UNAVAILABLE));
        return;
    }
    lv_textarea_set_text(s_input, "");
    serial_history_refresh(NULL);
    s_show_toast(L(DESKTOP_TEXT_SERIAL_SENT));
}

/**
 * @brief Hide the keyboard for taps outside the keyboard and command field.
 * @param event LVGL background event.
 */
static void serial_background_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (s_keyboard == NULL || target == s_keyboard) {
        return;
    }
    if (target == s_input) {
        serial_keyboard_show();
        return;
    }
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_keyboard, NULL);
}

static void serial_keyboard_cb(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
        case LV_EVENT_READY:
            serial_send_input();
            break;
        case LV_EVENT_CANCEL:
            lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
            lv_keyboard_set_textarea(s_keyboard, NULL);
            break;
        default:
            break;
    }
}

static void serial_command_cb(lv_event_t *event)
{
    serial_command_id_t command_id =
        (serial_command_id_t)(uintptr_t)lv_event_get_user_data(event);
    switch (command_id) {
        case SERIAL_COMMAND_STATUS:
            if (serial_terminal_send("status") != ESP_OK) {
                s_show_toast(L(DESKTOP_TEXT_SERIAL_UNAVAILABLE));
                return;
            }
            break;
        case SERIAL_COMMAND_TOUCH:
            if (serial_terminal_send("touch") != ESP_OK) {
                s_show_toast(L(DESKTOP_TEXT_SERIAL_UNAVAILABLE));
                return;
            }
            break;
        case SERIAL_COMMAND_NETWORK:
            if (serial_terminal_send("network") != ESP_OK) {
                s_show_toast(L(DESKTOP_TEXT_SERIAL_UNAVAILABLE));
                return;
            }
            break;
        case SERIAL_COMMAND_SEND:
            serial_send_input();
            return;
        case SERIAL_COMMAND_CLEAR:
            serial_terminal_history_clear();
            serial_history_refresh(NULL);
            return;
    }
    serial_history_refresh(NULL);
    s_show_toast(L(DESKTOP_TEXT_SERIAL_SENT));
}

/**
 * @brief Release the refresh timer, keyboard and history buffer.
 */
void app_serial_ui_destroy(void)
{
    if (s_refresh_timer != NULL) {
        lv_timer_delete(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    if (s_keyboard != NULL) {
        lv_obj_delete(s_keyboard);
        s_keyboard = NULL;
    }
    s_terminal = NULL;
    s_log = NULL;
    s_input = NULL;
    s_show_toast = NULL;
    heap_caps_free(s_history_text);
    s_history_text = NULL;
}

void app_serial_ui_build(lv_obj_t *body, lv_obj_t *screen, app_serial_toast_cb_t show_toast)
{
    if (s_history_text == NULL) {
        s_history_text = heap_caps_malloc(SERIAL_TERMINAL_HISTORY_TEXT_SIZE, MALLOC_CAP_SPIRAM);
        if (s_history_text == NULL) {
            ESP_LOGW("SerialUI", "terminal history buffer unavailable");
        }
    }
    s_show_toast = show_toast;
    const desktop_fullscreen_layout_colors_t colors = {
        .canvas = lv_color_hex(0xDCE8F5),
        .surface = C_SURFACE,
        .ink = C_INK,
        .ink_muted = lv_color_hex(0x53667B),
    };
    desktop_fullscreen_layout_t layout;
    if (!desktop_fullscreen_layout_create(body, &colors, "", "", "", &layout)) {
        return;
    }

    desktop_fullscreen_layout_remove_page_header(&layout);

    s_terminal = desktop_fullscreen_layout_create_panel(
        layout.content, C_SYSTEM, 69, 16, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_terminal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_terminal, LV_DIR_VER);
    lv_obj_set_style_pad_all(s_terminal, 16, 0);
    s_log = lv_obj_create(s_terminal);
    lv_obj_remove_style_all(s_log);
    lv_obj_set_width(s_log, LV_PCT(100));
    lv_obj_set_height(s_log, 24);
    lv_obj_clear_flag(s_log, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *controls = desktop_fullscreen_layout_create_panel(
        layout.content, colors.canvas, 30, 0, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(controls, 10, 0);
    s_input = lv_textarea_create(controls);
    style_surface(s_input, C_SURFACE, 10);
    lv_obj_set_width(s_input, LV_PCT(100));
    lv_textarea_set_one_line(s_input, true);
    lv_obj_set_height(s_input, SERIAL_INPUT_HEIGHT);
    {
        const lv_font_t *font = lv_obj_get_style_text_font(s_input, LV_PART_MAIN);
        const int32_t line_height = font != NULL ? lv_font_get_line_height(font) : 0;
        lv_obj_set_style_pad_top(s_input, (SERIAL_INPUT_HEIGHT - line_height) / 2, 0);
    }
    lv_obj_set_style_pad_left(s_input, 12, 0);
#if CONFIG_FACTORY_UART_ENABLE
    lv_textarea_set_max_length(s_input, CONFIG_FACTORY_UART_MESSAGE_MAX_LENGTH);
#else
    lv_textarea_set_max_length(s_input, 128);
#endif
    lv_textarea_set_placeholder_text(s_input, L(DESKTOP_TEXT_SERIAL_INPUT_HINT));
    lv_obj_add_event_cb(s_input, serial_input_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(screen, serial_background_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *send = make_button(controls, L(DESKTOP_TEXT_SERIAL_SEND), 0, 0, 1, 54,
                                 C_ACCENT_SOFT, serial_command_cb,
                                 (void *)(uintptr_t)SERIAL_COMMAND_SEND);
    lv_obj_set_width(send, LV_PCT(100));
    lv_obj_t *status = make_button(controls, L(DESKTOP_TEXT_APP_STATUS), 0, 0, 1, 48,
                                   C_MUTED, serial_command_cb,
                                   (void *)(uintptr_t)SERIAL_COMMAND_STATUS);
    lv_obj_set_width(status, LV_PCT(100));
    lv_obj_t *touch = make_button(controls, L(DESKTOP_TEXT_SERIAL_TOUCH_BUTTON), 0, 0, 1, 48,
                                  C_MUTED, serial_command_cb,
                                  (void *)(uintptr_t)SERIAL_COMMAND_TOUCH);
    lv_obj_set_width(touch, LV_PCT(100));
    lv_obj_t *network = make_button(controls, L(DESKTOP_TEXT_SERIAL_NETWORK_BUTTON), 0, 0, 1, 48,
                                    C_MUTED, serial_command_cb,
                                    (void *)(uintptr_t)SERIAL_COMMAND_NETWORK);
    lv_obj_set_width(network, LV_PCT(100));
    lv_obj_t *clear = make_button(controls, L(DESKTOP_TEXT_SERIAL_CLEAR), 0, 0, 1, 48,
                                  C_ACCENT_SOFT, serial_command_cb,
                                  (void *)(uintptr_t)SERIAL_COMMAND_CLEAR);
    lv_obj_set_width(clear, LV_PCT(100));

    s_keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(s_keyboard, LV_PCT(92), LV_PCT(40));
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_keyboard_set_textarea(s_keyboard, s_input);
    lv_obj_add_event_cb(s_keyboard, serial_keyboard_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, serial_keyboard_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    s_history_revision = UINT_MAX;
    serial_history_refresh(NULL);
    s_refresh_timer = lv_timer_create(serial_history_refresh, 200, NULL);
}
