#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_color_t canvas;
    lv_color_t surface;
    lv_color_t ink;
    lv_color_t ink_muted;
} desktop_fullscreen_layout_colors_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *header;
    lv_obj_t *title_block;
    lv_obj_t *actions;
    lv_obj_t *content;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *status;
} desktop_fullscreen_layout_t;

/**
 * @brief Create the shared fullscreen header and content layout directly in parent.
 * @param parent Parent object for the layout.
 * @param colors Colors used for the layout.
 * @param title Page title.
 * @param subtitle Page subtitle.
 * @param status Status text.
 * @param [out] layout Receives the created widget references.
 * @return true if the layout was created.
 */
bool desktop_fullscreen_layout_create(lv_obj_t *parent,
                                    const desktop_fullscreen_layout_colors_t *colors,
                                    const char *title,
                                    const char *subtitle,
                                    const char *status,
                                    desktop_fullscreen_layout_t *layout);

/**
 * @brief Hide the page title and subtitle, leaving the status row centred.
 * @param layout Layout whose title and subtitle are hidden.
 */
void desktop_fullscreen_layout_remove_page_title(desktop_fullscreen_layout_t *layout);

/**
 * @brief Hide the header and expand content to the full page height.
 * @param layout Layout whose header is hidden.
 */
void desktop_fullscreen_layout_remove_page_header(desktop_fullscreen_layout_t *layout);

/**
 * @brief Create a borderless flex panel with percentage width.
 * @param parent Parent object.
 * @param background Panel background color.
 * @param width_percent Panel width as a percentage of its parent.
 * @param radius Corner radius in pixels.
 * @param flow Flex flow for panel children.
 * @return Created panel object.
 */
lv_obj_t *desktop_fullscreen_layout_create_panel(lv_obj_t *parent,
                                               lv_color_t background,
                                               int32_t width_percent,
                                               int32_t radius,
                                               lv_flex_flow_t flow);

/**
 * @brief Apply shared panel colours and radius without changing geometry.
 * @param object Panel to style.
 * @param background Panel background color.
 * @param radius Corner radius in pixels.
 */
void desktop_fullscreen_layout_style_panel(lv_obj_t *object,
                                         lv_color_t background,
                                         int32_t radius);

#ifdef __cplusplus
}
#endif
