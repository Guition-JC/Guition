/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Angles use tenths of a degree, clockwise from twelve o'clock. */
#define APP_CLOCK_ANGLE_TURN 3600

/**
 * @brief Compute the hour-hand angle including minute progress.
 * @param hour Hour component of local time.
 * @param minute Minute component of local time.
 * @return Clockwise angle from twelve o'clock, in tenths of a degree.
 */
int32_t app_clock_face_hour_angle(uint8_t hour, uint8_t minute);

/**
 * @brief Compute the minute-hand angle including second progress.
 * @param minute Minute component of local time.
 * @param second Second component of local time.
 * @return Clockwise angle from twelve o'clock, in tenths of a degree.
 */
int32_t app_clock_face_minute_angle(uint8_t minute, uint8_t second);

/**
 * @brief Compute the second-hand angle in steps of 60 angle units.
 * @param second Second component of local time.
 * @return Clockwise angle from twelve o'clock, in tenths of a degree.
 */
int32_t app_clock_face_second_angle(uint8_t second);

/**
 * @brief Compute the hand tip in pixels; zero length returns the center.
 * @param cx Horizontal center coordinate in pixels.
 * @param cy Vertical center coordinate in pixels.
 * @param length Hand length in pixels; must be nonnegative.
 * @param angle_tenths Clockwise angle from twelve o'clock, in tenths of a degree.
 * @param[out] tip_x Receives the horizontal tip coordinate.
 * @param[out] tip_y Receives the vertical tip coordinate.
 * @return false for null output pointers or negative length; true otherwise.
 */
bool app_clock_face_hand_tip(int32_t cx, int32_t cy, int32_t length, int32_t angle_tenths,
                               int32_t *tip_x, int32_t *tip_y);

#ifdef __cplusplus
}
#endif
