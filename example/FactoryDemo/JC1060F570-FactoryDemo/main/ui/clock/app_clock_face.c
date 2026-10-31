/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "app_clock_face.h"

#include <stddef.h>

/** @brief Fixed-point sine scale; one quadrant spans 900 tenths of a degree. */
#define FACE_SIN_SCALE 10000
#define FACE_QUADRANT 900

/** @brief Sine values for whole degrees from 0 through 90. */
static const int32_t s_sin_deg[91] = {
        0,   175,   349,   523,   698,   872,  1045,  1219,  1392,  1564,
     1736,  1908,  2079,  2250,  2419,  2588,  2756,  2924,  3090,  3256,
     3420,  3584,  3746,  3907,  4067,  4226,  4384,  4540,  4695,  4848,
     5000,  5150,  5299,  5446,  5592,  5736,  5878,  6018,  6157,  6293,
     6428,  6561,  6691,  6820,  6947,  7071,  7193,  7314,  7431,  7547,
     7660,  7771,  7880,  7986,  8090,  8192,  8290,  8387,  8480,  8572,
     8660,  8746,  8829,  8910,  8988,  9063,  9135,  9205,  9272,  9336,
     9397,  9455,  9511,  9563,  9613,  9659,  9703,  9744,  9781,  9816,
     9848,  9877,  9903,  9925,  9945,  9962,  9976,  9986,  9994,  9998,
    10000,
};

/**
 * @brief Interpolate a first-quadrant sine in tenths of a degree at scale 10000.
 * @param tenths Angle within the first quadrant, in tenths of a degree.
 * @return Sine scaled by 10000.
 */
static int32_t sin_tenths(int32_t tenths)
{
    if (tenths <= 0) {
        return 0;
    }
    if (tenths >= FACE_QUADRANT) {
        return FACE_SIN_SCALE;
    }
    const int32_t degree = tenths / 10;
    const int32_t fraction = tenths % 10;
    const int32_t base = s_sin_deg[degree];
    const int32_t delta = s_sin_deg[degree + 1] - base;
    return base + delta * fraction / 10;
}

static int32_t scale_to_length(int32_t length, int32_t unit)
{
    return (int32_t)(((int64_t)length * (int64_t)unit) / FACE_SIN_SCALE);
}

int32_t app_clock_face_hour_angle(uint8_t hour, uint8_t minute)
{
    const int32_t hour_of_face = (int32_t)(hour % 12U);

    return hour_of_face * 300 + (int32_t)minute * 5;
}

int32_t app_clock_face_minute_angle(uint8_t minute, uint8_t second)
{
    return (int32_t)minute * 60 + (int32_t)second;
}

int32_t app_clock_face_second_angle(uint8_t second)
{
    return (int32_t)second * 60;
}

bool app_clock_face_hand_tip(int32_t cx, int32_t cy, int32_t length, int32_t angle_tenths,
                               int32_t *tip_x, int32_t *tip_y)
{
    if (tip_x == NULL || tip_y == NULL || length < 0) {
        return false;
    }

    int32_t angle = angle_tenths % APP_CLOCK_ANGLE_TURN;
    if (angle < 0) {
        angle += APP_CLOCK_ANGLE_TURN;
    }

    const int32_t quadrant = angle / FACE_QUADRANT;
    const int32_t within = angle % FACE_QUADRANT;
    const int32_t sin_within = sin_tenths(within);
    const int32_t cos_within = sin_tenths(FACE_QUADRANT - within);

    /** Map fixed-point offsets clockwise from twelve o'clock. */
    int32_t unit_dx = 0;
    int32_t unit_dy = 0;
    switch (quadrant) {
    case 0:
        unit_dx = sin_within;
        unit_dy = -cos_within;
        break;
    case 1:
        unit_dx = cos_within;
        unit_dy = sin_within;
        break;
    case 2:
        unit_dx = -sin_within;
        unit_dy = cos_within;
        break;
    default:
        unit_dx = -cos_within;
        unit_dy = -sin_within;
        break;
    }

    *tip_x = cx + scale_to_length(length, unit_dx);
    *tip_y = cy + scale_to_length(length, unit_dy);
    return true;
}
