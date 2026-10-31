/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Image-digit positions use the original 1024 x 600 artwork coordinates. */
#define APP_CLOCK_DIGITS_SLOTS 6U
#define APP_CLOCK_DIGITS_ART_WIDTH 1024
#define APP_CLOCK_DIGITS_ART_HEIGHT 600
#define APP_CLOCK_DIGITS_TUBE_LEFT 30
#define APP_CLOCK_DIGITS_TUBE_TOP 173
#define APP_CLOCK_DIGITS_TUBE_WIDTH 138
#define APP_CLOCK_DIGITS_TUBE_HEIGHT 266
#define APP_CLOCK_DIGITS_PITCH 166

/** @brief Destination rectangle in face pixels. */
typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} app_clock_digits_box_t;

/**
 * @brief Scale artwork coordinates to panel pixels with nearest-integer rounding.
 * @param art_pixels Coordinate or dimension in artwork pixels.
 * @param panel Corresponding panel dimension in pixels.
 * @param art_size Corresponding artwork dimension in pixels.
 * @return Scaled coordinate or dimension in panel pixels.
 */
static inline int32_t app_clock_digits_scale(int32_t art_pixels, int32_t panel,
                                               int32_t art_size)
{
    return (int32_t)(((int64_t)art_pixels * panel + art_size / 2) / art_size);
}

/**
 * @brief Calculate one digit rectangle.
 * @param index Digit slot index, from zero to APP_CLOCK_DIGITS_SLOTS - 1.
 * @param width Panel width in pixels.
 * @param height Panel height in pixels.
 * @param[out] box Receives the scaled rectangle.
 * @return true on success; false for an invalid slot, panel size or output pointer.
 */
static inline bool app_clock_digits_slot(size_t index, int32_t width, int32_t height,
                                           app_clock_digits_box_t *box)
{
    if (box == NULL || index >= (size_t)APP_CLOCK_DIGITS_SLOTS || width <= 0 || height <= 0) {
        return false;
    }
    const int32_t left = APP_CLOCK_DIGITS_TUBE_LEFT + (int32_t)index * APP_CLOCK_DIGITS_PITCH;
    box->x = app_clock_digits_scale(left, width, APP_CLOCK_DIGITS_ART_WIDTH);
    box->y = app_clock_digits_scale(APP_CLOCK_DIGITS_TUBE_TOP, height,
                                      APP_CLOCK_DIGITS_ART_HEIGHT);
    box->width = app_clock_digits_scale(APP_CLOCK_DIGITS_TUBE_WIDTH, width,
                                          APP_CLOCK_DIGITS_ART_WIDTH);
    box->height = app_clock_digits_scale(APP_CLOCK_DIGITS_TUBE_HEIGHT, height,
                                           APP_CLOCK_DIGITS_ART_HEIGHT);
    return true;
}

#ifdef __cplusplus
}
#endif
