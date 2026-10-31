/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DESKTOP_SEGMENT_A = 0, /**< 段编号依次对应 a（顶部）至 g（中部），同时作为位掩码索引。 */
    DESKTOP_SEGMENT_B,
    DESKTOP_SEGMENT_C,
    DESKTOP_SEGMENT_D,
    DESKTOP_SEGMENT_E,
    DESKTOP_SEGMENT_F,
    DESKTOP_SEGMENT_G,
    DESKTOP_SEGMENT_COUNT,
} desktop_segment_id_t;

typedef struct {
    int32_t x;
    int32_t y;
} desktop_point_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} desktop_segment_rect_t;

/** @brief 每个数码段由六个顶点构成。 */
#define DESKTOP_SEGMENT_POINT_COUNT 6U

typedef struct {
    desktop_point_t point[DESKTOP_SEGMENT_POINT_COUNT];
} desktop_segment_polygon_t;

typedef enum {
    APP_CLOCK_DIGIT_HOUR_TENS = 0, /**< HH:MM 的四个数字位置。 */
    APP_CLOCK_DIGIT_HOUR_UNITS,
    APP_CLOCK_DIGIT_MINUTE_TENS,
    APP_CLOCK_DIGIT_MINUTE_UNITS,
    APP_CLOCK_DIGIT_COUNT,
} app_clock_digit_t;

typedef struct {
    int32_t digit_x[APP_CLOCK_DIGIT_COUNT];
    int32_t digit_y;
    int32_t digit_width;
    int32_t digit_height;

    int32_t thickness; /**< 数码段厚度与冒号点尺寸，单位为像素。 */
    int32_t colon_width;
    int32_t colon_x;
    int32_t colon_dot_x;
    int32_t colon_dot_y[2];
} app_clock_layout_t;

/**
 * @brief 返回十进制数字的段掩码；超出 0 至 9 时返回全灭掩码。
 * @param digit 十进制数字。
 * @return 对应的段位掩码；超出范围时为零。
 */
uint8_t desktop_seven_segment_mask(uint8_t digit);

/**
 * @brief 根据段厚度计算连接间隙，单位为像素，最小为 1。
 * @param thickness 数码段厚度，单位为像素。
 * @return 连接间隙，单位为像素，最小为 1。
 */
int32_t desktop_seven_segment_joint_gap(int32_t thickness);

/**
 * @brief 在数字单元内生成七个六边形数码段。 单元宽度等于段厚度时横段折叠；参数或高度无效时返回 false。
 * @param x 数字单元左上角横坐标。
 * @param y 数字单元左上角纵坐标。
 * @param width 数字单元宽度。
 * @param height 数字单元高度。
 * @param thickness 段厚度。
 * @param [out] polygons 接收七个数码段。
 * @return 生成成功返回 true，参数或尺寸无效返回 false。
 */
bool desktop_seven_segment_digit_polygons(int32_t x, int32_t y, int32_t width, int32_t height,
                                        int32_t thickness,
                                        desktop_segment_polygon_t polygons[DESKTOP_SEGMENT_COUNT]);

/**
 * @brief 计算多边形边界；折叠段的宽或高为 0。
 * @param polygon 待计算的多边形。
 * @param [out] bounds 接收计算出的边界。
 */
void desktop_seven_segment_polygon_bounds(const desktop_segment_polygon_t *polygon,
                                        desktop_segment_rect_t *bounds);

/**
 * @brief 按屏幕尺寸生成居中的 HH:MM 布局，四个数字槽位等宽。 屏幕无法容纳布局时返回 false。
 * @param screen_width 屏幕宽度，单位为像素。
 * @param screen_height 屏幕高度，单位为像素。
 * @param [out] layout 接收布局结果。
 * @return 布局有效时返回 true，否则返回 false。
 */
bool app_clock_layout(uint16_t screen_width, uint16_t screen_height,
                        app_clock_layout_t *layout);

#ifdef __cplusplus
}
#endif
