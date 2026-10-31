/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "app_clock_seven.h"

#include <stddef.h>

#define DIGIT_HEIGHT_PERCENT 46
#define DIGIT_WIDTH_PERCENT_OF_HEIGHT 56
#define THICKNESS_PERCENT_OF_HEIGHT 11

#define JOINT_GAP_PERCENT_OF_THICKNESS 25

#define JOINT_GAP_MIN 2
#define DIGIT_GAP_PERCENT_OF_HEIGHT 9
#define COLON_WIDTH_PERCENT_OF_DIGIT 30

#define ROW_WIDTH_LIMIT_PERCENT 92

#define COLON_DOT_TOP_PERCENT 30
#define COLON_DOT_BOTTOM_PERCENT 70

/** @brief Decimal digit masks, with a through g mapped to bits zero through six. */
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

uint8_t app_clock_seven_mask(uint8_t digit)
{
    return digit < (uint8_t)(sizeof(s_digit_masks) / sizeof(s_digit_masks[0]))
               ? s_digit_masks[digit]
               : 0U;
}

int32_t app_clock_seven_axis_x(int32_t cell_x, int32_t cell_width, int32_t thickness,
                                 bool right_column)
{
    const int32_t half = thickness / 2;

    return right_column ? cell_x + cell_width - thickness + half : cell_x + half;
}

static void polygon_point(app_clock_seven_polygon_t *polygon, uint8_t index, int32_t x,
                          int32_t y)
{
    polygon->point[index].x = x;
    polygon->point[index].y = y;
}

/**
 * @brief Represent an unusable bar by coincident vertices.
 * @param[out] polygon Polygon to collapse.
 * @param x Horizontal coordinate of the collapsed point in pixels.
 * @param y Vertical coordinate of the collapsed point in pixels.
 */
static void polygon_collapse(app_clock_seven_polygon_t *polygon, int32_t x, int32_t y)
{
    for (uint8_t index = 0; index < (uint8_t)APP_CLOCK_SEVEN_POINTS; ++index) {
        polygon_point(polygon, index, x, y);
    }
}

/**
 * @brief Build a horizontal bar between its left and right tip axes.
 * @param[out] polygon Receives the bar vertices.
 * @param apex_left Left tip axis in pixels.
 * @param apex_right Right tip axis in pixels.
 * @param band_top Top of the bar in pixels.
 * @param thickness Bar thickness in pixels.
 */
static void polygon_horizontal(app_clock_seven_polygon_t *polygon, int32_t apex_left,
                               int32_t apex_right, int32_t band_top, int32_t thickness)
{
    const int32_t half = thickness / 2;
    const int32_t middle = band_top + half;
    const int32_t bottom = band_top + thickness;

    polygon_point(polygon, 0, apex_left, middle);
    polygon_point(polygon, 1, apex_left + half, band_top);
    polygon_point(polygon, 2, apex_right - half, band_top);
    polygon_point(polygon, 3, apex_right, middle);
    polygon_point(polygon, 4, apex_right - half, bottom);
    polygon_point(polygon, 5, apex_left + half, bottom);
}

/**
 * @brief Build a vertical bar between its upper and lower tips.
 * @param[out] polygon Receives the bar vertices.
 * @param band_left Left edge of the bar in pixels.
 * @param band_right Right edge of the bar in pixels.
 * @param apex_top Upper tip axis in pixels.
 * @param apex_bottom Lower tip axis in pixels.
 * @param thickness Bar thickness in pixels.
 */
static void polygon_vertical(app_clock_seven_polygon_t *polygon, int32_t band_left,
                             int32_t band_right, int32_t apex_top, int32_t apex_bottom,
                             int32_t thickness)
{
    const int32_t half = thickness / 2;
    const int32_t center = (band_left + band_right) / 2;

    polygon_point(polygon, 0, center, apex_top);
    polygon_point(polygon, 1, band_right, apex_top + half);
    polygon_point(polygon, 2, band_right, apex_bottom - half);
    polygon_point(polygon, 3, center, apex_bottom);
    polygon_point(polygon, 4, band_left, apex_bottom - half);
    polygon_point(polygon, 5, band_left, apex_top + half);
}

bool app_clock_seven_polygons(int32_t x, int32_t y, int32_t width, int32_t height,
                                int32_t thickness, int32_t gap,
                                app_clock_seven_polygon_t polygons[APP_CLOCK_SEVEN_SEGMENTS])
{
    if (polygons == NULL || thickness <= 0 || width <= 0 || height <= 0 || gap < 0) {
        return false;
    }

    const int32_t half = thickness / 2;
    const int32_t middle_band_top = y + height / 2 - thickness / 2;
    const int32_t bottom_band_top = y + height - thickness;
    const int32_t left_band_left = x;
    const int32_t right_band_right = x + width;
    const int32_t right_band_left = right_band_right - thickness;

    const int32_t upper_apex_top = y + thickness + gap;
    const int32_t upper_apex_bottom = middle_band_top - gap;
    const int32_t lower_apex_top = middle_band_top + thickness + gap;
    const int32_t lower_apex_bottom = bottom_band_top - gap;
    if (upper_apex_bottom <= upper_apex_top || lower_apex_bottom <= lower_apex_top) {
        return false;
    }

    const bool has_horizontal_room = width >= 2 * thickness;
    if (has_horizontal_room) {
        const int32_t apex_left = app_clock_seven_axis_x(x, width, thickness, false);
        const int32_t apex_right = app_clock_seven_axis_x(x, width, thickness, true);
        polygon_horizontal(&polygons[APP_CLOCK_SEGMENT_A], apex_left, apex_right, y, thickness);
        polygon_horizontal(&polygons[APP_CLOCK_SEGMENT_D], apex_left, apex_right,
                           bottom_band_top, thickness);
        polygon_horizontal(&polygons[APP_CLOCK_SEGMENT_G], apex_left, apex_right,
                           middle_band_top, thickness);
    } else {
        polygon_collapse(&polygons[APP_CLOCK_SEGMENT_A], x + half, y + half);
        polygon_collapse(&polygons[APP_CLOCK_SEGMENT_D], x + half, bottom_band_top + half);
        polygon_collapse(&polygons[APP_CLOCK_SEGMENT_G], x + half, middle_band_top + half);
    }

    polygon_vertical(&polygons[APP_CLOCK_SEGMENT_F], left_band_left,
                     left_band_left + thickness, upper_apex_top, upper_apex_bottom, thickness);
    polygon_vertical(&polygons[APP_CLOCK_SEGMENT_B], right_band_left, right_band_right,
                     upper_apex_top, upper_apex_bottom, thickness);
    polygon_vertical(&polygons[APP_CLOCK_SEGMENT_E], left_band_left,
                     left_band_left + thickness, lower_apex_top, lower_apex_bottom, thickness);
    polygon_vertical(&polygons[APP_CLOCK_SEGMENT_C], right_band_left, right_band_right,
                     lower_apex_top, lower_apex_bottom, thickness);
    return true;
}

void app_clock_seven_bounds(const app_clock_seven_polygon_t *polygon,
                              app_clock_seven_rect_t *bounds)
{
    if (polygon == NULL || bounds == NULL) {
        return;
    }
    int32_t min_x = polygon->point[0].x;
    int32_t max_x = min_x;
    int32_t min_y = polygon->point[0].y;
    int32_t max_y = min_y;
    for (uint8_t index = 1; index < (uint8_t)APP_CLOCK_SEVEN_POINTS; ++index) {
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

/**
 * @brief Fit six digits, two colon columns and seven gaps inside the panel.
 * @param panel_width Panel width in pixels.
 * @param panel_height Panel height in pixels.
 * @param[out] layout Receives digit and colon geometry.
 * @return true if the layout fits; false otherwise.
 */
bool app_clock_seven_layout(uint16_t panel_width, uint16_t panel_height,
                              app_clock_seven_layout_t *layout)
{
    if (layout == NULL || panel_width == 0U || panel_height == 0U) {
        return false;
    }

    int32_t digit_height = (int32_t)panel_height * DIGIT_HEIGHT_PERCENT / 100;
    int32_t digit_width = 0;
    int32_t thickness = 0;
    int32_t gap = 0;
    int32_t digit_gap = 0;
    int32_t colon_width = 0;
    int32_t total_width = 0;
    const int32_t width_limit = (int32_t)panel_width * ROW_WIDTH_LIMIT_PERCENT / 100;
    if (width_limit <= 0) {
        return false;
    }

    for (uint8_t attempt = 0; attempt < 8U; ++attempt) {
        digit_width = digit_height * DIGIT_WIDTH_PERCENT_OF_HEIGHT / 100;
        thickness = digit_height * THICKNESS_PERCENT_OF_HEIGHT / 100;
        digit_gap = digit_height * DIGIT_GAP_PERCENT_OF_HEIGHT / 100;
        gap = thickness * JOINT_GAP_PERCENT_OF_THICKNESS / 100;
        if (gap < JOINT_GAP_MIN) {
            gap = JOINT_GAP_MIN;
        }
        colon_width = digit_width * COLON_WIDTH_PERCENT_OF_DIGIT / 100;
        if (digit_width <= 0 || thickness <= 0 || colon_width <= 0) {
            return false;
        }
        total_width = (int32_t)APP_CLOCK_SEVEN_DIGITS * digit_width +
                      2 * colon_width + 7 * digit_gap;
        if (total_width <= 0) {
            return false;
        }
        if (total_width <= width_limit) {
            break;
        }
        const int32_t scaled = digit_height * width_limit / total_width;
        if (scaled >= digit_height || scaled <= 0) {
            digit_height = scaled > 0 ? scaled : digit_height;
            break;
        }
        digit_height = scaled;
    }

    if (total_width > (int32_t)panel_width || digit_width <= 0 || thickness <= 0) {
        return false;
    }

    layout->digit_y = ((int32_t)panel_height - digit_height) / 2;
    layout->digit_width = digit_width;
    layout->digit_height = digit_height;
    layout->thickness = thickness;
    layout->gap = gap;
    layout->colon_width = colon_width;

    int32_t cursor = ((int32_t)panel_width - total_width) / 2;
    uint8_t digit = 0U;
    uint8_t colon = 0U;
    for (uint8_t slot = 0; slot < (uint8_t)(APP_CLOCK_SEVEN_DIGITS +
                                            APP_CLOCK_SEVEN_COLONS); ++slot) {
        const bool is_colon = (slot == 2U) || (slot == 5U);
        if (is_colon) {
            layout->colon_x[colon] = cursor;
            ++colon;
            cursor += colon_width;
        } else {
            layout->digit_x[digit] = cursor;
            ++digit;
            cursor += digit_width;
        }
        cursor += digit_gap;
    }

    for (uint8_t index = 0U; index < (uint8_t)APP_CLOCK_SEVEN_COLONS; ++index) {
        layout->colon_dot_x[index] = layout->colon_x[index] + (colon_width - thickness) / 2;
    }
    layout->colon_dot_y[0] = layout->digit_y + digit_height * COLON_DOT_TOP_PERCENT / 100;
    layout->colon_dot_y[1] = layout->digit_y + digit_height * COLON_DOT_BOTTOM_PERCENT / 100 -
                             thickness;
    return true;
}

int32_t app_clock_seven_intensity_step(int32_t current, int32_t target, int32_t step)
{
    if (current < 0) {
        current = 0;
    } else if (current > 255) {
        current = 255;
    }
    if (target < 0) {
        target = 0;
    } else if (target > 255) {
        target = 255;
    }
    if (step <= 0 || current == target) {
        return current;
    }
    const int32_t distance = target - current;
    if (distance > 0) {
        return distance <= step ? target : current + step;
    }
    return -distance <= step ? target : current - step;
}

uint32_t app_clock_seven_blend(uint32_t off_rgb, uint32_t on_rgb, int32_t intensity)
{
    if (intensity <= 0) {
        return off_rgb & 0xFFFFFFU;
    }
    if (intensity >= 255) {
        return on_rgb & 0xFFFFFFU;
    }
    uint32_t result = 0U;
    for (uint8_t shift = 0U; shift < 24U; shift += 8U) {
        const int32_t off = (int32_t)((off_rgb >> shift) & 0xFFU);
        const int32_t on = (int32_t)((on_rgb >> shift) & 0xFFU);
        const int32_t mixed = off + (on - off) * intensity / 255;
        result |= (uint32_t)(mixed & 0xFF) << shift;
    }
    return result;
}
