/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Full animation progress; values at or above this limit are complete. */
#define APP_CLOCK_FLIP_PROGRESS_MAX 256

/** @brief Total downward flip duration in milliseconds. */
#define APP_CLOCK_FLIP_DURATION_MS 260U

typedef enum {
    APP_CLOCK_FLIP_HALF_TOP = 0,
    APP_CLOCK_FLIP_HALF_BOTTOM,
    APP_CLOCK_FLIP_HALF_DONE,
} app_clock_flip_half_t;

typedef enum {
    APP_CLOCK_FLIP_SHOWS_OLD = 0,
    APP_CLOCK_FLIP_SHOWS_NEW,
} app_clock_flip_content_t;

typedef struct {
    app_clock_flip_half_t flap_half; /**< Moving half, or DONE when both static halves show the new digit. */

    app_clock_flip_content_t flap_content; /**< Digit content carried by the moving half. */

    int32_t scale_y; /**< Vertical scale about the fold line: 0 is flat and 256 is fully unfolded. */

    bool top_shows_new; /**< Select old or new content for each static half. */
    bool bottom_shows_new;
} app_clock_flip_frame_t;

/**
 * @brief Return frame progress in sixteenths; accumulate and shift right by four.
 * @param frame_period_ms Frame period in milliseconds.
 * @return Progress increment in sixteenths of an animation progress unit.
 */
uint32_t app_clock_flip_progress_step16(uint16_t frame_period_ms);

/**
 * @brief Resolve the two downward fold phases.
 * @param progress Animation progress; APP_CLOCK_FLIP_PROGRESS_MAX marks completion.
 * @param[out] frame Receives the resolved frame.
 * @return false if frame is NULL; true otherwise.
 */
bool app_clock_flip_frame(uint32_t progress, app_clock_flip_frame_t *frame);

#ifdef __cplusplus
}
#endif
