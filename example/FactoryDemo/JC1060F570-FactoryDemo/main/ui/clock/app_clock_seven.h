/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Segment masks map bit zero through bit six to a through g. */
#define APP_CLOCK_SEVEN_SEGMENTS 7U

/** @brief Six digits display HH:MM:SS with leading zeros. */
#define APP_CLOCK_SEVEN_DIGITS 6U

#define APP_CLOCK_SEVEN_COLONS 2U

/** @brief Each bar is a six-point polygon with triangular ends. */
#define APP_CLOCK_SEVEN_POINTS 6U

typedef enum {
    APP_CLOCK_SEGMENT_A = 0,
    APP_CLOCK_SEGMENT_B,
    APP_CLOCK_SEGMENT_C,
    APP_CLOCK_SEGMENT_D,
    APP_CLOCK_SEGMENT_E,
    APP_CLOCK_SEGMENT_F,
    APP_CLOCK_SEGMENT_G,
    APP_CLOCK_SEGMENT_COUNT,
} app_clock_segment_id_t;

typedef struct {
    int32_t x;
    int32_t y;
} app_clock_seven_point_t;

typedef struct {
    app_clock_seven_point_t point[APP_CLOCK_SEVEN_POINTS];
} app_clock_seven_polygon_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} app_clock_seven_rect_t;

typedef struct {
    int32_t digit_x[APP_CLOCK_SEVEN_DIGITS];
    int32_t digit_y;
    int32_t digit_width;
    int32_t digit_height;
    int32_t thickness;
    int32_t gap;
    int32_t colon_x[APP_CLOCK_SEVEN_COLONS];
    int32_t colon_width;
    int32_t colon_dot_x[APP_CLOCK_SEVEN_COLONS];
    int32_t colon_dot_y[2];
} app_clock_seven_layout_t;

/**
 * @brief Return the decimal digit mask.
 * @param digit Decimal digit.
 * @return Segment mask for 0..9, or zero for other values.
 */
uint8_t app_clock_seven_mask(uint8_t digit);

/**
 * @brief Locate the shared tip axis for the selected left or right bar column.
 * @param cell_x Left edge of the digit cell in pixels.
 * @param cell_width Width of the digit cell in pixels.
 * @param thickness Segment thickness in pixels.
 * @param right_column true selects the right column; false selects the left.
 * @return Horizontal axis coordinate in pixels.
 */
int32_t app_clock_seven_axis_x(int32_t cell_x, int32_t cell_width, int32_t thickness,
                                 bool right_column);

/**
 * @brief Build seven bars with pixel dimensions and gaps.
 * @param x Left edge of the digit cell in pixels.
 * @param y Top edge of the digit cell in pixels.
 * @param width Cell width in pixels.
 * @param height Cell height in pixels.
 * @param thickness Segment thickness in pixels.
 * @param gap Gap between segments in pixels.
 * @param[out] polygons Array receiving seven segment polygons.
 * @return true on success; false when the geometry or output is invalid.
 */
bool app_clock_seven_polygons(int32_t x, int32_t y, int32_t width, int32_t height,
                                int32_t thickness, int32_t gap,
                                app_clock_seven_polygon_t polygons[APP_CLOCK_SEVEN_SEGMENTS]);

/**
 * @brief Calculate axis-aligned bounds; collapsed bars have zero extent.
 * @param polygon Segment polygon.
 * @param[out] bounds Receives its bounding rectangle.
 */
void app_clock_seven_bounds(const app_clock_seven_polygon_t *polygon,
                              app_clock_seven_rect_t *bounds);

/**
 * @brief Center HH:MM:SS on the panel.
 * @param panel_width Panel width in pixels.
 * @param panel_height Panel height in pixels.
 * @param[out] layout Receives digit and colon geometry.
 * @return true on success; false when the display cannot fit or layout is NULL.
 */
bool app_clock_seven_layout(uint16_t panel_width, uint16_t panel_height,
                              app_clock_seven_layout_t *layout);

/**
 * @brief Move brightness toward the target in 0..255; negative steps become zero.
 * @param current Current intensity.
 * @param target Target intensity.
 * @param step Maximum change per call.
 * @return Updated intensity.
 */
int32_t app_clock_seven_intensity_step(int32_t current, int32_t target, int32_t step);

/**
 * @brief Blend RGB colors with intensity 0 for off and 255 for on.
 * @param off_rgb RGB color at zero intensity.
 * @param on_rgb RGB color at full intensity.
 * @param intensity Blend intensity, clamped to 0..255.
 * @return Blended RGB color.
 */
uint32_t app_clock_seven_blend(uint32_t off_rgb, uint32_t on_rgb, int32_t intensity);

#ifdef __cplusplus
}
#endif
