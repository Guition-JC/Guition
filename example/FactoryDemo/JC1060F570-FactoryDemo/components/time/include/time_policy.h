/** SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIME_POLICY_VALID_EPOCH ((int64_t)1704067200)
#define TIME_POLICY_MIN_OFFSET_MINUTES (-720)
#define TIME_POLICY_MAX_OFFSET_MINUTES (840)
#define TIME_POLICY_OFFSET_STEP_MINUTES (15)
#define TIME_POLICY_DEFAULT_OFFSET_MINUTES (480)
#define TIME_POLICY_RTC_DRIFT_THRESHOLD_SECONDS (120)
#define TIME_POLICY_RTC_READBACK_TOLERANCE_SECONDS (2)
#define TIME_POLICY_MIN_YEAR (2000U)
#define TIME_POLICY_RTC_MIN_YEAR (2024U)
#define TIME_POLICY_MAX_YEAR (2099U)

typedef enum {
    TIME_SYNC_UNSYNCHRONIZED = 0,
    TIME_SYNC_SYNCHRONIZED,
} time_sync_state_t;

typedef enum {
    RTC_STATE_UNAVAILABLE = 0,
    RTC_STATE_HEALTHY,
    RTC_STATE_REPAIR_REQUIRED,
    RTC_STATE_ERROR,
} rtc_state_t;

typedef enum {
    TIME_POLICY_RTC_HEALTHY = 0,
    TIME_POLICY_RTC_REPAIR_REQUIRED,
    TIME_POLICY_RTC_UNAVAILABLE,
    TIME_POLICY_RTC_ERROR,
} time_policy_rtc_decision_t;

/** @brief UTC calendar fields used by time policy. */
typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} time_policy_calendar_t;

/** @brief Synchronization state and timezone offset. */
typedef struct {
    time_sync_state_t sync_state;
    rtc_state_t rtc_state;
    int64_t synchronized_epoch;
    int32_t timezone_offset_minutes;
} time_status_snapshot_t;

/** @brief RTC communication, validity and calendar observations. */
typedef struct {
    bool available;
    bool status_read_ok;
    bool calendar_read_ok;
    bool calendar_valid;
    bool power_on_reset;
    bool oscillator_stopped;
    bool voltage_drop;
    time_policy_calendar_t calendar;
} time_policy_rtc_observation_t;

/**
 * @brief Check timezone offset range and 15-minute alignment.
 * @param offset_minutes Timezone offset in minutes.
 * @return true if the offset is in range and aligned to 15 minutes.
 */
bool time_policy_is_valid_offset(int32_t offset_minutes);
/**
 * @brief Format a timezone offset in minutes for display.
 * @param offset_minutes Timezone offset in minutes.
 * @param[out] buffer Receives the formatted offset.
 * @param buffer_size Buffer capacity in bytes.
 * @return true if the display offset was formatted successfully.
 */
bool time_policy_format_display_offset(int32_t offset_minutes, char *buffer, size_t buffer_size);
/**
 * @brief Format a minute offset as a POSIX timezone string.
 * @param offset_minutes Timezone offset in minutes.
 * @param[out] buffer Receives the POSIX timezone string.
 * @param buffer_size Buffer capacity in bytes.
 * @return true if the POSIX timezone string was formatted successfully.
 */
bool time_policy_format_posix_tz(int32_t offset_minutes, char *buffer, size_t buffer_size);

/**
 * @brief Return whether the Gregorian year is a leap year.
 * @param year Calendar year.
 * @return true if the Gregorian year is a leap year; false otherwise.
 */
bool time_policy_is_leap_year(uint16_t year);
/**
 * @brief Return the number of days in the given month.
 * @param year Calendar year.
 * @param month Calendar month.
 * @return The number of days in the given month.
 */
uint8_t time_policy_days_in_month(uint16_t year, uint8_t month);
/**
 * @brief Validate supported calendar fields and date ranges.
 * @param calendar Calendar fields.
 * @return true if the calendar fields and date are valid.
 */
bool time_policy_calendar_is_valid(const time_policy_calendar_t *calendar);
/**
 * @brief Convert a valid UTC calendar to epoch seconds.
 * @param calendar Calendar fields.
 * @param[out] epoch Receives the epoch in seconds.
 * @return true if the calendar was converted to epoch seconds.
 */
bool time_policy_calendar_to_epoch(const time_policy_calendar_t *calendar, int64_t *epoch);
/**
 * @brief Convert UTC epoch seconds to calendar fields.
 * @param epoch Epoch time in seconds.
 * @param[out] calendar Receives the UTC calendar fields.
 * @return true if the epoch was converted to calendar fields.
 */
bool time_policy_epoch_to_calendar(int64_t epoch, time_policy_calendar_t *calendar);
/**
 * @brief Check whether epoch seconds meet the system validity threshold.
 * @param epoch Epoch time in seconds.
 * @return true if epoch seconds meet the system validity threshold; false otherwise.
 */
bool time_policy_is_sane_system_epoch(int64_t epoch);
/**
 * @brief Return the absolute time difference in seconds.
 * @param left_epoch First epoch in seconds.
 * @param right_epoch Second epoch in seconds.
 * @return The absolute time difference in seconds.
 */
int64_t time_policy_absolute_drift_seconds(int64_t left_epoch, int64_t right_epoch);

/**
 * @brief Determine RTC health or repair requirements from synchronized time.
 * @param observation RTC observations.
 * @param synchronized_epoch Synchronized epoch in seconds.
 * @return RTC health or repair requirements from synchronized time.
 */
time_policy_rtc_decision_t time_policy_decide_rtc(
    const time_policy_rtc_observation_t *observation,
    int64_t synchronized_epoch);
/**
 * @brief Check RTC readback validity and drift tolerance.
 * @param observation RTC observations.
 * @param synchronized_epoch Synchronized epoch in seconds.
 * @return true if the RTC readback is valid and within drift tolerance.
 */
bool time_policy_is_rtc_readback_acceptable(
    const time_policy_rtc_observation_t *observation,
    int64_t synchronized_epoch);

#ifdef __cplusplus
}
#endif
