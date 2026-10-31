/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "desktop_seven_segment.h"

#include <stddef.h>

#define DIGIT_HEIGHT_PERCENT 50
#define DIGIT_WIDTH_PERCENT_OF_HEIGHT 58
#define THICKNESS_PERCENT_OF_HEIGHT 11
#define DIGIT_GAP_PERCENT_OF_HEIGHT 12
#define COLON_WIDTH_PERCENT_OF_DIGIT 50
#define COLON_GAP_PERCENT_OF_HEIGHT 8

#define COLON_DOT_TOP_PERCENT 30
#define COLON_DOT_BOTTOM_PERCENT 70

#define JOINT_GAP_PERCENT_OF_THICKNESS 25
#define JOINT_GAP_MIN 2

static const uint8_t s_digit_masks[10] = {
    0x3FU,
    0x06U,
    0x5BU,
    0x4FU,
    0x66U,
    0x6DU,
    0x7DU,
    0x07U,
    0x7FU,
    0x6FU,
};

/**
 * @brief 将十进制数字转换为 a 至 g 的位掩码。
 * @param digit 十进制数字。
 * @return 对应的段位掩码；超出范围时为零。
 */
uint8_t desktop_seven_segment_mask(uint8_t digit)
{
    return digit < (uint8_t)(sizeof(s_digit_masks) / sizeof(s_digit_masks[0]))
               ? s_digit_masks[digit]
               : 0U;
}

int32_t desktop_seven_segment_joint_gap(int32_t thickness)
{
    const int32_t gap = thickness * JOINT_GAP_PERCENT_OF_THICKNESS / 100;
    return gap < JOINT_GAP_MIN ? JOINT_GAP_MIN : gap;
}

static void polygon_point(desktop_segment_polygon_t *polygon, uint8_t index, int32_t x, int32_t y)
{
    polygon->point[index].x = x;
    polygon->point[index].y = y;
}

/**
 * @brief 将无可用空间的数码段折叠为单点。
 * @param [out] polygon 接收折叠后的多边形。
 * @param x 折叠点的横坐标。
 * @param y 折叠点的纵坐标。
 */
static void polygon_collapse(desktop_segment_polygon_t *polygon, int32_t x, int32_t y)
{
    for (uint8_t index = 0; index < (uint8_t)DESKTOP_SEGMENT_POINT_COUNT; ++index) {
        polygon_point(polygon, index, x, y);
    }
}

/**
 * @brief 生成两端尖角的横向数码段。
 * @param [out] polygon 接收横段顶点。
 * @param left 左边界坐标。
 * @param right 右边界坐标。
 * @param top 上边界坐标。
 * @param thickness 段厚度。
 */
static void polygon_horizontal(desktop_segment_polygon_t *polygon, int32_t left, int32_t right,
                               int32_t top, int32_t thickness)
{
    const int32_t half = thickness / 2;
    const int32_t middle = top + half;
    const int32_t bottom = top + thickness;

    polygon_point(polygon, 0, left, middle);
    polygon_point(polygon, 1, left + half, top);
    polygon_point(polygon, 2, right - half, top);
    polygon_point(polygon, 3, right, middle);
    polygon_point(polygon, 4, right - half, bottom);
    polygon_point(polygon, 5, left + half, bottom);
}

/**
 * @brief 生成两端尖角的纵向数码段。
 * @param [out] polygon 接收纵段顶点。
 * @param left 左边界坐标。
 * @param right 右边界坐标。
 * @param top 上边界坐标。
 * @param bottom 下边界坐标。
 * @param thickness 段厚度。
 */
static void polygon_vertical(desktop_segment_polygon_t *polygon, int32_t left, int32_t right,
                             int32_t top, int32_t bottom, int32_t thickness)
{
    const int32_t half = thickness / 2;
    const int32_t center = (left + right) / 2;

    polygon_point(polygon, 0, center, top);
    polygon_point(polygon, 1, right, top + half);
    polygon_point(polygon, 2, right, bottom - half);
    polygon_point(polygon, 3, center, bottom);
    polygon_point(polygon, 4, left, bottom - half);
    polygon_point(polygon, 5, left, top + half);
}

bool desktop_seven_segment_digit_polygons(int32_t x, int32_t y, int32_t width, int32_t height,
                                        int32_t thickness,
                                        desktop_segment_polygon_t polygons[DESKTOP_SEGMENT_COUNT])
{
    if (polygons == NULL || thickness <= 0 || width <= 0 || height <= 0) {
        return false;
    }

    const int32_t middle_bar_top = y + height / 2 - thickness / 2;
    const int32_t bottom_bar_top = y + height - thickness;
    const int32_t left_edge = x;
    const int32_t right_edge = x + width;
    const int32_t half = thickness / 2;
    const int32_t gap = desktop_seven_segment_joint_gap(thickness);

    const int32_t left_column_top = y + thickness + gap;
    const int32_t left_column_bottom = middle_bar_top - gap;
    const int32_t right_column_top = middle_bar_top + thickness + gap;
    const int32_t right_column_bottom = bottom_bar_top - gap;
    if (left_column_bottom <= left_column_top || right_column_bottom <= right_column_top) {
        return false;
    }

    const int32_t apex_left = left_edge + half;
    const int32_t apex_right = right_edge - thickness + half;

    const bool has_horizontal_room = (right_edge - left_edge) >= 2 * thickness;
    if (has_horizontal_room) {
        polygon_horizontal(&polygons[DESKTOP_SEGMENT_A], apex_left, apex_right, y, thickness);
        polygon_horizontal(&polygons[DESKTOP_SEGMENT_D], apex_left, apex_right, bottom_bar_top,
                           thickness);
        polygon_horizontal(&polygons[DESKTOP_SEGMENT_G], apex_left, apex_right, middle_bar_top,
                           thickness);
    } else {
        polygon_collapse(&polygons[DESKTOP_SEGMENT_A], apex_left, y + half);
        polygon_collapse(&polygons[DESKTOP_SEGMENT_D], apex_left, bottom_bar_top + half);
        polygon_collapse(&polygons[DESKTOP_SEGMENT_G], apex_left, middle_bar_top + half);
    }

    polygon_vertical(&polygons[DESKTOP_SEGMENT_F], left_edge, left_edge + thickness,
                     left_column_top, left_column_bottom, thickness);
    polygon_vertical(&polygons[DESKTOP_SEGMENT_B], right_edge - thickness, right_edge,
                     left_column_top, left_column_bottom, thickness);
    polygon_vertical(&polygons[DESKTOP_SEGMENT_E], left_edge, left_edge + thickness,
                     right_column_top, right_column_bottom, thickness);
    polygon_vertical(&polygons[DESKTOP_SEGMENT_C], right_edge - thickness, right_edge,
                     right_column_top, right_column_bottom, thickness);
    return true;
}

void desktop_seven_segment_polygon_bounds(const desktop_segment_polygon_t *polygon,
                                        desktop_segment_rect_t *bounds)
{
    if (polygon == NULL || bounds == NULL) {
        return;
    }
    int32_t min_x = polygon->point[0].x;
    int32_t max_x = min_x;
    int32_t min_y = polygon->point[0].y;
    int32_t max_y = min_y;
    for (uint8_t index = 1; index < (uint8_t)DESKTOP_SEGMENT_POINT_COUNT; ++index) {
        const int32_t x = polygon->point[index].x;
        const int32_t y = polygon->point[index].y;
        if (x < min_x) {
            min_x = x;
        }
        if (x > max_x) {
            max_x = x;
        }
        if (y < min_y) {
            min_y = y;
        }
        if (y > max_y) {
            max_y = y;
        }
    }
    bounds->x = min_x;
    bounds->y = min_y;
    bounds->width = max_x - min_x;
    bounds->height = max_y - min_y;
}

bool app_clock_layout(uint16_t screen_width, uint16_t screen_height,
                        app_clock_layout_t *layout)
{
    if (layout == NULL || screen_width == 0U || screen_height == 0U) {
        return false;
    }

    const int32_t digit_height = (int32_t)screen_height * DIGIT_HEIGHT_PERCENT / 100;
    const int32_t digit_width = digit_height * DIGIT_WIDTH_PERCENT_OF_HEIGHT / 100;
    const int32_t thickness = digit_height * THICKNESS_PERCENT_OF_HEIGHT / 100;
    const int32_t digit_gap = digit_height * DIGIT_GAP_PERCENT_OF_HEIGHT / 100;
    const int32_t colon_width = digit_width * COLON_WIDTH_PERCENT_OF_DIGIT / 100;
    const int32_t colon_gap = digit_height * COLON_GAP_PERCENT_OF_HEIGHT / 100;

    const int32_t total_width = (int32_t)APP_CLOCK_DIGIT_COUNT * digit_width +
                                2 * digit_gap + colon_width + 2 * colon_gap;
    if (total_width <= 0 || total_width > (int32_t)screen_width) {
        return false;
    }

    layout->digit_height = digit_height;
    layout->digit_width = digit_width;
    layout->thickness = thickness;
    layout->digit_y = ((int32_t)screen_height - digit_height) / 2;

    const int32_t start_x = ((int32_t)screen_width - total_width) / 2;
    layout->digit_x[APP_CLOCK_DIGIT_HOUR_TENS] = start_x;
    layout->digit_x[APP_CLOCK_DIGIT_HOUR_UNITS] = start_x + digit_width + digit_gap;
    layout->colon_x = layout->digit_x[APP_CLOCK_DIGIT_HOUR_UNITS] + digit_width + colon_gap;
    layout->colon_width = colon_width;
    layout->digit_x[APP_CLOCK_DIGIT_MINUTE_TENS] = layout->colon_x + colon_width + colon_gap;
    layout->digit_x[APP_CLOCK_DIGIT_MINUTE_UNITS] =
        layout->digit_x[APP_CLOCK_DIGIT_MINUTE_TENS] + digit_width + digit_gap;

    layout->colon_dot_x = layout->colon_x + (colon_width - thickness) / 2;
    layout->colon_dot_y[0] = layout->digit_y + digit_height * COLON_DOT_TOP_PERCENT / 100;
    layout->colon_dot_y[1] = layout->digit_y + digit_height * COLON_DOT_BOTTOM_PERCENT / 100 -
                             thickness;
    return true;
}
