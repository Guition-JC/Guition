/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "app_ft_touch_view.h"

#include <stddef.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "desktop_language.h"

#define L(key) desktop_language_lookup(key)

#define FT_TOUCH_TAG "FactoryTouch"
#define FT_TOUCH_BG lv_color_hex(0xFFFFFF)
#define FT_TOUCH_DOT lv_color_hex(0x147EFB)

#define FT_TOUCH_DOT_SIZE 9
#define FT_TOUCH_EXIT_MARGIN 12
#define FT_TOUCH_EXIT_WIDTH 132
#define FT_TOUCH_EXIT_HEIGHT 46

static lv_obj_t *s_parent;
static lv_obj_t *s_canvas;
static lv_obj_t *s_exit_button;
static void *s_canvas_buffer;
static int32_t s_width;
static int32_t s_height;
static app_ft_touch_exit_cb_t s_exit_cb;
static void *s_exit_user_data;

static void ft_touch_style_plain(lv_obj_t *object, lv_color_t background, int32_t radius)
{
    lv_obj_remove_style_all(object);
    lv_obj_set_style_bg_color(object, background, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
}

/**
 * @brief Draw one bounded round mark per touch sample without stroke interpolation.
 * @param x Horizontal touch coordinate in pixels.
 * @param y Vertical touch coordinate in pixels.
 */
static void ft_touch_draw_dot(int32_t x, int32_t y)
{
    if (s_canvas == NULL) {
        return;
    }

    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = FT_TOUCH_DOT;
    dot.bg_opa = LV_OPA_COVER;
    dot.radius = FT_TOUCH_DOT_SIZE / 2;

    lv_area_t area;
    area.x1 = x - FT_TOUCH_DOT_SIZE / 2;
    area.y1 = y - FT_TOUCH_DOT_SIZE / 2;
    area.x2 = area.x1 + FT_TOUCH_DOT_SIZE - 1;
    area.y2 = area.y1 + FT_TOUCH_DOT_SIZE - 1;

    if (area.x1 < 0) {
        area.x1 = 0;
    }
    if (area.y1 < 0) {
        area.y1 = 0;
    }
    if (area.x2 > s_width - 1) {
        area.x2 = s_width - 1;
    }
    if (area.y2 > s_height - 1) {
        area.y2 = s_height - 1;
    }

    lv_draw_rect(&layer, &dot, &area);
    lv_canvas_finish_layer(s_canvas, &layer);
}

static bool ft_touch_point_from_event(lv_event_t *event, int32_t *x, int32_t *y)
{
    if (event == NULL || x == NULL || y == NULL || lv_indev_active() == NULL) {
        return false;
    }
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    if (point.x < 0 || point.y < 0 || point.x >= s_width || point.y >= s_height) {
        return false;
    }
    *x = point.x;
    *y = point.y;
    return true;
}

static void ft_touch_canvas_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) {
        return;
    }
    int32_t x = 0;
    int32_t y = 0;
    if (ft_touch_point_from_event(event, &x, &y)) {
        ft_touch_draw_dot(x, y);
    }
}

static void ft_touch_exit_cb(lv_event_t *event)
{
    (void)event;
    if (s_exit_cb != NULL) {
        s_exit_cb(s_exit_user_data);
    }
}

/**
 * @brief Create the exit control above the drawing canvas.
 * @param parent Parent LVGL object.
 */
static void ft_touch_build_exit_button(lv_obj_t *parent)
{
    s_exit_button = lv_button_create(parent);
    ft_touch_style_plain(s_exit_button, lv_color_hex(0x101A2B), 10);
    lv_obj_set_size(s_exit_button, FT_TOUCH_EXIT_WIDTH, FT_TOUCH_EXIT_HEIGHT);
    lv_obj_align(s_exit_button, LV_ALIGN_TOP_RIGHT, -FT_TOUCH_EXIT_MARGIN, FT_TOUCH_EXIT_MARGIN);
    lv_obj_set_style_bg_color(s_exit_button, lv_color_hex(0x147EFB), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_exit_button, ft_touch_exit_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *caption = lv_label_create(s_exit_button);
    lv_label_set_text(caption, L(DESKTOP_TEXT_FACTORY_EXIT_TOUCH));
    lv_obj_set_style_text_color(caption, lv_color_hex(0xF8FBFF), 0);
    lv_obj_center(caption);
}

/**
 * @brief Allocate the canvas in PSRAM, falling back to byte-addressable internal memory.
 * @param parent Parent LVGL object, with no padding.
 * @param width Canvas width in pixels.
 * @param height Canvas height in pixels.
 * @param exit_cb Callback invoked by the exit control.
 * @param user_data Context passed to exit_cb.
 * @return true on success; false for invalid dimensions, a null parent or allocation failure.
 */
bool app_ft_touch_view_build(lv_obj_t *parent, int32_t width, int32_t height,
                               app_ft_touch_exit_cb_t exit_cb, void *user_data)
{
    app_ft_touch_view_cleanup();
    if (parent == NULL || width <= 0 || height <= 0) {
        return false;
    }

    const size_t buffer_size =
        (size_t)width * (size_t)height * lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);
    s_canvas_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_canvas_buffer == NULL) {
        s_canvas_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_8BIT);
    }
    if (s_canvas_buffer == NULL) {
        ESP_LOGE(FT_TOUCH_TAG, "canvas allocation failed: %u bytes", (unsigned)buffer_size);
        return false;
    }

    s_parent = parent;
    s_width = width;
    s_height = height;
    s_exit_cb = exit_cb;
    s_exit_user_data = user_data;

    s_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_canvas, s_canvas_buffer, width, height, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_size(s_canvas, width, height);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, ft_touch_canvas_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_canvas, ft_touch_canvas_cb, LV_EVENT_PRESSING, NULL);
    lv_canvas_fill_bg(s_canvas, FT_TOUCH_BG, LV_OPA_COVER);

    ft_touch_build_exit_button(parent);

    if (s_exit_button != NULL) {
        lv_obj_move_foreground(s_exit_button);
    }

    ESP_LOGI(FT_TOUCH_TAG, "dots-only canvas ready: %dx%d (%u bytes)",
             (int)width, (int)height, (unsigned)buffer_size);
    return true;
}

void app_ft_touch_view_cleanup(void)
{
    if (s_canvas != NULL) {
        lv_obj_delete(s_canvas);
        s_canvas = NULL;
    }
    if (s_exit_button != NULL) {
        lv_obj_delete(s_exit_button);
        s_exit_button = NULL;
    }
    if (s_canvas_buffer != NULL) {
        heap_caps_free(s_canvas_buffer);
        s_canvas_buffer = NULL;
    }
    s_parent = NULL;
    s_width = 0;
    s_height = 0;
    s_exit_cb = NULL;
    s_exit_user_data = NULL;
}
