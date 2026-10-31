#include "desktop_fullscreen_layout.h"

#include <string.h>

#define DESKTOP_FULLSCREEN_SAFE_X_PERCENT 2
#define DESKTOP_FULLSCREEN_SAFE_TOP_PERCENT 3
#define DESKTOP_FULLSCREEN_SAFE_BOTTOM_PERCENT 2
#define DESKTOP_FULLSCREEN_HEADER_PERCENT 15
#define DESKTOP_FULLSCREEN_TITLE_PERCENT 64
#define DESKTOP_FULLSCREEN_ACTIONS_PERCENT 35
#define DESKTOP_FULLSCREEN_GAP 8

void desktop_fullscreen_layout_style_panel(lv_obj_t *object,
                                         lv_color_t background,
                                         int32_t radius)
{
    if (object == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(object, background, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *create_full_width_label(lv_obj_t *parent,
                                         const char *text,
                                         lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text == NULL ? "" : text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

bool desktop_fullscreen_layout_create(lv_obj_t *parent,
                                    const desktop_fullscreen_layout_colors_t *colors,
                                    const char *title,
                                    const char *subtitle,
                                    const char *status,
                                    desktop_fullscreen_layout_t *layout)
{
    if (parent == NULL || colors == NULL || layout == NULL) {
        return false;
    }

    memset(layout, 0, sizeof(*layout));
    layout->root = parent;
    desktop_fullscreen_layout_style_panel(parent, colors->canvas, 0);
    lv_obj_set_size(parent, LV_PCT(100), LV_PCT(100));
    lv_obj_update_layout(parent);

    const int32_t width = lv_obj_get_width(parent);
    const int32_t height = lv_obj_get_height(parent);
    lv_obj_set_style_pad_left(parent, width * DESKTOP_FULLSCREEN_SAFE_X_PERCENT / 100, 0);
    lv_obj_set_style_pad_right(parent, width * DESKTOP_FULLSCREEN_SAFE_X_PERCENT / 100, 0);
    lv_obj_set_style_pad_top(parent, height * DESKTOP_FULLSCREEN_SAFE_TOP_PERCENT / 100, 0);
    lv_obj_set_style_pad_bottom(parent, height * DESKTOP_FULLSCREEN_SAFE_BOTTOM_PERCENT / 100, 0);
    lv_obj_set_style_pad_row(parent, DESKTOP_FULLSCREEN_GAP, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    layout->header = lv_obj_create(parent);
    desktop_fullscreen_layout_style_panel(layout->header, colors->canvas, 0);
    lv_obj_set_width(layout->header, LV_PCT(100));
    lv_obj_set_height(layout->header, LV_PCT(DESKTOP_FULLSCREEN_HEADER_PERCENT));
    lv_obj_set_flex_flow(layout->header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(layout->header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    layout->title_block = lv_obj_create(layout->header);
    desktop_fullscreen_layout_style_panel(layout->title_block, colors->canvas, 0);
    lv_obj_set_width(layout->title_block, LV_PCT(DESKTOP_FULLSCREEN_TITLE_PERCENT));
    lv_obj_set_height(layout->title_block, LV_PCT(100));
    lv_obj_set_flex_flow(layout->title_block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(layout->title_block, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    layout->title = create_full_width_label(layout->title_block, title, colors->ink);
    lv_obj_set_height(layout->title, LV_PCT(35));
    layout->subtitle = create_full_width_label(layout->title_block, subtitle,
                                                colors->ink_muted);
    lv_obj_set_height(layout->subtitle, LV_PCT(35));
    layout->status = create_full_width_label(layout->title_block, status,
                                              colors->ink_muted);
    lv_obj_set_height(layout->status, LV_PCT(30));

    layout->actions = lv_obj_create(layout->header);
    desktop_fullscreen_layout_style_panel(layout->actions, colors->canvas, 0);
    lv_obj_set_width(layout->actions, LV_PCT(DESKTOP_FULLSCREEN_ACTIONS_PERCENT));
    lv_obj_set_height(layout->actions, LV_PCT(100));
    lv_obj_set_style_pad_column(layout->actions, DESKTOP_FULLSCREEN_GAP, 0);
    lv_obj_set_flex_flow(layout->actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(layout->actions, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    layout->content = lv_obj_create(parent);
    desktop_fullscreen_layout_style_panel(layout->content, colors->canvas, 0);
    lv_obj_set_width(layout->content, LV_PCT(100));
    lv_obj_set_flex_grow(layout->content, 1);
    lv_obj_set_style_pad_column(layout->content, DESKTOP_FULLSCREEN_GAP, 0);
    lv_obj_set_flex_flow(layout->content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(layout->content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return true;
}

void desktop_fullscreen_layout_remove_page_title(desktop_fullscreen_layout_t *layout)
{
    if (layout == NULL) {
        return;
    }
    if (layout->title != NULL) {
        lv_obj_add_flag(layout->title, LV_OBJ_FLAG_HIDDEN);
    }
    if (layout->subtitle != NULL) {
        lv_obj_add_flag(layout->subtitle, LV_OBJ_FLAG_HIDDEN);
    }
    if (layout->actions != NULL) {
        lv_obj_add_flag(layout->actions, LV_OBJ_FLAG_HIDDEN);
    }
    if (layout->title_block != NULL) {
        lv_obj_set_width(layout->title_block, LV_PCT(100));
    }
    if (layout->status != NULL) {
        /** Size the remaining status row to its content for vertical centring. */
        lv_obj_set_height(layout->status, LV_SIZE_CONTENT);
    }
}

void desktop_fullscreen_layout_remove_page_header(desktop_fullscreen_layout_t *layout)
{
    if (layout == NULL) {
        return;
    }
    if (layout->header != NULL) {
        /** Let the content fill the space released by the hidden header. */
        lv_obj_add_flag(layout->header, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *desktop_fullscreen_layout_create_panel(lv_obj_t *parent,
                                               lv_color_t background,
                                               int32_t width_percent,
                                               int32_t radius,
                                               lv_flex_flow_t flow)
{
    if (parent == NULL) {
        return NULL;
    }
    lv_obj_t *panel = lv_obj_create(parent);
    desktop_fullscreen_layout_style_panel(panel, background, radius);
    if (width_percent > 0) {
        lv_obj_set_width(panel, LV_PCT(width_percent));
    } else {
        lv_obj_set_flex_grow(panel, 1);
    }
    lv_obj_set_height(panel, LV_PCT(100));
    lv_obj_set_flex_flow(panel, flow);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    return panel;
}
