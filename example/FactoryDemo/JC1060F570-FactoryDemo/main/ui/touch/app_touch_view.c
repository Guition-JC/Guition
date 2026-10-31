#include "app_touch_view.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "desktop_big_buffer.h"
#include "desktop_language.h"
#include "app_touch_gesture.h"

#define TAG "app_touch"
#define TOUCH_CANVAS_BG lv_color_hex(0xFFFFFF)
#define TOUCH_TOOL_BG lv_color_hex(0x101A2B)
#define TOUCH_TOOL_FG lv_color_hex(0xF8FBFF)
#define TOUCH_ACCENT lv_color_hex(0x147EFB)
#define TOUCH_EXIT_STRIP_HEIGHT 10
#define TOUCH_TOOL_MARGIN 12
#define TOUCH_BUTTON_SIZE 42
#define TOUCH_BUTTON_GAP 6
#define TOUCH_TOOL_HEIGHT 56
#define TOUCH_TOOL_HEIGHT_COMPACT 104

static const uint32_t s_color_hex[] = {
    0x111827,
    0xE5484D,
    0x147EFB,
    0x22A96B,
    0xED9D33,
};
static const int32_t s_widths[] = {2, 5, 8, 12};

static lv_obj_t *s_parent;
static lv_obj_t *s_canvas;
static lv_obj_t *s_toolbar;
static lv_obj_t *s_color_indicator;
static lv_obj_t *s_lock_button;
static lv_obj_t *s_hide_button;
static lv_obj_t *s_edge_handle;
static lv_obj_t *s_color_buttons[sizeof(s_color_hex) / sizeof(s_color_hex[0])];
static lv_obj_t *s_width_buttons[sizeof(s_widths) / sizeof(s_widths[0])];
static void *s_canvas_buffer;
static size_t s_canvas_buffer_size;
static uint16_t s_screen_width;
static uint16_t s_screen_height;
static uint16_t s_last_x;
static uint16_t s_last_y;
static uint8_t s_color_index;
static uint8_t s_width_index;
static bool s_drawing;
static bool s_edge_tracking;
static bool s_exit_requested;
static bool s_locked;
static bool s_toolbar_hidden;
static app_touch_gesture_t s_gesture;
static app_touch_view_close_cb_t s_close_cb;
static void *s_close_user_data;

static void style_plain(lv_obj_t *obj, lv_color_t bg, lv_opa_t opa, int32_t radius)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static void stop_tool_event_bubbling(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
}

static lv_color_t pen_color(void)
{
    return lv_color_hex(s_color_hex[s_color_index]);
}

/**
 * @brief Draw a stroke segment using the selected color and pen width.
 * @param x1 Start point x coordinate.
 * @param y1 Start point y coordinate.
 * @param x2 End point x coordinate.
 * @param y2 End point y coordinate.
 */
static void draw_segment(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    if (s_canvas == NULL) {
        return;
    }

    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = pen_color();
    dsc.width = s_widths[s_width_index];
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(&layer, &dsc);
    lv_canvas_finish_layer(s_canvas, &layer);
}

static void clear_canvas(void)
{
    if (s_canvas != NULL) {
        lv_canvas_fill_bg(s_canvas, TOUCH_CANVAS_BG, LV_OPA_COVER);
    }
}

static void update_color_indicator(void)
{
    if (s_color_indicator != NULL) {
        lv_obj_set_style_bg_color(s_color_indicator, pen_color(), 0);
    }
}

static void update_color_button_states(void)
{
    for (size_t i = 0; i < sizeof(s_color_buttons) / sizeof(s_color_buttons[0]); ++i) {
        if (s_color_buttons[i] == NULL) {
            continue;
        }
        if (i == s_color_index) {
            lv_obj_add_state(s_color_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_color_buttons[i], LV_STATE_CHECKED);
        }
    }
}

static void update_width_button_states(void)
{
    for (size_t i = 0; i < sizeof(s_width_buttons) / sizeof(s_width_buttons[0]); ++i) {
        if (s_width_buttons[i] == NULL) {
            continue;
        }
        if (i == s_width_index) {
            lv_obj_add_state(s_width_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_width_buttons[i], LV_STATE_CHECKED);
        }
    }
}

static void color_button_cb(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_target_obj(event);
    const uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && index < sizeof(s_color_hex) / sizeof(s_color_hex[0])) {
        s_color_index = (uint8_t)index;
        update_color_button_states();
        update_color_indicator();
    }
    if (button != NULL) {
        stop_tool_event_bubbling(event);
    }
}

static void width_button_cb(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_target_obj(event);
    const uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && index < sizeof(s_widths) / sizeof(s_widths[0])) {
        s_width_index = (uint8_t)index;
        update_width_button_states();
    }
    if (button != NULL) {
        stop_tool_event_bubbling(event);
    }
}

static void clear_button_cb(lv_event_t *event)
{
    clear_canvas();
    stop_tool_event_bubbling(event);
}

static void set_tool_button_text(lv_obj_t *button, const char *text)
{
    if (button != NULL && text != NULL) {
        lv_obj_t *label = lv_obj_get_child(button, 0);
        if (label != NULL) {
            lv_label_set_text(label, text);
        }
    }
}

static void lock_button_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        s_locked = !s_locked;
        if (s_locked) {
            s_edge_tracking = false;
            app_touch_gesture_end(&s_gesture);
        }
        set_tool_button_text(s_lock_button,
                             desktop_language_lookup(s_locked ? DESKTOP_TEXT_TOUCH_UNLOCK
                                                             : DESKTOP_TEXT_TOUCH_LOCK));
        lv_obj_set_state(s_lock_button, LV_STATE_CHECKED, s_locked);
    }
    stop_tool_event_bubbling(event);
}

static void show_toolbar_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        s_toolbar_hidden = false;
        lv_obj_remove_flag(s_toolbar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_color_indicator, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_toolbar);
    }
    stop_tool_event_bubbling(event);
}

static void hide_toolbar_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        s_toolbar_hidden = true;
        lv_obj_add_flag(s_toolbar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_color_indicator, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_color_indicator);
    }
    stop_tool_event_bubbling(event);
}

static void request_close(void)
{
    if (s_exit_requested || s_close_cb == NULL) {
        return;
    }
    s_exit_requested = true;
    s_close_cb(s_close_user_data);
}

/**
 * @brief Translate the bottom-edge gesture into a deferred close request.
 */
static void process_edge_update(void)
{
    if (s_locked || !s_edge_tracking || s_exit_requested || lv_indev_active() == NULL) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    if (point.x < 0 || point.y < 0) {
        return;
    }
    const app_touch_gesture_action_t action =
        app_touch_gesture_update(&s_gesture, (uint16_t)point.x, (uint16_t)point.y);
    if (action == APP_TOUCH_GESTURE_EXIT) {
        request_close();
    }
}

static bool point_from_event(lv_event_t *event, uint16_t *x, uint16_t *y)
{
    if (event == NULL || x == NULL || y == NULL || lv_indev_active() == NULL) {
        return false;
    }
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    if (point.x < 0 || point.y < 0 || point.x >= s_screen_width || point.y >= s_screen_height) {
        return false;
    }
    *x = (uint16_t)point.x;
    *y = (uint16_t)point.y;
    return true;
}

static void canvas_input_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    uint16_t x;
    uint16_t y;

    if (code == LV_EVENT_PRESSED) {
        if (!point_from_event(event, &x, &y)) {
            return;
        }
        s_edge_tracking = !s_locked && app_touch_gesture_begin(&s_gesture, x, y);
        s_drawing = !s_edge_tracking;
        s_exit_requested = false;
        if (s_drawing) {
            s_last_x = x;
            s_last_y = y;
            draw_segment(x, y, x, y);
        }
        return;
    }

    if (code == LV_EVENT_PRESSING || code == LV_EVENT_LONG_PRESSED) {
        process_edge_update();
        if (s_edge_tracking || s_exit_requested || !s_drawing ||
            !point_from_event(event, &x, &y)) {
            return;
        }
        if (x != s_last_x || y != s_last_y) {
            draw_segment(s_last_x, s_last_y, x, y);
            s_last_x = x;
            s_last_y = y;
        }
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_drawing = false;
        s_edge_tracking = false;
        app_touch_gesture_end(&s_gesture);
    }
}

static lv_obj_t *make_tool_button(lv_obj_t *parent, int32_t width, const char *text)
{
    lv_obj_t *button = lv_button_create(parent);
    style_plain(button, TOUCH_TOOL_BG, LV_OPA_COVER, 8);
    lv_obj_set_size(button, width, TOUCH_BUTTON_SIZE);
    lv_obj_set_style_border_width(button, 2, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(button, TOUCH_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(button, TOUCH_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(button, TOUCH_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, TOUCH_TOOL_FG, 0);
    lv_obj_add_event_cb(button, stop_tool_event_bubbling, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(button, stop_tool_event_bubbling, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(button, stop_tool_event_bubbling, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(button, stop_tool_event_bubbling, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, TOUCH_TOOL_FG, 0);
    lv_obj_center(label);
    return button;
}

/**
 * @brief Allocate the full-panel buffer with image-cache reclaim support.
 * @return true if the drawing buffer is available.
 */
static bool allocate_canvas_buffer(void)
{
    s_canvas_buffer_size = (size_t)s_screen_width * (size_t)s_screen_height *
                            lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);

    s_canvas_buffer = desktop_big_buffer_alloc(s_canvas_buffer_size);
    if (s_canvas_buffer == NULL) {
        s_canvas_buffer = heap_caps_malloc(s_canvas_buffer_size, MALLOC_CAP_8BIT);
    }
    if (s_canvas_buffer == NULL) {
        ESP_LOGE(TAG, "drawing canvas allocation failed: %u bytes", (unsigned)s_canvas_buffer_size);
        s_canvas_buffer_size = 0;
        return false;
    }
    return true;
}

static void build_toolbar(void)
{
    const int32_t toolbar_width = s_screen_width > 24 ? s_screen_width - 24 : s_screen_width;
    const int32_t toolbar_height = s_screen_width < 640 ? TOUCH_TOOL_HEIGHT_COMPACT : TOUCH_TOOL_HEIGHT;
    s_toolbar = lv_obj_create(s_parent);
    style_plain(s_toolbar, TOUCH_TOOL_BG, LV_OPA_90, 10);
    lv_obj_set_pos(s_toolbar, TOUCH_TOOL_MARGIN, TOUCH_TOOL_MARGIN);
    lv_obj_set_size(s_toolbar, toolbar_width, toolbar_height);
    lv_obj_set_style_pad_all(s_toolbar, 7, 0);
    lv_obj_set_style_pad_column(s_toolbar, TOUCH_BUTTON_GAP, 0);
    lv_obj_set_style_pad_row(s_toolbar, TOUCH_BUTTON_GAP, 0);
    lv_obj_set_flex_flow(s_toolbar, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_toolbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    for (size_t i = 0; i < sizeof(s_color_hex) / sizeof(s_color_hex[0]); ++i) {
        s_color_buttons[i] = make_tool_button(s_toolbar, TOUCH_BUTTON_SIZE, "");
        lv_obj_set_style_bg_color(s_color_buttons[i], lv_color_hex(s_color_hex[i]), 0);

        lv_obj_set_style_bg_color(s_color_buttons[i], lv_color_hex(s_color_hex[i]), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s_color_buttons[i], lv_color_hex(s_color_hex[i]), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(s_color_buttons[i], TOUCH_TOOL_FG, LV_STATE_CHECKED);
        lv_obj_add_event_cb(s_color_buttons[i], color_button_cb, LV_EVENT_ALL, (void *)(uintptr_t)i);
    }

    for (size_t i = 0; i < sizeof(s_widths) / sizeof(s_widths[0]); ++i) {
        char label[8];
        lv_snprintf(label, sizeof(label), "%d", (int)s_widths[i]);
        s_width_buttons[i] = make_tool_button(s_toolbar, TOUCH_BUTTON_SIZE, label);
        lv_obj_add_event_cb(s_width_buttons[i], width_button_cb, LV_EVENT_ALL, (void *)(uintptr_t)i);
    }

    lv_obj_t *clear_button = make_tool_button(s_toolbar, 76, desktop_language_lookup(DESKTOP_TEXT_TOUCH_CLEAR));
    lv_obj_add_event_cb(clear_button, clear_button_cb, LV_EVENT_CLICKED, NULL);

    s_lock_button = make_tool_button(s_toolbar, 70, desktop_language_lookup(DESKTOP_TEXT_TOUCH_LOCK));
    lv_obj_add_event_cb(s_lock_button, lock_button_cb, LV_EVENT_CLICKED, NULL);
    s_hide_button = make_tool_button(s_toolbar, 70, desktop_language_lookup(DESKTOP_TEXT_TOUCH_HIDE));
    lv_obj_add_event_cb(s_hide_button, hide_toolbar_cb, LV_EVENT_CLICKED, NULL);

    update_color_button_states();
    update_color_indicator();
    update_width_button_states();
}

/**
 * @brief Reset parent layout and create the canvas, exit zone and toolbar.
 * @param parent Parent object for the drawing surface.
 * @param screen_width Display width in pixels.
 * @param screen_height Display height in pixels.
 * @param close_cb Callback invoked to request closing the view.
 * @param user_data Context passed to the close callback.
 * @return true on successful creation, otherwise false.
 */
bool app_touch_view_build(lv_obj_t *parent, uint16_t screen_width, uint16_t screen_height,
                            app_touch_view_close_cb_t close_cb, void *user_data)
{
    if (parent == NULL || screen_width == 0 || screen_height == 0) {
        return false;
    }

    s_parent = parent;
    s_screen_width = screen_width;
    s_screen_height = screen_height;

    lv_obj_set_style_pad_all(parent, 0, 0);

    lv_obj_set_layout(parent, LV_LAYOUT_NONE);
    if (!allocate_canvas_buffer()) {
        s_parent = NULL;
        s_screen_width = 0;
        s_screen_height = 0;
        return false;
    }
    s_close_cb = close_cb;
    s_close_user_data = user_data;
    s_color_index = 0;
    s_width_index = 1;
    s_drawing = false;
    s_edge_tracking = false;
    s_exit_requested = false;
    s_locked = false;
    s_toolbar_hidden = false;
    app_touch_gesture_init(&s_gesture, screen_width, screen_height);

    s_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_canvas, s_canvas_buffer, screen_width, screen_height, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_size(s_canvas, screen_width, screen_height);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, canvas_input_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_canvas, canvas_input_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_canvas, canvas_input_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_canvas, canvas_input_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_canvas, canvas_input_cb, LV_EVENT_PRESS_LOST, NULL);
    clear_canvas();

    s_edge_handle = lv_obj_create(parent);

    style_plain(s_edge_handle, lv_color_hex(0xFFFFFF), LV_OPA_TRANSP, 0);
    const int32_t handle_width = screen_width;
    const int32_t handle_height = TOUCH_EXIT_STRIP_HEIGHT;
    lv_obj_set_size(s_edge_handle, handle_width, handle_height);
    lv_obj_set_pos(s_edge_handle, 0, screen_height - handle_height);
    lv_obj_clear_flag(s_edge_handle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_edge_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_edge_handle, canvas_input_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_edge_handle, canvas_input_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_edge_handle, canvas_input_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_edge_handle, canvas_input_cb, LV_EVENT_PRESS_LOST, NULL);

    s_color_indicator = lv_button_create(parent);
    style_plain(s_color_indicator, lv_color_hex(s_color_hex[s_color_index]), LV_OPA_COVER, 21);
    lv_obj_set_size(s_color_indicator, TOUCH_BUTTON_SIZE, TOUCH_BUTTON_SIZE);
    lv_obj_set_pos(s_color_indicator, screen_width > TOUCH_BUTTON_SIZE + TOUCH_TOOL_MARGIN
                                         ? screen_width - TOUCH_BUTTON_SIZE - TOUCH_TOOL_MARGIN
                                         : 0,
                   TOUCH_TOOL_MARGIN);
    lv_obj_set_style_border_width(s_color_indicator, 2, 0);
    lv_obj_set_style_border_color(s_color_indicator, TOUCH_TOOL_FG, 0);
    lv_obj_add_event_cb(s_color_indicator, show_toolbar_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_color_indicator, LV_OBJ_FLAG_HIDDEN);

    build_toolbar();
    lv_obj_remove_flag(s_toolbar, LV_OBJ_FLAG_HIDDEN);

    lv_obj_move_foreground(s_toolbar);
    ESP_LOGI(TAG, "drawing view ready: %ux%u, canvas=%u bytes",
             (unsigned)screen_width, (unsigned)screen_height, (unsigned)s_canvas_buffer_size);
    return true;
}

/**
 * @brief Delete the canvas before freeing its referenced drawing buffer.
 */
void app_touch_view_cleanup(void)
{
    if (s_canvas != NULL) {
        lv_obj_delete(s_canvas);
    }
    if (s_toolbar != NULL) {
        lv_obj_delete(s_toolbar);
    }
    if (s_color_indicator != NULL) {
        lv_obj_delete(s_color_indicator);
    }
    if (s_edge_handle != NULL) {
        lv_obj_delete(s_edge_handle);
    }
    if (s_canvas_buffer != NULL) {
        heap_caps_free(s_canvas_buffer);
    }
    s_canvas_buffer = NULL;
    s_canvas_buffer_size = 0;
    s_parent = NULL;
    s_canvas = NULL;
    s_toolbar = NULL;
    s_color_indicator = NULL;
    s_lock_button = NULL;
    s_hide_button = NULL;
    s_edge_handle = NULL;
    for (size_t i = 0; i < sizeof(s_color_buttons) / sizeof(s_color_buttons[0]); ++i) {
        s_color_buttons[i] = NULL;
    }
    for (size_t i = 0; i < sizeof(s_width_buttons) / sizeof(s_width_buttons[0]); ++i) {
        s_width_buttons[i] = NULL;
    }
    s_close_cb = NULL;
    s_close_user_data = NULL;
    s_drawing = false;
    s_edge_tracking = false;
    s_exit_requested = false;
    s_locked = false;
    s_toolbar_hidden = false;
}

bool app_touch_view_is_active(void)
{
    return s_canvas != NULL && s_canvas_buffer != NULL;
}
