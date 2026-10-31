#include "app_wifi_widgets.h"

/**
 * @brief Reset panel styles before applying its surface appearance.
 * @param object Panel to style.
 * @param color Background color.
 * @param radius Corner radius in pixels.
 */
void app_wifi_style_panel(lv_obj_t *object, lv_color_t color, int radius)
{
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

/**
 * @brief Let label text wrap within the flex container.
 * @param parent Parent object.
 * @param text Label text.
 * @param color Text color.
 * @return Created label object.
 */
lv_obj_t *app_wifi_make_flex_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_width(label, LV_PCT(100));
    return label;
}

/**
 * @brief Build the button, caption and optional click handler.
 * @param parent Parent object.
 * @param text Button caption.
 * @param x Horizontal position in pixels.
 * @param y Vertical position in pixels.
 * @param width Button width in pixels.
 * @param height Button height in pixels.
 * @param color Button background color.
 * @param callback Optional click callback.
 * @param user_data Context passed to the callback.
 * @return Created button object.
 */
lv_obj_t *app_wifi_make_button(lv_obj_t *parent, const char *text, int x, int y, int width, int height,
                                  lv_color_t color, lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    app_wifi_style_panel(button, color, 12);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, APP_WIFI_C_ACCENT_SOFT, LV_STATE_PRESSED);
    if (callback != NULL) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, APP_WIFI_C_INK, 0);
    lv_obj_center(label);
    return button;
}
