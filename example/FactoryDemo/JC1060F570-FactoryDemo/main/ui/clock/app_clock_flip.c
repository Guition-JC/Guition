/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "app_clock_flip.h"

#include <stddef.h>

/** @brief The old upper half folds before the new lower half unfolds. */
#define FLIP_PHASE_BOUNDARY (APP_CLOCK_FLIP_PROGRESS_MAX / 2)

/** @brief Full flap height in 1/256 scale units. */
#define FLIP_SCALE_FULL 256

uint32_t app_clock_flip_progress_step16(uint16_t frame_period_ms)
{
    if (frame_period_ms == 0U) {
        return 1U;
    }
    return ((uint32_t)APP_CLOCK_FLIP_PROGRESS_MAX * 16U * (uint32_t)frame_period_ms) /
           APP_CLOCK_FLIP_DURATION_MS;
}

bool app_clock_flip_frame(uint32_t progress, app_clock_flip_frame_t *frame)
{
    if (frame == NULL) {
        return false;
    }

    if (progress >= APP_CLOCK_FLIP_PROGRESS_MAX) {
        frame->flap_half = APP_CLOCK_FLIP_HALF_DONE;
        frame->flap_content = APP_CLOCK_FLIP_SHOWS_NEW;
        frame->scale_y = 0;
        frame->top_shows_new = true;
        frame->bottom_shows_new = true;
        return true;
    }

    if (progress < FLIP_PHASE_BOUNDARY) {
        /** Fold the old upper half over the new static upper half. */
        frame->flap_half = APP_CLOCK_FLIP_HALF_TOP;
        frame->flap_content = APP_CLOCK_FLIP_SHOWS_OLD;
        frame->scale_y = FLIP_SCALE_FULL - (int32_t)progress * 2;
        frame->top_shows_new = true;
        frame->bottom_shows_new = false;
        return true;
    }

    /** Unfold the new lower half onto the new static lower half. */
    frame->flap_half = APP_CLOCK_FLIP_HALF_BOTTOM;
    frame->flap_content = APP_CLOCK_FLIP_SHOWS_NEW;
    frame->scale_y = ((int32_t)progress - FLIP_PHASE_BOUNDARY) * 2;
    frame->top_shows_new = true;
    frame->bottom_shows_new = true;
    return true;
}
