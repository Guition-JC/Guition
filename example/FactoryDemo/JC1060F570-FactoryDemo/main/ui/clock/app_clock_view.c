/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "app_clock_view.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "image/lv_image_decoder_private.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "app_clock_analog.h"
#include "app_clock_digits.h"
#include "app_clock_face.h"
#include "app_clock_flip.h"
#include "app_clock_prefs.h"
#include "app_clock_seven.h"
#include "desktop_font.h"
#include "desktop_fullscreen_layout.h"
#include "desktop_language.h"
#include "time_policy.h"
#include "time_service.h"
#include "time_settings.h"

static const char *TAG = "ClockView";

/** @brief Shared fullscreen clock and settings palette. */
#define C_CANVAS lv_color_hex(0x0E1420)

#define C_PANEL lv_color_hex(0x151C29)
#define C_PANEL_EDGE lv_color_hex(0x2A3547)
#define C_CARD lv_color_hex(0x1B2436)
#define C_TEXT lv_color_hex(0xE6EEFA)
#define C_TEXT_MUTED lv_color_hex(0x8FA3C4)
#define C_ACCENT lv_color_hex(0x147EFB)
#define C_ACCENT_SOFT lv_color_hex(0x12305A)
#define C_ACCENT_STRONG lv_color_hex(0x0B5CAD)
#define C_ACCENT_PRESSED lv_color_hex(0x084A8F)

#define C_FACE_TEXT lv_color_hex(0x9FB2D0)
#define C_DIAL_FACE lv_color_hex(0x151C29)
#define C_DIAL_RING lv_color_hex(0x3A4761)
#define C_TICK_MAJOR lv_color_hex(0x9FB2D0)
#define C_TICK_MINOR lv_color_hex(0x46536B)
#define C_HOUR_HAND lv_color_hex(0xF2F6FF)
#define C_MINUTE_HAND lv_color_hex(0x6FB6FF)
#define C_SECOND_HAND lv_color_hex(0xFF6B6B)
#define C_FLIP_CARD lv_color_hex(0x1B2436)
#define C_FLIP_SPLIT lv_color_hex(0x0B0F16)
#define C_FLIP_EDGE lv_color_hex(0x3A4761)
#define C_FLIP_DIGIT lv_color_hex(0xF2F6FF)

#define C_TUBE_ON lv_color_hex(0xFF9F0A)
#define C_TUBE_OFF lv_color_hex(0x30291C)

/** @brief Animate at fixed brightness steps with a maximum fade duration in milliseconds. */
#define CLOCK_ANIM_PERIOD_MS 50U
#define CLOCK_SEGMENT_FADE_STEPS 8U
#define CLOCK_SEGMENT_FADE_MAX_MS 700U

#define CLOCK_HINT_MS 4000U

/** @brief Flip-card dimensions as percentages of card height. */
#define FLIP_CARD_WIDTH_PERCENT 70
#define FLIP_GAP_PERCENT 10
#define FLIP_COLON_WIDTH_PERCENT 30
#define FLIP_CARD_ROW_PERCENT 86
#define FLIP_CARD_FONT_PERCENT 132

/** @brief Hand lengths as percentages of dial radius. */
#define HAND_HOUR_PERCENT 52
#define HAND_MINUTE_PERCENT 78
#define HAND_SECOND_PERCENT 88

#define DIAL_RADIUS_PERCENT 84

#define CLOCK_DIGITS 6U

/** @brief LVGL asset paths and matching POSIX paths used for presence checks. */
#define CLOCK_DIGITS_BACKGROUND_LV "A:/assets/clock/clock_background.jpg"
#define CLOCK_DIGITS_BACKGROUND_PROBE "/assets/clock/clock_background.jpg"
#define CLOCK_DIGITS_TUBE_LV "A:/assets/clock/%u.png"
#define CLOCK_DIGITS_TUBE_PROBE "/assets/clock/%u.png"
#define CLOCK_DIGITS_TUBES 10U
#define CLOCK_TUBE_PATH_MAX 48U

typedef struct {
    lv_obj_t *card;
    lv_obj_t *top_clip;
    lv_obj_t *top_label;
    lv_obj_t *bottom_clip;
    lv_obj_t *bottom_label;
    lv_obj_t *flap;
    lv_obj_t *flap_label;
} clock_flip_card_t;

typedef struct {
    lv_obj_t *tube;
} clock_digit_slot_t;

/** @brief Singleton view state shared by the clock app and the sleep face. */
static bool s_sleep_mode;
static app_clock_time_cb_t s_sleep_time_cb;
static void *s_sleep_time_context;
static lv_obj_t *s_view;
static lv_obj_t *s_face;
static lv_obj_t *s_date;
static lv_obj_t *s_hint;
static lv_obj_t *s_settings;
static lv_timer_t *s_tick;
static lv_timer_t *s_anim;
static lv_timer_t *s_hint_timer;
static app_clock_toast_cb_t s_toast;
static app_clock_style_t s_style;
static int32_t s_active_offset;
static int32_t s_pending_offset;

static bool s_time_valid;
/** @brief Current HHMMSS digits and the digits shown by resting cards. */
static uint8_t s_digit[CLOCK_DIGITS];
static uint8_t s_shown_digit[CLOCK_DIGITS];
static uint8_t s_flip_old_digit[CLOCK_DIGITS];
static char s_date_text[96];

static clock_flip_card_t s_flip_card[CLOCK_DIGITS];
static bool s_flip_digit_active[CLOCK_DIGITS];
static bool s_flip_active;
/** @brief Accumulated flip progress in sixteenths. */
static uint32_t s_flip_accum16;
static app_clock_flip_frame_t s_flip_frame;
static int32_t s_flip_card_width;
static int32_t s_flip_card_height;
static int32_t s_flip_card_x[CLOCK_DIGITS];
static int32_t s_flip_card_y;

typedef struct {
    app_clock_seven_layout_t layout; /**< Cached geometry and repaint regions for one seven-segment display. */
    app_clock_seven_polygon_t bar[CLOCK_DIGITS][APP_CLOCK_SEVEN_SEGMENTS];
    lv_area_t cell[CLOCK_DIGITS];
    lv_area_t colon[APP_CLOCK_SEVEN_COLONS];
} clock_tubes_t;

static int32_t s_seg_intensity[CLOCK_DIGITS][APP_CLOCK_SEVEN_SEGMENTS];
static uint8_t s_seg_target[CLOCK_DIGITS][APP_CLOCK_SEVEN_SEGMENTS];
static int32_t s_seven_colon_value[APP_CLOCK_SEVEN_COLONS];
static bool s_seven_active;
static bool s_seven_ready;
static clock_tubes_t s_tubes;

static clock_tubes_t s_preview_tubes;
static bool s_preview_ready;
static int32_t s_preview_box[4];

/** @brief LVGL tick at fade start, in milliseconds. */
static uint32_t s_fade_tick;

static lv_obj_t *s_digits_background;
static clock_digit_slot_t s_digit_slot[CLOCK_DIGITS];
static bool s_digits_built;

/** @brief Render the first frame immediately without an entrance animation. */
static bool s_first_frame = true;

/** @brief True after the active style has usable geometry and widgets. */
static bool s_face_built;

static int32_t s_flip_colon_x[2];
static int32_t s_flip_colon_width;

static lv_obj_t *s_style_card[APP_CLOCK_STYLE_COUNT];
static lv_obj_t *s_pending_label;
static lv_obj_t *s_active_label;
static lv_obj_t *s_sync_label;
static lv_obj_t *s_rtc_label;
static lv_obj_t *s_face_source_label;

static const char *L(desktop_text_key_t key) { return desktop_language_lookup(key); }

static void fill_rect(lv_layer_t *layer, int32_t x, int32_t y, int32_t width, int32_t height,
                      int32_t radius, lv_color_t color)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    const lv_area_t area = {.x1 = x, .y1 = y, .x2 = x + width - 1, .y2 = y + height - 1};
    lv_draw_rect(layer, &dsc, &area);
}

static void fill_circle(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t radius,
                        lv_color_t color)
{
    if (radius <= 0) {
        return;
    }
    fill_rect(layer, cx - radius, cy - radius, radius * 2, radius * 2, radius, color);
}

static void fill_triangle(lv_layer_t *layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                          int32_t x2, int32_t y2, lv_color_t color)
{
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = LV_OPA_COVER;
    dsc.p[0].x = x0;
    dsc.p[0].y = y0;
    dsc.p[1].x = x1;
    dsc.p[1].y = y1;
    dsc.p[2].x = x2;
    dsc.p[2].y = y2;
    lv_draw_triangle(layer, &dsc);
}

static void fill_quad(lv_layer_t *layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      int32_t x2, int32_t y2, int32_t x3, int32_t y3, lv_color_t color)
{
    fill_triangle(layer, x0, y0, x1, y1, x2, y2, color);
    fill_triangle(layer, x0, y0, x2, y2, x3, y3, color);
}

static void draw_radial_line(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t inner,
                             int32_t outer, int32_t angle, int32_t width, lv_color_t color)
{
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;
    if (!app_clock_face_hand_tip(cx, cy, inner, angle, &x1, &y1) ||
        !app_clock_face_hand_tip(cx, cy, outer, angle, &x2, &y2)) {
        return;
    }
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.opa = LV_OPA_COVER;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

/**
 * @brief Draw a tapered hand from fixed-point radial and perpendicular offsets.
 * @param layer LVGL drawing layer.
 * @param cx Horizontal coordinate of the center in pixels.
 * @param cy Vertical coordinate of the center in pixels.
 * @param length Hand length in pixels.
 * @param angle Hand angle in tenths of a degree.
 * @param base_half_width Half-width at the base in pixels.
 * @param tip_half_width Half-width at the tip in pixels.
 * @param color Hand color.
 */
static void draw_hand(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t length,
                      int32_t angle, int32_t base_half_width, int32_t tip_half_width,
                      lv_color_t color)
{
    if (length <= 0) {
        return;
    }
    int32_t axis_x = 0;
    int32_t axis_y = 0;
    int32_t tip_x = 0;
    int32_t tip_y = 0;

    if (!app_clock_face_hand_tip(cx, cy, 10000, angle, &axis_x, &axis_y) ||
        !app_clock_face_hand_tip(cx, cy, length, angle, &tip_x, &tip_y)) {
        return;
    }
    const int32_t unit_x = axis_x - cx;
    const int32_t unit_y = axis_y - cy;
    const int32_t tail = length / 6;
    const int32_t base_x = cx - unit_x * tail / 10000;
    const int32_t base_y = cy - unit_y * tail / 10000;

    const int32_t perp_x = -unit_y * base_half_width / 10000;
    const int32_t perp_y = unit_x * base_half_width / 10000;
    const int32_t tip_perp_x = -unit_y * tip_half_width / 10000;
    const int32_t tip_perp_y = unit_x * tip_half_width / 10000;
    fill_quad(layer, base_x + perp_x, base_y + perp_y, tip_x + tip_perp_x, tip_y + tip_perp_y,
              tip_x - tip_perp_x, tip_y - tip_perp_y, base_x - perp_x, base_y - perp_y, color);
}

/**
 * @brief Paint a drawn dial and hands into the supplied face or preview geometry.
 * @param layer LVGL drawing layer.
 * @param cx Horizontal coordinate of the dial center in pixels.
 * @param cy Vertical coordinate of the dial center in pixels.
 * @param radius Dial radius in pixels.
 * @param valid Whether the supplied time is valid.
 * @param hour Local hour.
 * @param minute Local minute.
 * @param second Local second.
 */
static void analog_paint(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t radius,
                         bool valid, uint8_t hour, uint8_t minute, uint8_t second)
{
    if (radius <= 0) {
        return;
    }

    const int32_t ring = radius / 18 > 2 ? radius / 18 : 2;
    fill_circle(layer, cx, cy, radius, C_DIAL_RING);
    fill_circle(layer, cx, cy, radius - ring, C_DIAL_FACE);

    for (int32_t index = 0; index < 60; ++index) {
        const bool major = (index % 5) == 0;
        const int32_t angle = index * 60;
        const int32_t outer = radius * 92 / 100;
        const int32_t inner = major ? radius * 80 / 100 : radius * 87 / 100;
        draw_radial_line(layer, cx, cy, inner, outer, angle, major ? 4 : 2,
                         major ? C_TICK_MAJOR : C_TICK_MINOR);
    }

    if (!valid) {
        fill_circle(layer, cx, cy, radius / 18, C_DIAL_RING);
        return;
    }

    const int32_t hour_angle = app_clock_face_hour_angle(hour, minute);
    const int32_t minute_angle = app_clock_face_minute_angle(minute, second);
    const int32_t second_angle = app_clock_face_second_angle(second);
    const int32_t thickness = radius / 9 > 0 ? radius / 9 : 1;

    draw_hand(layer, cx, cy, radius * HAND_HOUR_PERCENT / 100, hour_angle, thickness * 3 / 4,
              thickness / 3, C_HOUR_HAND);
    draw_hand(layer, cx, cy, radius * HAND_MINUTE_PERCENT / 100, minute_angle, thickness / 2,
              thickness / 6, C_MINUTE_HAND);
    draw_hand(layer, cx, cy, radius * HAND_SECOND_PERCENT / 100, second_angle, thickness / 5, 1,
              C_SECOND_HAND);
    fill_circle(layer, cx, cy, thickness / 2 + 2, C_HOUR_HAND);
}

static void flip_label_style(lv_obj_t *label, const lv_font_t *font, int32_t width,
                             int32_t height)
{
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, C_FLIP_DIGIT, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(label, width, height);
    const int32_t line = lv_font_get_line_height(font);
    lv_obj_set_style_pad_top(label, height > line ? (height - line) / 2 : 0, 0);
}

/**
 * @brief Create clipped static halves and a moving flap over the fold line.
 * @param parent Parent LVGL object.
 * @param[out] card Receives the created card objects.
 * @param x Horizontal card position in pixels.
 * @param y Vertical card position in pixels.
 * @param width Card width in pixels.
 * @param height Card height in pixels.
 * @param font Font for the digit.
 */
static void flip_card_create(lv_obj_t *parent, clock_flip_card_t *card, int32_t x, int32_t y,
                             int32_t width, int32_t height, const lv_font_t *font)
{
    memset(card, 0, sizeof(*card));
    const int32_t half = height / 2;

    card->card = lv_obj_create(parent);
    desktop_fullscreen_layout_style_panel(card->card, C_FLIP_CARD, 8);
    lv_obj_set_pos(card->card, x, y);
    lv_obj_set_size(card->card, width, height);
    lv_obj_set_style_border_width(card->card, 1, 0);
    lv_obj_set_style_border_color(card->card, C_FLIP_EDGE, 0);
    lv_obj_clear_flag(card->card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    card->top_clip = lv_obj_create(card->card);
    desktop_fullscreen_layout_style_panel(card->top_clip, C_FLIP_CARD, 0);
    lv_obj_set_pos(card->top_clip, 0, 0);
    lv_obj_set_size(card->top_clip, width, half);
    lv_obj_clear_flag(card->top_clip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    card->top_label = lv_label_create(card->top_clip);
    flip_label_style(card->top_label, font, width, height);
    lv_obj_set_pos(card->top_label, 0, 0);

    card->bottom_clip = lv_obj_create(card->card);
    desktop_fullscreen_layout_style_panel(card->bottom_clip, C_FLIP_CARD, 0);
    lv_obj_set_pos(card->bottom_clip, 0, half);
    lv_obj_set_size(card->bottom_clip, width, half);
    lv_obj_clear_flag(card->bottom_clip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    card->bottom_label = lv_label_create(card->bottom_clip);
    flip_label_style(card->bottom_label, font, width, height);
    lv_obj_set_pos(card->bottom_label, 0, -half);

    lv_obj_t *split = lv_obj_create(card->card);
    desktop_fullscreen_layout_style_panel(split, C_FLIP_SPLIT, 0);
    lv_obj_set_pos(split, 0, half - 1);
    lv_obj_set_size(split, width, 2);
    lv_obj_clear_flag(split, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    card->flap = lv_obj_create(card->card);
    desktop_fullscreen_layout_style_panel(card->flap, C_FLIP_CARD, 0);
    lv_obj_set_size(card->flap, width, half);
    lv_obj_clear_flag(card->flap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    card->flap_label = lv_label_create(card->flap);
    flip_label_style(card->flap_label, font, width, height);
    lv_obj_set_style_transform_scale_x(card->flap, 256, 0);
    lv_obj_set_style_transform_scale_y(card->flap, 256, 0);
    lv_obj_add_flag(card->flap, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief Place the moving half with its transform pivot on the fold line.
 * @param card Card whose flap is positioned.
 * @param top_half Whether the flap occupies the upper half.
 * @param width Card width in pixels.
 * @param height Card height in pixels.
 */
static void flip_card_place_flap(const clock_flip_card_t *card, bool top_half, int32_t width,
                                 int32_t height)
{
    const int32_t half = height / 2;
    lv_obj_set_pos(card->flap, 0, top_half ? 0 : half);
    lv_obj_set_size(card->flap, width, half);
    lv_obj_set_pos(card->flap_label, 0, top_half ? 0 : -half);
    lv_obj_set_style_transform_pivot_x(card->flap, width / 2, 0);
    lv_obj_set_style_transform_pivot_y(card->flap, top_half ? half : 0, 0);
    lv_obj_move_foreground(card->flap);
}

static void flip_card_set_digit(const clock_flip_card_t *card, uint8_t digit, bool valid)
{
    const char text[2] = {valid ? (char)('0' + digit) : ' ', '\0'};

    if (strcmp(lv_label_get_text(card->top_label), text) != 0) {
        lv_label_set_text(card->top_label, text);
    }
    if (strcmp(lv_label_get_text(card->bottom_label), text) != 0) {
        lv_label_set_text(card->bottom_label, text);
    }
}

/**
 * @brief Apply the current fold frame only to cards whose digits are changing.
 */
static void flip_apply(void)
{
    for (size_t index = 0; index < CLOCK_DIGITS; ++index) {
        const clock_flip_card_t *card = &s_flip_card[index];
        char new_text[2] = {s_time_valid ? (char)('0' + s_digit[index]) : ' ', '\0'};
        char old_text[2] = {s_time_valid ? (char)('0' + s_flip_old_digit[index]) : ' ',
                            '\0'};
        const bool flipping = s_flip_active && s_flip_digit_active[index] &&
                              s_flip_frame.flap_half != APP_CLOCK_FLIP_HALF_DONE;

        const char *top_text = s_flip_frame.top_shows_new ? new_text : old_text;
        const char *bottom_text = s_flip_frame.bottom_shows_new ? new_text : old_text;
        if (strcmp(lv_label_get_text(card->top_label), top_text) != 0) {
            lv_label_set_text(card->top_label, top_text);
        }
        if (strcmp(lv_label_get_text(card->bottom_label), bottom_text) != 0) {
            lv_label_set_text(card->bottom_label, bottom_text);
        }
        if (!flipping) {
            flip_card_set_digit(card, s_digit[index], s_time_valid);
            lv_obj_add_flag(card->flap, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const bool top_half = s_flip_frame.flap_half == APP_CLOCK_FLIP_HALF_TOP;
        const char *flap_text =
            s_flip_frame.flap_content == APP_CLOCK_FLIP_SHOWS_NEW ? new_text : old_text;
        if (strcmp(lv_label_get_text(card->flap_label), flap_text) != 0) {
            lv_label_set_text(card->flap_label, flap_text);
        }
        flip_card_place_flap(card, top_half, s_flip_card_width, s_flip_card_height);
        lv_obj_set_style_transform_scale_y(card->flap, s_flip_frame.scale_y, 0);
        lv_obj_remove_flag(card->flap, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Fill a convex segment using a triangle fan.
 * @param layer LVGL drawing layer.
 * @param polygon Segment polygon to draw.
 * @param color Fill color.
 */
static void draw_polygon(lv_layer_t *layer, const app_clock_seven_polygon_t *polygon,
                         lv_color_t color)
{
    for (uint8_t index = 1; index + 1 < (uint8_t)APP_CLOCK_SEVEN_POINTS; ++index) {
        fill_triangle(layer, polygon->point[0].x, polygon->point[0].y,
                      polygon->point[index].x, polygon->point[index].y,
                      polygon->point[index + 1].x, polygon->point[index + 1].y, color);
    }
}

/**
 * @brief Derive colon brightness from the neighboring digits.
 * @param intensity Segment brightness for each digit, in the range 0..255.
 * @param colon Colon index.
 * @return Maximum brightness among the segments of the two adjacent digits.
 */
static int32_t seven_colon_intensity(
    const int32_t intensity[CLOCK_DIGITS][APP_CLOCK_SEVEN_SEGMENTS], size_t colon)
{
    const size_t left_digit = colon == 0U ? 1U : 3U;
    int32_t value = 0;
    for (size_t segment = 0; segment < APP_CLOCK_SEVEN_SEGMENTS; ++segment) {
        if (intensity[left_digit][segment] > value) {
            value = intensity[left_digit][segment];
        }
        if (intensity[left_digit + 1U][segment] > value) {
            value = intensity[left_digit + 1U][segment];
        }
    }
    return value;
}

static bool area_overlaps(const lv_area_t *a, const lv_area_t *b)
{
    return !(a->x2 < b->x1 || a->x1 > b->x2 || a->y2 < b->y1 || a->y1 > b->y2);
}

/**
 * @brief Cache digit polygons, cell bounds and colon regions for the display box.
 * @param[out] tubes Receives digit and colon geometry.
 * @param x Left edge of the display box in pixels.
 * @param y Top edge of the display box in pixels.
 * @param width Display box width in pixels.
 * @param height Display box height in pixels.
 * @return true if all geometry could be built.
 */
static bool seven_tubes_build(clock_tubes_t *tubes, int32_t x, int32_t y, int32_t width,
                              int32_t height)
{
    if (tubes == NULL || width <= 0 || height <= 0) {
        return false;
    }
    if (!app_clock_seven_layout((uint16_t)width, (uint16_t)height, &tubes->layout)) {
        return false;
    }

    for (size_t digit = 0; digit < CLOCK_DIGITS; ++digit) {
        if (!app_clock_seven_polygons(tubes->layout.digit_x[digit] + x,
                                        tubes->layout.digit_y + y, tubes->layout.digit_width,
                                        tubes->layout.digit_height, tubes->layout.thickness,
                                        tubes->layout.gap, tubes->bar[digit])) {
            return false;
        }

        tubes->cell[digit].x1 = x + tubes->layout.digit_x[digit];
        tubes->cell[digit].y1 = y + tubes->layout.digit_y;
        tubes->cell[digit].x2 =
            x + tubes->layout.digit_x[digit] + tubes->layout.digit_width - 1;
        tubes->cell[digit].y2 =
            y + tubes->layout.digit_y + tubes->layout.digit_height - 1;
    }

    for (size_t colon = 0; colon < (size_t)APP_CLOCK_SEVEN_COLONS; ++colon) {
        tubes->colon[colon].x1 = x + tubes->layout.colon_x[colon];
        tubes->colon[colon].x2 =
            x + tubes->layout.colon_x[colon] + tubes->layout.colon_width - 1;
        tubes->colon[colon].y1 = y + tubes->layout.colon_dot_y[0];
        tubes->colon[colon].y2 =
            y + tubes->layout.colon_dot_y[1] + tubes->layout.thickness - 1;
    }
    return true;
}

static bool seven_build(void)
{
    s_seven_ready = false;
    if (s_face == NULL) {
        return false;
    }
    lv_obj_update_layout(s_face);
    lv_area_t coords;
    lv_obj_get_coords(s_face, &coords);
    const int32_t width = lv_obj_get_width(s_face);
    const int32_t height = lv_obj_get_height(s_face);
    if (!seven_tubes_build(&s_tubes, coords.x1, coords.y1, width, height)) {
        ESP_LOGW(TAG, "panel %dx%d is too small for the tubes", (int)width, (int)height);
        return false;
    }
    for (size_t colon = 0; colon < (size_t)APP_CLOCK_SEVEN_COLONS; ++colon) {
        s_seven_colon_value[colon] = 0;
    }
    s_seven_ready = true;
    ESP_LOGI(TAG, "tubes %dx%d digit=%dx%d thickness=%d gap=%d", (int)width, (int)height,
             (int)s_tubes.layout.digit_width, (int)s_tubes.layout.digit_height,
             (int)s_tubes.layout.thickness, (int)s_tubes.layout.gap);
    return true;
}

/**
 * @brief Invalidate only the changed digit cell.
 * @param digit Index of the digit cell to invalidate.
 */
static void seven_invalidate_digit(size_t digit)
{
    if (s_face != NULL && s_seven_ready && digit < CLOCK_DIGITS) {
        lv_obj_invalidate_area(s_face, &s_tubes.cell[digit]);
    }
}

static void seven_invalidate_colon(size_t colon)
{
    if (s_face != NULL && s_seven_ready && colon < (size_t)APP_CLOCK_SEVEN_COLONS) {
        lv_obj_invalidate_area(s_face, &s_tubes.colon[colon]);
    }
}

/**
 * @brief Paint clipped digit regions with dark segment beds beneath lit segments.
 * @param layer LVGL drawing layer.
 * @param clip Drawing clip rectangle.
 * @param tubes Cached digit and colon geometry.
 * @param intensity Segment brightness for each digit, in the range 0..255.
 * @param colon_value Brightness for each colon, in the range 0..255.
 */
static void seven_paint_tubes(lv_layer_t *layer, const lv_area_t *clip,
                              const clock_tubes_t *tubes,
                              const int32_t intensity[CLOCK_DIGITS]
                                                       [APP_CLOCK_SEVEN_SEGMENTS],
                              const int32_t colon_value[APP_CLOCK_SEVEN_COLONS])
{
    if (tubes == NULL || clip == NULL) {
        return;
    }

    const uint32_t off_rgb = lv_color_to_u32(C_TUBE_OFF) & 0xFFFFFFU;
    const uint32_t on_rgb = lv_color_to_u32(C_TUBE_ON) & 0xFFFFFFU;
    const lv_color_t bed = lv_color_hex(off_rgb);

    for (size_t digit = 0; digit < CLOCK_DIGITS; ++digit) {
        if (!area_overlaps(&tubes->cell[digit], clip)) {
            continue;
        }

        for (size_t segment = 0; segment < APP_CLOCK_SEVEN_SEGMENTS; ++segment) {
            draw_polygon(layer, &tubes->bar[digit][segment], bed);
        }

        for (size_t segment = 0; segment < APP_CLOCK_SEVEN_SEGMENTS; ++segment) {
            const int32_t value = intensity[digit][segment];
            if (value <= 0) {
                continue;
            }
            draw_polygon(layer, &tubes->bar[digit][segment],
                         lv_color_hex(app_clock_seven_blend(off_rgb, on_rgb, value)));
        }
    }

    for (size_t colon = 0; colon < (size_t)APP_CLOCK_SEVEN_COLONS; ++colon) {
        if (!area_overlaps(&tubes->colon[colon], clip)) {
            continue;
        }
        const int32_t value = colon_value[colon];
        if (value <= 0) {
            continue;
        }
        const lv_color_t color =
            lv_color_hex(app_clock_seven_blend(off_rgb, on_rgb, value));
        for (size_t dot = 0; dot < 2U; ++dot) {
            fill_rect(layer, tubes->layout.colon_dot_x[colon],
                      tubes->layout.colon_dot_y[dot], tubes->layout.thickness,
                      tubes->layout.thickness, 0, color);
        }
    }
}

static void digits_tube_path(char *out, size_t size, uint8_t digit)
{
    snprintf(out, size, CLOCK_DIGITS_TUBE_LV, (unsigned)digit);
}

static bool digits_readable(const char *path)
{
    FILE *probe = fopen(path, "rb");
    if (probe == NULL) {
        return false;
    }
    fclose(probe);
    return true;
}

/**
 * @brief Require the backdrop and all ten digit images before building the image face.
 * @return true if the backdrop and all ten digit images are available.
 */
static bool digits_art_present(void)
{
    if (!digits_readable(CLOCK_DIGITS_BACKGROUND_PROBE)) {
        return false;
    }
    char path[CLOCK_TUBE_PATH_MAX];
    for (uint8_t digit = 0U; digit < CLOCK_DIGITS_TUBES; ++digit) {
        snprintf(path, sizeof(path), CLOCK_DIGITS_TUBE_PROBE, (unsigned)digit);
        if (!digits_readable(path)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Release only cache entries belonging to the image-digit style.
 */
static void digits_cache_drop(void)
{
    lv_image_cache_drop(CLOCK_DIGITS_BACKGROUND_LV);
    char path[CLOCK_TUBE_PATH_MAX];
    for (uint8_t digit = 0U; digit < CLOCK_DIGITS_TUBES; ++digit) {
        digits_tube_path(path, sizeof(path), digit);
        lv_image_cache_drop(path);
    }
}

/**
 * @brief Clear image handles and cached assets after the parent has removed the widgets.
 */
static void digits_forget(void)
{
    s_digits_background = NULL;
    memset(s_digit_slot, 0, sizeof(s_digit_slot));
    s_digits_built = false;

    digits_cache_drop();
}

/**
 * @brief Delete any partially built image-digit widgets and release their cached assets.
 */
static void digits_teardown(void)
{
    if (s_digits_background != NULL) {
        lv_obj_delete(s_digits_background);
    }
    for (size_t index = 0; index < (size_t)CLOCK_DIGITS; ++index) {
        if (s_digit_slot[index].tube != NULL) {
            lv_obj_delete(s_digit_slot[index].tube);
        }
    }
    digits_forget();
}

/**
 * @brief Create a digit image; LVGL copies its local filename into the image object.
 * @param box Position and size of the digit image.
 * @param digit Digit image to load.
 * @return Created image object, or NULL if creation fails.
 */
static lv_obj_t *digits_tube_create(const app_clock_digits_box_t *box, uint8_t digit)
{
    lv_obj_t *tube = lv_image_create(s_face);
    if (tube == NULL) {
        return NULL;
    }
    char path[CLOCK_TUBE_PATH_MAX];
    digits_tube_path(path, sizeof(path), digit);

    lv_image_set_src(tube, path);
    lv_obj_set_pos(tube, box->x, box->y);
    lv_obj_set_size(tube, box->width, box->height);
    lv_image_set_antialias(tube, true);
    lv_obj_clear_flag(tube, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return tube;
}

/**
 * @brief Decode and cache the backdrop before constructing image widgets.
 * @return true if the backdrop can be decoded.
 */
static bool digits_backdrop_decodes(void)
{
    ESP_LOGI(TAG, "backdrop decode: psram free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, CLOCK_DIGITS_BACKGROUND_LV, NULL) != LV_RESULT_OK) {
        return false;
    }
    lv_image_decoder_close(&dsc);
    return true;
}

/**
 * @brief Build the backdrop and digit slots, cleaning up partial widget construction on failure.
 * @param width Available width in pixels.
 * @param height Available height in pixels.
 * @return true if all image widgets were created.
 */
static bool digits_build(int32_t width, int32_t height)
{
    if (s_face == NULL || !digits_art_present()) {
        return false;
    }

    digits_cache_drop();
    if (!digits_backdrop_decodes()) {
        ESP_LOGW(TAG, "no room for the clock backdrop beside the desktop; clearing the image cache");
        /** Retry the backdrop after releasing the shared image cache. */
        lv_image_cache_drop(NULL);
        if (!digits_backdrop_decodes()) {
            ESP_LOGW(TAG, "clock backdrop cannot be decoded: %s",
                     CLOCK_DIGITS_BACKGROUND_LV);
            return false;
        }
    }

    s_digits_background = lv_image_create(s_face);
    lv_obj_t *background = s_digits_background;
    if (background == NULL) {
        return false;
    }
    lv_image_set_src(background, CLOCK_DIGITS_BACKGROUND_LV);
    if (lv_image_get_src(background) == NULL) {
        ESP_LOGW(TAG, "clock backdrop could not be decoded: %s", CLOCK_DIGITS_BACKGROUND_LV);
        digits_teardown();
        return false;
    }
    lv_obj_set_pos(background, 0, 0);
    lv_obj_set_size(background, width, height);

    lv_image_set_inner_align(background, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_clear_flag(background, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (size_t index = 0; index < (size_t)CLOCK_DIGITS; ++index) {
        app_clock_digits_box_t box;
        if (!app_clock_digits_slot(index, width, height, &box)) {
            digits_teardown();
            return false;
        }
        clock_digit_slot_t *slot = &s_digit_slot[index];

        slot->tube = digits_tube_create(&box, 0U);
        if (slot->tube == NULL) {
            digits_teardown();
            return false;
        }
        s_shown_digit[index] = 0U;
    }
    s_digits_built = true;
    ESP_LOGI(TAG, "image digits in use (%dx%d)", (int)width, (int)height);
    return true;
}

/**
 * @brief Update changed image slots and hide them while local time is unavailable.
 */
static void digits_show(void)
{
    if (!s_digits_built) {
        return;
    }

    if (!s_time_valid) {
        for (size_t index = 0; index < (size_t)CLOCK_DIGITS; ++index) {
            lv_obj_add_flag(s_digit_slot[index].tube, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    for (size_t index = 0; index < (size_t)CLOCK_DIGITS; ++index) {
        clock_digit_slot_t *slot = &s_digit_slot[index];
        lv_obj_t *tube = slot->tube;
        const uint8_t digit = s_digit[index];

        if (digit == s_shown_digit[index] &&
            !lv_obj_has_flag(tube, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        s_shown_digit[index] = digit;
        lv_obj_clear_flag(tube, LV_OBJ_FLAG_HIDDEN);
        char path[CLOCK_TUBE_PATH_MAX];
        digits_tube_path(path, sizeof(path), digit);
        lv_image_set_src(tube, path);
    }
}

/**
 * @brief Create the image-style preview from a single digit image.
 * @param parent Parent LVGL object for the preview.
 */
static void digits_preview_prepare(lv_obj_t *parent)
{
    lv_obj_t *tube = lv_image_create(parent);
    if (tube == NULL) {
        return;
    }
    char path[CLOCK_TUBE_PATH_MAX];
    digits_tube_path(path, sizeof(path), 8U);
    lv_image_set_src(tube, path);
    lv_obj_set_size(tube, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(tube, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(tube, true);
    lv_obj_center(tube);
    lv_obj_clear_flag(tube, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

static const desktop_text_key_t s_weekday_keys[] = {
    DESKTOP_TEXT_WEEKDAY_SUNDAY,   DESKTOP_TEXT_WEEKDAY_MONDAY, DESKTOP_TEXT_WEEKDAY_TUESDAY,
    DESKTOP_TEXT_WEEKDAY_WEDNESDAY, DESKTOP_TEXT_WEEKDAY_THURSDAY, DESKTOP_TEXT_WEEKDAY_FRIDAY,
    DESKTOP_TEXT_WEEKDAY_SATURDAY,
};

/**
 * @brief Read local civil time, date text and HHMMSS digits for the active view.
 */
static void clock_read_time(void)
{
    if (s_sleep_mode) {
        uint8_t hour = 0, minute = 0, second = 0;
        s_time_valid = s_sleep_time_cb != NULL &&
                       s_sleep_time_cb(s_sleep_time_context, &hour, &minute, &second) &&
                       hour < 24U && minute < 60U && second < 60U;
        s_digit[0] = hour / 10U;
        s_digit[1] = hour % 10U;
        s_digit[2] = minute / 10U;
        s_digit[3] = minute % 10U;
        s_digit[4] = second / 10U;
        s_digit[5] = second % 10U;
        return;
    }

    time_service_snapshot_t snapshot;
    time_policy_calendar_t calendar;

    s_time_valid = false;
    snprintf(s_date_text, sizeof(s_date_text), "%s", L(DESKTOP_TEXT_CLOCK_WAITING));

    if (time_service_get_snapshot(&snapshot) != ESP_OK ||
        snapshot.sync_state != TIME_SYNC_SYNCHRONIZED ||
        !time_policy_is_sane_system_epoch(snapshot.synchronized_epoch)) {
        return;
    }
    const time_t now = time(NULL);
    if (!time_policy_is_sane_system_epoch((int64_t)now) ||
        !time_policy_epoch_to_calendar((int64_t)now + (int64_t)s_active_offset * 60,
                                       &calendar)) {
        return;
    }

    s_digit[0] = (uint8_t)(calendar.hour / 10U);
    s_digit[1] = (uint8_t)(calendar.hour % 10U);
    s_digit[2] = (uint8_t)(calendar.minute / 10U);
    s_digit[3] = (uint8_t)(calendar.minute % 10U);
    s_digit[4] = (uint8_t)(calendar.second / 10U);
    s_digit[5] = (uint8_t)(calendar.second % 10U);
    s_time_valid = true;
    snprintf(s_date_text, sizeof(s_date_text), L(DESKTOP_TEXT_DATE_FORMAT), calendar.year,
             calendar.month, calendar.day, L(s_weekday_keys[calendar.weekday]));
}

static void flip_begin(bool animate)
{
    bool changed = false;
    for (size_t index = 0; index < CLOCK_DIGITS; ++index) {
        s_flip_old_digit[index] = s_shown_digit[index];
        s_flip_digit_active[index] =
            animate && s_time_valid && s_shown_digit[index] != s_digit[index];
        if (s_flip_digit_active[index]) {
            changed = true;
        }
        s_shown_digit[index] = s_digit[index];
    }
    s_flip_active = changed;
    s_flip_accum16 = 0U;
    (void)app_clock_flip_frame(changed ? 0U : (uint32_t)APP_CLOCK_FLIP_PROGRESS_MAX,
                                 &s_flip_frame);
    flip_apply();
}

static void seven_begin(bool animate)
{
    if (!s_seven_ready) {
        return;
    }
    bool changed = false;
    for (size_t digit = 0; digit < CLOCK_DIGITS; ++digit) {
        const uint8_t mask = s_time_valid ? app_clock_seven_mask(s_digit[digit]) : 0U;
        for (size_t segment = 0; segment < APP_CLOCK_SEVEN_SEGMENTS; ++segment) {
            const uint8_t target = (uint8_t)((mask >> segment) & 0x1U);
            if (target != s_seg_target[digit][segment]) {
                s_seg_target[digit][segment] = target;
                changed = true;
            }
            if (!animate) {
                s_seg_intensity[digit][segment] = target != 0U ? 255 : 0;
            }
        }
    }
    if (!changed) {
        return;
    }
    if (!animate) {
        for (size_t colon = 0; colon < (size_t)APP_CLOCK_SEVEN_COLONS; ++colon) {
            s_seven_colon_value[colon] = seven_colon_intensity(s_seg_intensity, colon);
        }
        if (s_face != NULL) {
            lv_obj_invalidate(s_face);
        }
        return;
    }
    s_seven_active = true;
}

static void anim_ensure_running(void)
{
    if (s_anim == NULL) {
        return;
    }
    if (s_flip_active || s_seven_active) {
        s_fade_tick = lv_tick_get();
        lv_timer_resume(s_anim);
        lv_timer_reset(s_anim);
    }
}

static void analog_begin(void)
{
    app_clock_analog_time(s_time_valid, s_digit[0] * 10U + s_digit[1],
                            s_digit[2] * 10U + s_digit[3], s_digit[4] * 10U + s_digit[5]);
}

/**
 * @brief Advance flip or brightness animation and invalidate changed regions.
 * @param timer LVGL timer invoking the callback.
 */
static void anim_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_flip_active) {
        s_flip_accum16 += app_clock_flip_progress_step16((uint16_t)CLOCK_ANIM_PERIOD_MS);
        const uint32_t progress = s_flip_accum16 >> 4;
        (void)app_clock_flip_frame(progress, &s_flip_frame);
        if (progress >= (uint32_t)APP_CLOCK_FLIP_PROGRESS_MAX) {
            s_flip_active = false;
        }
        flip_apply();
    }
    if (s_seven_active) {
        const uint32_t fade_ms = lv_tick_elaps(s_fade_tick);

        const int32_t step = fade_ms >= CLOCK_SEGMENT_FADE_MAX_MS
                                 ? 255
                                 : (255 + (int32_t)CLOCK_SEGMENT_FADE_STEPS - 1) /
                                       (int32_t)CLOCK_SEGMENT_FADE_STEPS;
        bool busy = false;
        for (size_t digit = 0; digit < CLOCK_DIGITS; ++digit) {
            bool digit_moved = false;
            for (size_t segment = 0; segment < APP_CLOCK_SEVEN_SEGMENTS; ++segment) {
                const int32_t target = s_seg_target[digit][segment] != 0U ? 255 : 0;
                const int32_t previous = s_seg_intensity[digit][segment];
                const int32_t next = app_clock_seven_intensity_step(previous, target, step);
                s_seg_intensity[digit][segment] = next;
                if (next != target) {
                    busy = true;
                }
                if (next != previous) {
                    digit_moved = true;
                }
            }

            if (digit_moved) {
                seven_invalidate_digit(digit);
            }
        }
        s_seven_active = busy;
        for (size_t colon = 0; colon < (size_t)APP_CLOCK_SEVEN_COLONS; ++colon) {
            const int32_t value = seven_colon_intensity(s_seg_intensity, colon);
            if (value != s_seven_colon_value[colon]) {
                s_seven_colon_value[colon] = value;
                seven_invalidate_colon(colon);
            }
        }
    }
    if (!s_flip_active && !s_seven_active) {
        lv_timer_pause(s_anim);
    }
}

static void draw_text_centered(lv_layer_t *layer, const lv_font_t *font, lv_color_t color,
                               const char *text, int32_t x, int32_t y, int32_t width,
                               int32_t height)
{
    if (font == NULL || text == NULL || width <= 0 || height <= 0) {
        return;
    }
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = color;
    dsc.font = font;
    dsc.text = text;
    dsc.align = LV_TEXT_ALIGN_CENTER;
    dsc.opa = LV_OPA_COVER;
    const int32_t line = lv_font_get_line_height(font);
    const lv_area_t area = {.x1 = x,
                            .y1 = y + (height > line ? (height - line) / 2 : 0),
                            .x2 = x + width - 1,
                            .y2 = y + height - 1};
    lv_draw_label(layer, &dsc, &area);
}

static void analog_preview_cb(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (layer == NULL || target == NULL) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(target, &area);
    const int32_t width = lv_obj_get_width(target);
    const int32_t height = lv_obj_get_height(target);
    const int32_t radius = (width < height ? width : height) / 2 * 92 / 100;

    analog_paint(layer, area.x1 + width / 2, area.y1 + height / 2, radius, true, 10, 9, 35);
}

static void seven_preview_cb(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (layer == NULL || target == NULL) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(target, &area);
    const int32_t width = lv_obj_get_width(target);
    const int32_t height = lv_obj_get_height(target);

    if (!s_preview_ready || s_preview_box[0] != area.x1 || s_preview_box[1] != area.y1 ||
        s_preview_box[2] != width || s_preview_box[3] != height) {
        s_preview_ready = seven_tubes_build(&s_preview_tubes, area.x1, area.y1, width, height);
        s_preview_box[0] = area.x1;
        s_preview_box[1] = area.y1;
        s_preview_box[2] = width;
        s_preview_box[3] = height;
    }
    if (!s_preview_ready) {
        return;
    }

    static const uint8_t demo[CLOCK_DIGITS] = {1U, 2U, 3U, 4U, 5U, 6U};
    int32_t intensity[CLOCK_DIGITS][APP_CLOCK_SEVEN_SEGMENTS];
    for (size_t digit = 0; digit < CLOCK_DIGITS; ++digit) {
        const uint8_t mask = app_clock_seven_mask(demo[digit]);
        for (size_t segment = 0; segment < APP_CLOCK_SEVEN_SEGMENTS; ++segment) {
            intensity[digit][segment] = ((mask >> segment) & 0x1U) != 0U ? 255 : 0;
        }
    }
    const int32_t colon_value[APP_CLOCK_SEVEN_COLONS] = {255, 255};
    seven_paint_tubes(layer, &layer->_clip_area, &s_preview_tubes, intensity, colon_value);
}

static void flip_preview_cb(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (layer == NULL || target == NULL) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(target, &area);
    const int32_t width = lv_obj_get_width(target);
    const int32_t height = lv_obj_get_height(target);

    int32_t card_height = height * FLIP_CARD_ROW_PERCENT / 100;
    int32_t card_width = card_height * FLIP_CARD_WIDTH_PERCENT / 100;
    const int32_t width_limit = width * 70 / 100;
    if (card_width > width_limit && width_limit > 0) {
        card_width = width_limit;
        card_height = card_width * 100 / FLIP_CARD_WIDTH_PERCENT;
    }
    if (card_width <= 0 || card_height <= 0) {
        return;
    }
    const int32_t card_x = area.x1 + (width - card_width) / 2;
    const int32_t card_y = area.y1 + (height - card_height) / 2;
    const int32_t half = card_height / 2;

    fill_rect(layer, card_x, card_y, card_width, card_height, 8, C_FLIP_CARD);

    fill_rect(layer, card_x, card_y, card_width, half / 2, 6, C_FLIP_EDGE);
    fill_rect(layer, card_x, card_y + half - 1, card_width, 2, 0, C_FLIP_SPLIT);
    draw_text_centered(layer, desktop_font_get_clock_face(), C_FLIP_DIGIT, "8", card_x, card_y,
                       card_width, card_height);
}

static void settings_refresh(void);
static void settings_close(void);
static void clock_render_face(void);

static lv_obj_t *settings_label(lv_obj_t *parent, const char *text, lv_color_t color,
                                const lv_font_t *font, int32_t height)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_height(label, height);
    lv_obj_set_style_text_color(label, color, 0);
    if (font != NULL) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    return label;
}

static lv_obj_t *settings_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback,
                                 void *data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_height(button, LV_PCT(100));
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, C_ACCENT_STRONG, 0);
    lv_obj_set_style_bg_color(button, C_ACCENT_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
    lv_obj_t *caption = lv_label_create(button);
    lv_label_set_text(caption, text);
    lv_obj_set_style_text_color(caption, lv_color_white(), 0);
    lv_obj_center(caption);
    return button;
}

static void settings_scrim_cb(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) == s_settings) {
        settings_close();
    }
}

static void settings_done_cb(lv_event_t *event)
{
    (void)event;
    settings_close();
}

static void settings_adjust_cb(lv_event_t *event)
{
    const int32_t delta = (int32_t)(intptr_t)lv_event_get_user_data(event);
    s_pending_offset += delta;
    if (s_pending_offset < TIME_POLICY_MIN_OFFSET_MINUTES) {
        s_pending_offset = TIME_POLICY_MIN_OFFSET_MINUTES;
    }
    if (s_pending_offset > TIME_POLICY_MAX_OFFSET_MINUTES) {
        s_pending_offset = TIME_POLICY_MAX_OFFSET_MINUTES;
    }
    settings_refresh();
}

static void settings_apply_cb(lv_event_t *event)
{
    (void)event;
    if (s_pending_offset == s_active_offset) {
        return;
    }
    if (time_settings_apply(s_pending_offset) != ESP_OK) {
        s_pending_offset = s_active_offset;
        if (s_toast != NULL) {
            s_toast(L(DESKTOP_TEXT_CLOCK_APPLY_ERROR));
        }
        settings_refresh();
        return;
    }
    if (time_settings_save(s_pending_offset) != ESP_OK ||
        time_service_update_timezone_offset(s_pending_offset) != ESP_OK) {
        (void)time_settings_apply(s_active_offset);
        (void)time_settings_save(s_active_offset);
        s_pending_offset = s_active_offset;
        if (s_toast != NULL) {
            s_toast(L(DESKTOP_TEXT_CLOCK_APPLY_ERROR));
        }
        settings_refresh();
        return;
    }
    s_active_offset = s_pending_offset;
    if (s_toast != NULL) {
        s_toast(L(DESKTOP_TEXT_CLOCK_APPLY_SUCCESS));
    }
    settings_refresh();
}

static void face_rebuild(void);

static void style_card_cb(lv_event_t *event)
{
    const app_clock_style_t style =
        (app_clock_style_t)(intptr_t)lv_event_get_user_data(event);
    if (!app_clock_style_is_valid(style)) {
        return;
    }
    if (style != s_style) {
        s_style = style;
        (void)app_clock_prefs_save_style(style);
        face_rebuild();
        clock_read_time();

        clock_render_face();
    }
    settings_refresh();
}

static void settings_close(void)
{
    if (s_settings != NULL) {
        lv_obj_delete(s_settings);
        s_settings = NULL;
    }
    app_clock_analog_covered(false);
    s_style_card[0] = NULL;
    s_style_card[1] = NULL;
    s_style_card[2] = NULL;
    s_style_card[3] = NULL;
    s_pending_label = NULL;
    s_active_label = NULL;
    s_sync_label = NULL;
    s_rtc_label = NULL;
    s_face_source_label = NULL;

    if (s_face != NULL) {
        lv_obj_invalidate(s_face);
    }
}

static lv_obj_t *settings_style_card(lv_obj_t *parent, app_clock_style_t style,
                                     desktop_text_key_t name_key, lv_event_cb_t draw_cb,
                                     bool tube_preview)
{
    lv_obj_t *card = lv_obj_create(parent);
    desktop_fullscreen_layout_style_panel(card, C_CARD, 14);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, C_PANEL_EDGE, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, style_card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)style);

    lv_obj_t *surface = lv_obj_create(card);
    desktop_fullscreen_layout_style_panel(surface, C_CANVAS, 10);
    lv_obj_set_width(surface, LV_PCT(100));
    lv_obj_set_flex_grow(surface, 1);
    lv_obj_clear_flag(surface, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (draw_cb != NULL) {
        lv_obj_add_event_cb(surface, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    }

    if (tube_preview) {
        digits_preview_prepare(surface);
    }

    lv_obj_t *caption = lv_label_create(card);
    lv_label_set_text(caption, L(name_key));
    lv_obj_set_style_text_color(caption, C_TEXT, 0);
    lv_obj_set_style_text_font(caption, desktop_font_get_primary(), 0);
    lv_obj_set_height(caption, LV_SIZE_CONTENT);
    return card;
}

/**
 * @brief Open style previews and timezone controls above the clock face.
 */
static void settings_open(void)
{
    if (s_settings != NULL || s_view == NULL || app_clock_analog_has_modal()) {
        return;
    }

    lv_obj_t *scrim = lv_obj_create(s_view);
    desktop_fullscreen_layout_style_panel(scrim, C_CANVAS, 0);

    lv_obj_add_flag(scrim, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, settings_scrim_cb, LV_EVENT_CLICKED, NULL);
    s_settings = scrim;
    app_clock_analog_covered(true);

    lv_obj_t *panel = lv_obj_create(scrim);
    desktop_fullscreen_layout_style_panel(panel, C_PANEL, 20);

    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, C_PANEL_EDGE, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(panel, LV_PCT(94), LV_PCT(92));
    lv_obj_center(panel);
    lv_obj_set_style_pad_all(panel, 14, 0);
    lv_obj_set_style_pad_row(panel, 6, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    settings_label(panel, L(DESKTOP_TEXT_CLOCK_SETTINGS_TITLE), C_TEXT, desktop_font_get_title(),
                   LV_SIZE_CONTENT);

    lv_obj_t *style_row = lv_obj_create(panel);
    desktop_fullscreen_layout_style_panel(style_row, C_PANEL, 0);
    lv_obj_set_width(style_row, LV_PCT(100));
    lv_obj_set_height(style_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(style_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(style_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(style_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *style_title = lv_label_create(style_row);
    lv_label_set_text(style_title, L(DESKTOP_TEXT_CLOCK_STYLE_TITLE));
    lv_obj_set_style_text_color(style_title, C_TEXT, 0);
    lv_obj_set_style_text_font(style_title, desktop_font_get_primary(), 0);
    s_face_source_label = lv_label_create(style_row);
    lv_label_set_text(s_face_source_label, "");
    lv_obj_set_style_text_color(s_face_source_label, C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_face_source_label, desktop_font_get_primary(), 0);

    lv_obj_t *preview_row = lv_obj_create(panel);
    desktop_fullscreen_layout_style_panel(preview_row, C_PANEL, 0);
    lv_obj_set_width(preview_row, LV_PCT(100));
    lv_obj_set_flex_grow(preview_row, 1);
    lv_obj_set_style_pad_column(preview_row, 10, 0);
    lv_obj_set_flex_flow(preview_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(preview_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(preview_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_style_card[APP_CLOCK_STYLE_ANALOG] =
        settings_style_card(preview_row, APP_CLOCK_STYLE_ANALOG,
                            DESKTOP_TEXT_CLOCK_STYLE_ANALOG, analog_preview_cb, false);
    s_style_card[APP_CLOCK_STYLE_FLIP] =
        settings_style_card(preview_row, APP_CLOCK_STYLE_FLIP, DESKTOP_TEXT_CLOCK_STYLE_FLIP,
                            flip_preview_cb, false);
    s_style_card[APP_CLOCK_STYLE_SEVEN_SEGMENT] =
        settings_style_card(preview_row, APP_CLOCK_STYLE_SEVEN_SEGMENT,
                            DESKTOP_TEXT_CLOCK_STYLE_SEVEN, seven_preview_cb, false);
    s_style_card[APP_CLOCK_STYLE_IMAGE] =
        settings_style_card(preview_row, APP_CLOCK_STYLE_IMAGE,
                            DESKTOP_TEXT_CLOCK_STYLE_IMAGE, NULL, true);

    lv_obj_t *divider = lv_obj_create(panel);
    desktop_fullscreen_layout_style_panel(divider, C_PANEL_EDGE, 0);
    lv_obj_set_width(divider, LV_PCT(100));
    lv_obj_set_height(divider, 1);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    settings_label(panel, L(DESKTOP_TEXT_CLOCK_TIMEZONE), C_TEXT, desktop_font_get_primary(),
                   LV_SIZE_CONTENT);

    lv_obj_t *offsets = lv_obj_create(panel);
    desktop_fullscreen_layout_style_panel(offsets, C_PANEL, 0);
    lv_obj_set_width(offsets, LV_PCT(100));
    lv_obj_set_height(offsets, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(offsets, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(offsets, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(offsets, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_active_label = lv_label_create(offsets);
    lv_obj_set_style_text_color(s_active_label, C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_active_label, desktop_font_get_primary(), 0);
    s_pending_label = lv_label_create(offsets);
    lv_obj_set_style_text_color(s_pending_label, C_TEXT, 0);
    lv_obj_set_style_text_font(s_pending_label, desktop_font_get_primary(), 0);

    lv_obj_t *buttons = lv_obj_create(panel);
    desktop_fullscreen_layout_style_panel(buttons, C_PANEL, 0);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, 46);
    lv_obj_set_style_pad_column(buttons, 10, 0);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    (void)settings_button(buttons, L(DESKTOP_TEXT_CLOCK_DECREASE), settings_adjust_cb,
                          (void *)(intptr_t)(-APP_CLOCK_TIMEZONE_STEP_MINUTES));
    (void)settings_button(buttons, L(DESKTOP_TEXT_CLOCK_INCREASE), settings_adjust_cb,
                          (void *)(intptr_t)APP_CLOCK_TIMEZONE_STEP_MINUTES);
    (void)settings_button(buttons, L(DESKTOP_TEXT_CLOCK_APPLY), settings_apply_cb, NULL);

    lv_obj_t *states = lv_obj_create(panel);
    desktop_fullscreen_layout_style_panel(states, C_PANEL, 0);
    lv_obj_set_width(states, LV_PCT(100));
    lv_obj_set_height(states, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(states, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(states, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(states, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_sync_label = lv_label_create(states);
    lv_obj_set_style_text_color(s_sync_label, C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_sync_label, desktop_font_get_primary(), 0);
    s_rtc_label = lv_label_create(states);
    lv_obj_set_style_text_color(s_rtc_label, C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_rtc_label, desktop_font_get_primary(), 0);

    lv_obj_t *done_row = lv_obj_create(panel);
    desktop_fullscreen_layout_style_panel(done_row, C_PANEL, 0);
    lv_obj_set_width(done_row, LV_PCT(100));
    lv_obj_set_height(done_row, 42);
    lv_obj_set_flex_flow(done_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(done_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    (void)settings_button(done_row, L(DESKTOP_TEXT_CLOCK_STYLE_DONE), settings_done_cb, NULL);

    lv_obj_move_foreground(scrim);
    settings_refresh();
}

static const char *sync_state_text(time_sync_state_t state)
{
    return L(state == TIME_SYNC_SYNCHRONIZED ? DESKTOP_TEXT_CLOCK_SYNCHRONIZED
                                             : DESKTOP_TEXT_CLOCK_UNSYNCHRONIZED);
}

static const char *rtc_state_text(rtc_state_t state)
{
    switch (state) {
    case RTC_STATE_HEALTHY:
        return L(DESKTOP_TEXT_CLOCK_RTC_HEALTHY);
    case RTC_STATE_REPAIR_REQUIRED:
        return L(DESKTOP_TEXT_CLOCK_RTC_REPAIR_REQUIRED);
    case RTC_STATE_ERROR:
        return L(DESKTOP_TEXT_CLOCK_RTC_ERROR);
    default:
        return L(DESKTOP_TEXT_CLOCK_RTC_UNAVAILABLE);
    }
}

static void settings_refresh(void)
{
    for (size_t index = 0; index < (size_t)APP_CLOCK_STYLE_COUNT; ++index) {
        lv_obj_t *card = s_style_card[index];
        if (card == NULL) {
            continue;
        }
        const bool selected = (app_clock_style_t)index == s_style;
        lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
        lv_obj_set_style_border_color(card, selected ? C_ACCENT : C_PANEL_EDGE, 0);
        lv_obj_set_style_bg_color(card, selected ? C_ACCENT_SOFT : C_CARD, 0);
    }
    if (s_face_source_label != NULL) {
        lv_label_set_text(s_face_source_label,
                          s_style != APP_CLOCK_STYLE_ANALOG
                              ? ""
                              : (app_clock_analog_is_image() ? L(app_clock_analog_is_sd() ? DESKTOP_TEXT_CLOCK_FACE_IMAGE : DESKTOP_TEXT_CLOCK_FACE_BUILTIN)
                                               : L(DESKTOP_TEXT_CLOCK_FACE_DRAWN)));
    }
    if (s_settings == NULL) {
        return;
    }

    char offset[16];
    char line[80];
    if (s_active_label != NULL &&
        time_policy_format_display_offset(s_active_offset, offset, sizeof(offset))) {
        snprintf(line, sizeof(line), "%s  %s", L(DESKTOP_TEXT_CLOCK_ACTIVE_OFFSET), offset);
        lv_label_set_text(s_active_label, line);
    }
    if (s_pending_label != NULL &&
        time_policy_format_display_offset(s_pending_offset, offset, sizeof(offset))) {
        snprintf(line, sizeof(line), "%s  %s", L(DESKTOP_TEXT_CLOCK_PENDING_OFFSET), offset);
        lv_label_set_text(s_pending_label, line);
    }
    time_service_snapshot_t snapshot;
    if (s_sync_label != NULL && s_rtc_label != NULL &&
        time_service_get_snapshot(&snapshot) == ESP_OK) {
        snprintf(line, sizeof(line), "%s  %s", L(DESKTOP_TEXT_CLOCK_SYNC_STATE),
                 sync_state_text(snapshot.sync_state));
        lv_label_set_text(s_sync_label, line);
        snprintf(line, sizeof(line), "%s  %s", L(DESKTOP_TEXT_CLOCK_RTC_STATE),
                 rtc_state_text(snapshot.rtc_state));
        lv_label_set_text(s_rtc_label, line);
    }
}

/**
 * @brief Calculate flip-card and colon geometry for the available surface.
 * @param width Available width in pixels.
 * @param height Available height in pixels.
 * @param font Digit font.
 * @return true if the layout fits.
 */
static bool flip_layout(int32_t width, int32_t height, const lv_font_t *font)
{
    if (font == NULL || width <= 0 || height <= 0) {
        return false;
    }
    const int32_t line = lv_font_get_line_height(font);
    const int32_t available = width * 96 / 100;
    const int32_t units = (int32_t)CLOCK_DIGITS * FLIP_CARD_WIDTH_PERCENT +
                          2 * FLIP_COLON_WIDTH_PERCENT + 7 * FLIP_GAP_PERCENT;
    const int32_t unit = available * 100 / units;

    int32_t card_height = height * FLIP_CARD_ROW_PERCENT / 100;
    if (unit < card_height) {
        card_height = unit;
    }
    const int32_t font_bound = line * FLIP_CARD_FONT_PERCENT / 100;
    if (font_bound > 0 && font_bound < card_height) {
        card_height = font_bound;
    }
    if (card_height <= 0) {
        return false;
    }

    s_flip_card_height = card_height;
    s_flip_card_width = card_height * FLIP_CARD_WIDTH_PERCENT / 100;
    if (s_flip_card_width <= 0) {
        return false;
    }
    const int32_t gap = card_height * FLIP_GAP_PERCENT / 100;
    s_flip_colon_width = card_height * FLIP_COLON_WIDTH_PERCENT / 100;
    const int32_t total = (int32_t)CLOCK_DIGITS * s_flip_card_width +
                          2 * s_flip_colon_width + 7 * gap;

    uint8_t digit = 0U;
    uint8_t colon = 0U;
    int32_t cursor = (width - total) / 2;
    s_flip_card_y = (height - card_height) / 2;
    for (uint8_t slot = 0U; slot < (uint8_t)(CLOCK_DIGITS + 2U); ++slot) {
        const bool is_colon = (slot == 2U) || (slot == 5U);
        if (is_colon) {
            s_flip_colon_x[colon] = cursor;
            ++colon;
            cursor += s_flip_colon_width;
        } else {
            s_flip_card_x[digit] = cursor;
            ++digit;
            cursor += s_flip_card_width;
        }
        cursor += gap;
    }
    return true;
}

static void flip_colons_create(void)
{
    const int32_t dot = s_flip_card_height * 12 / 100 > 2 ? s_flip_card_height * 12 / 100 : 2;
    const int32_t dot_x = (s_flip_colon_width - dot) / 2;
    for (size_t colon = 0; colon < 2U; ++colon) {
        for (size_t index = 0; index < 2U; ++index) {
            lv_obj_t *object = lv_obj_create(s_face);
            desktop_fullscreen_layout_style_panel(object, C_FLIP_DIGIT, dot / 2);
            lv_obj_set_pos(object, s_flip_colon_x[colon] + dot_x,
                           s_flip_card_y + s_flip_card_height * (index == 0U ? 34 : 62) / 100);
            lv_obj_set_size(object, dot, dot);
            lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

/**
 * @brief Construct the selected face, falling back when image assets are unavailable.
 */
static void face_build(void)
{
    s_face_built = false;
    if (s_face == NULL) {
        return;
    }
    lv_obj_update_layout(s_face);
    const int32_t width = lv_obj_get_width(s_face);
    const int32_t height = lv_obj_get_height(s_face);
    if (width <= 0 || height <= 0) {
        return;
    }

    if (s_style == APP_CLOCK_STYLE_ANALOG) {
        app_clock_analog_start(s_face, width, height, s_sleep_mode, s_toast);
        app_clock_analog_covered(s_settings != NULL);
        s_face_built = true;
        return;
    }

    if (s_style == APP_CLOCK_STYLE_FLIP) {
        const lv_font_t *font = desktop_font_get_clock_face();
        if (!flip_layout(width, height, font)) {
            ESP_LOGW(TAG, "panel %dx%d is too small for the flip cards", (int)width,
                     (int)height);
            return;
        }
        for (size_t index = 0; index < CLOCK_DIGITS; ++index) {
            flip_card_create(s_face, &s_flip_card[index], s_flip_card_x[index], s_flip_card_y,
                             s_flip_card_width, s_flip_card_height, font);
        }
        flip_colons_create();
        flip_apply();
        s_face_built = true;
        return;
    }

    if (s_style == APP_CLOCK_STYLE_IMAGE) {
        if (digits_build(width, height)) {
            s_face_built = true;
            return;
        }

        ESP_LOGW(TAG, "clock digit art is unusable; drawing the tubes instead");
        s_style = APP_CLOCK_STYLE_SEVEN_SEGMENT;
        s_face_built = seven_build();
        return;
    }

    s_face_built = seven_build();
}

/**
 * @brief Release the previous style and rebuild widgets from the current preference.
 */
static void face_rebuild(void)
{
    if (s_face == NULL) {
        return;
    }

    memset(s_flip_card, 0, sizeof(s_flip_card));
    s_flip_active = false;
    s_seven_active = false;
    s_seven_ready = false;
    memset(s_seg_intensity, 0, sizeof(s_seg_intensity));
    memset(s_seg_target, 0, sizeof(s_seg_target));
    memset(s_seven_colon_value, 0, sizeof(s_seven_colon_value));
    s_first_frame = true;
    s_face_built = false;

    app_clock_analog_destroy();
    lv_obj_clean(s_face);

    if (s_digits_built) {
        digits_forget();
    }
    face_build();
    lv_obj_invalidate(s_face);
}

static void clock_face_draw_cb(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    if (layer == NULL || s_face == NULL) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(s_face, &area);
    const int32_t width = lv_obj_get_width(s_face);
    const int32_t height = lv_obj_get_height(s_face);
    if (width <= 0 || height <= 0) {
        return;
    }

    if (s_style == APP_CLOCK_STYLE_ANALOG) {
        if (app_clock_analog_is_image()) {
            return;
        }
        const int32_t radius =
            (width < height ? width : height) / 2 * DIAL_RADIUS_PERCENT / 100;
        analog_paint(layer, area.x1 + width / 2, area.y1 + height / 2, radius, s_time_valid,
                     (uint8_t)(s_digit[0] * 10U + s_digit[1]),
                     (uint8_t)(s_digit[2] * 10U + s_digit[3]),
                     (uint8_t)(s_digit[4] * 10U + s_digit[5]));
        return;
    }
    if (s_style == APP_CLOCK_STYLE_SEVEN_SEGMENT) {
        seven_paint_tubes(layer, &layer->_clip_area, &s_tubes, s_seg_intensity,
                          s_seven_colon_value);
    }

}

static void analog_gesture_cb(lv_event_t *event)
{
    if (s_style != APP_CLOCK_STYLE_ANALOG || s_sleep_mode || s_settings != NULL) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if (direction == LV_DIR_LEFT || direction == LV_DIR_RIGHT) {
        lv_event_stop_bubbling(event);
        clock_read_time();
        analog_begin();
        app_clock_analog_step(direction == LV_DIR_LEFT ? 1 : -1);
        lv_indev_wait_release(indev);
    }
}

static void settings_long_press_cb(lv_event_t *event)
{
    (void)event;
    settings_open();
}

static void hint_hide_cb(lv_timer_t *timer)
{
    if (s_hint != NULL) {
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_hint_timer == timer) {
        s_hint_timer = NULL;
    }
    lv_timer_delete(timer);
}

/**
 * @brief Render the active style, snapping its first frame and retrying pending layout.
 */
static void clock_render_face(void)
{
    if (!s_face_built) {
        face_build();
    }
    const bool animate = !s_first_frame;
    s_first_frame = false;
    switch (s_style) {
    case APP_CLOCK_STYLE_FLIP:
        flip_begin(animate);
        break;
    case APP_CLOCK_STYLE_SEVEN_SEGMENT:

        seven_begin(animate);
        break;
    case APP_CLOCK_STYLE_IMAGE:

        digits_show();
        break;
    default:
        analog_begin();

        if (s_face != NULL && !app_clock_analog_is_image()) {
            lv_obj_invalidate(s_face);
        }
        break;
    }
    anim_ensure_running();
}

/**
 * @brief Refresh time and settings; redraw the face only when it is uncovered.
 */
static void clock_tick(void)
{
    const bool covered = s_settings != NULL;
    clock_read_time();

    if (!covered) {
        if (s_date != NULL) {
            lv_label_set_text(s_date, s_date_text);
        }
        clock_render_face();
    }
    settings_refresh();
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    clock_tick();
}

/**
 * @brief Stop timers, delete the view and release style-owned image resources.
 */
void app_clock_view_destroy(void)
{
    const int64_t started_us = esp_timer_get_time();
    const bool had_page = s_view != NULL;
    const app_clock_style_t left_style = s_style;

    if (s_tick != NULL) {
        lv_timer_delete(s_tick);
        s_tick = NULL;
    }
    if (s_anim != NULL) {
        lv_timer_delete(s_anim);
        s_anim = NULL;
    }
    if (s_hint_timer != NULL) {
        lv_timer_delete(s_hint_timer);
        s_hint_timer = NULL;
    }
    app_clock_analog_destroy();
    if (s_view != NULL) {
        lv_obj_delete(s_view);
        s_view = NULL;
    }

    if (s_digits_built) {
        digits_forget();
    }
    if (had_page) {
        ESP_LOGI(TAG, "page teardown %" PRId64 " ms (style=%d)",
                 (esp_timer_get_time() - started_us) / 1000, (int)left_style);
    }

    s_face = NULL;
    s_date = NULL;
    s_hint = NULL;
    s_settings = NULL;
    memset(s_flip_card, 0, sizeof(s_flip_card));
    s_style_card[0] = NULL;
    s_style_card[1] = NULL;
    s_style_card[2] = NULL;
    s_style_card[3] = NULL;
    s_pending_label = NULL;
    s_active_label = NULL;
    s_sync_label = NULL;
    s_rtc_label = NULL;
    s_face_source_label = NULL;
    s_toast = NULL;
    s_sleep_mode = false;
    s_sleep_time_cb = NULL;
    s_sleep_time_context = NULL;
    s_time_valid = false;
    s_flip_active = false;
    s_seven_active = false;
    s_seven_ready = false;
    s_first_frame = true;
    s_fade_tick = 0U;
    memset(s_seg_intensity, 0, sizeof(s_seg_intensity));
    memset(s_seg_target, 0, sizeof(s_seg_target));
    memset(s_seven_colon_value, 0, sizeof(s_seven_colon_value));
}

/**
 * @brief Create the shared clock surface with optional app controls and timers.
 * @param parent Parent LVGL object.
 * @param toast_cb Callback used to display messages.
 * @param sleep_mode Whether to build the sleep face.
 * @param time_cb Callback supplying time to the sleep face.
 * @param time_context Context passed to time_cb.
 */
static void clock_view_build(lv_obj_t *parent, app_clock_toast_cb_t toast_cb,
                             bool sleep_mode, app_clock_time_cb_t time_cb,
                             void *time_context)
{
    app_clock_view_destroy();
    if (parent == NULL) {
        return;
    }
    app_clock_analog_new_session();
    s_toast = toast_cb;
    s_sleep_mode = sleep_mode;
    s_sleep_time_cb = time_cb;
    s_sleep_time_context = time_context;

    s_style = APP_CLOCK_STYLE_DEFAULT;
    app_clock_style_t stored = APP_CLOCK_STYLE_DEFAULT;
    if (app_clock_prefs_load_style(&stored) == ESP_OK) {
        s_style = stored;
    }

    time_service_snapshot_t snapshot;
    s_active_offset = TIME_SETTINGS_DEFAULT_OFFSET_MINUTES;
    if (time_service_get_snapshot(&snapshot) == ESP_OK) {
        s_active_offset = snapshot.timezone_offset_minutes;
    }
    if (!time_policy_is_valid_offset(s_active_offset)) {
        s_active_offset = TIME_SETTINGS_DEFAULT_OFFSET_MINUTES;
    }
    s_pending_offset = s_active_offset;

    s_view = lv_obj_create(parent);
    if (s_view == NULL) {
        if (s_toast != NULL) {
            s_toast(L(DESKTOP_TEXT_CLOCK_TIMER_ERROR));
        }
        app_clock_view_destroy();
        return;
    }

    desktop_fullscreen_layout_style_panel(s_view, C_CANVAS, 0);
    lv_obj_set_size(s_view, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_view, 0, 0);

    lv_obj_add_flag(s_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_view, LV_OBJ_FLAG_GESTURE_BUBBLE);
    if (!s_sleep_mode) {
        lv_obj_add_event_cb(s_view, settings_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_add_event_cb(s_view, analog_gesture_cb, LV_EVENT_GESTURE, NULL);
    }

    s_face = lv_obj_create(s_view);
    if (s_face == NULL) {
        app_clock_view_destroy();
        return;
    }
    desktop_fullscreen_layout_style_panel(s_face, C_CANVAS, 0);
    lv_obj_set_size(s_face, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_face, 0, 0);

    lv_obj_clear_flag(s_face, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_face, clock_face_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    if (!s_sleep_mode) {
        s_date = settings_label(s_view, "", C_FACE_TEXT, desktop_font_get_primary(), 30);
        lv_obj_add_flag(s_date, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(s_date, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_style_text_align(s_date, LV_TEXT_ALIGN_CENTER, 0);

        bool hint_seen = true;
        if (app_clock_prefs_hint_seen(&hint_seen) != ESP_OK) {
            hint_seen = true;
        }
        if (!hint_seen) {
            s_hint = lv_obj_create(s_view);
            desktop_fullscreen_layout_style_panel(s_hint, C_CARD, 14);
            lv_obj_set_style_border_width(s_hint, 1, 0);
            lv_obj_set_style_border_color(s_hint, C_PANEL_EDGE, 0);
            lv_obj_add_flag(s_hint, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_set_size(s_hint, LV_PCT(72), 36);
            lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -50);
            lv_obj_set_style_bg_opa(s_hint, LV_OPA_90, 0);
            lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t *hint_text = lv_label_create(s_hint);
            lv_label_set_text(hint_text, L(DESKTOP_TEXT_CLOCK_HINT));
            lv_obj_set_style_text_color(hint_text, C_TEXT, 0);
            lv_obj_set_style_text_font(hint_text, desktop_font_get_primary(), 0);
            lv_obj_center(hint_text);
            (void)app_clock_prefs_mark_hint_seen();
            s_hint_timer = lv_timer_create(hint_hide_cb, CLOCK_HINT_MS, NULL);
        }
    }

    s_anim = lv_timer_create(anim_cb, CLOCK_ANIM_PERIOD_MS, NULL);
    if (s_anim != NULL) {
        lv_timer_pause(s_anim);
    }
    s_first_frame = true;
    face_build();
    clock_tick();
    if (!s_sleep_mode) {
        s_tick = lv_timer_create(tick_cb, 1000, NULL);
    }
    if ((!s_sleep_mode && s_tick == NULL) || s_anim == NULL ||
        (s_sleep_mode && !s_face_built)) {
        if (s_toast != NULL) {
            s_toast(L(DESKTOP_TEXT_CLOCK_TIMER_ERROR));
        }
        app_clock_view_destroy();
    }
}

void app_clock_view_build(lv_obj_t *parent, app_clock_toast_cb_t toast_cb)
{
    clock_view_build(parent, toast_cb, false, NULL, NULL);
}

bool app_clock_view_build_sleep(lv_obj_t *parent, app_clock_time_cb_t time_cb,
                                  void *time_context)
{
    if (s_view != NULL || parent == NULL || time_cb == NULL) {
        return false;
    }
    clock_view_build(parent, NULL, true, time_cb, time_context);
    return s_view != NULL && s_face_built;
}

void app_clock_view_refresh_sleep(void)
{
    if (s_sleep_mode && s_view != NULL) {
        clock_tick();
    }
}
