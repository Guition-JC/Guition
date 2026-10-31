#include "i2ctools_view_widgets.h"

#define C_CARD lv_color_hex(0xF7FAFD)
#define C_INK lv_color_hex(0x122033)
#define C_MUTED lv_color_hex(0x53667B)
#define C_BUTTON lv_color_hex(0x124E78)
#define C_BUTTON_TEXT lv_color_hex(0xFFFFFF)
#define C_ACCENT lv_color_hex(0x0B4F6C)

void i2ctools_view_plain(lv_obj_t *obj, lv_color_t color, int radius) {
  lv_obj_remove_style_all(obj);
  lv_obj_set_style_bg_color(obj, color, 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(obj, radius, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_set_layout(obj, LV_LAYOUT_NONE);
}

lv_obj_t *i2ctools_view_label(lv_obj_t *parent, const char *text,
                              lv_color_t color, int x, int y) {
  lv_obj_t *obj = lv_label_create(parent);
  lv_label_set_text(obj, text);
  lv_obj_set_style_text_color(obj, color, 0);
  lv_obj_set_pos(obj, x, y);
  return obj;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             int width_percent, int height_percent,
                             lv_event_cb_t callback) {
  lv_obj_t *obj = lv_button_create(parent);
  i2ctools_view_plain(obj, C_BUTTON, 10);
  lv_obj_set_width(obj, LV_PCT(width_percent > 0 ? width_percent : 100));
  lv_obj_set_height(obj,
                    LV_PCT(height_percent > 0 ? height_percent : 100));
  lv_obj_set_style_bg_color(obj, C_ACCENT, LV_STATE_PRESSED);
  lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, NULL);
  lv_obj_t *caption = lv_label_create(obj);
  lv_label_set_text(caption, text);
  lv_obj_set_style_text_color(caption, C_BUTTON_TEXT, 0);
  lv_obj_center(caption);
  return obj;
}

lv_obj_t *i2ctools_view_flex_button(lv_obj_t *parent, const char *text,
                                    int width_percent, int height_percent,
                                    lv_event_cb_t callback) {
  return make_button(parent, text, width_percent, height_percent, callback);
}

static void style_text_field(lv_obj_t *field, const char *placeholder) {
  i2ctools_view_plain(field, C_CARD, 8);
  lv_obj_set_style_border_width(field, 1, 0);
  lv_obj_set_style_border_color(field, C_ACCENT, 0);
  lv_obj_set_style_text_color(field, C_INK, 0);
  /** Highlight the active touch input field. */
  lv_obj_set_style_bg_color(field, lv_color_hex(0xE4F2FF), LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(field, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
  lv_obj_set_style_border_color(field, C_ACCENT, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(field, 3, LV_STATE_FOCUSED);
  lv_obj_set_style_outline_color(field, C_ACCENT, LV_STATE_FOCUSED);
  lv_obj_set_style_outline_width(field, 2, LV_STATE_FOCUSED);
  lv_obj_set_style_outline_pad(field, 2, LV_STATE_FOCUSED);
  lv_obj_set_style_text_color(field, C_ACCENT, LV_STATE_FOCUSED);
  lv_obj_set_style_pad_all(field, 2, 0);
  lv_textarea_set_one_line(field, true);
  lv_obj_t *label = lv_textarea_get_label(field);
  lv_obj_set_width(label, LV_PCT(100));
  lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
  lv_textarea_set_cursor_click_pos(field, true);
  if (placeholder != NULL) {
    lv_textarea_set_placeholder_text(field, placeholder);
  }
}

lv_obj_t *i2ctools_view_flex_text_field(lv_obj_t *parent, const char *text,
                                        int width_percent, int height_percent,
                                        const char *placeholder) {
  lv_obj_t *group = lv_obj_create(parent);
  i2ctools_view_plain(group, C_CARD, 0);
  lv_obj_set_width(group,
                   LV_PCT(width_percent > 0 ? width_percent : 100));
  lv_obj_set_height(group,
                    LV_PCT(height_percent > 0 ? height_percent : 100));
  lv_obj_set_layout(group, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_all(group, 0, 0);

  lv_obj_t *label = lv_label_create(group);
  lv_label_set_text(label, text);
  lv_obj_set_width(label, LV_PCT(100));
  lv_obj_set_height(label, LV_PCT(25));
  lv_obj_set_style_text_color(label, C_MUTED, 0);

  lv_obj_t *field = lv_textarea_create(group);
  style_text_field(field, placeholder);
  lv_obj_set_width(field, LV_PCT(100));
  lv_obj_set_height(field, LV_PCT(75));
  return field;
}
