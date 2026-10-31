/** SPDX-License-Identifier: GPL-2.0-only */
#include "time_policy.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const uint8_t s_days_in_month[12] = {
    31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U,
};

static bool format_offset(int32_t offset_minutes, bool inverted, char *buffer, size_t buffer_size)
{
    if (!time_policy_is_valid_offset(offset_minutes) || buffer == NULL || buffer_size == 0U) {
        return false;
    }

    int32_t minutes = inverted ? -offset_minutes : offset_minutes;
    const char sign = minutes < 0 ? '-' : '+';
    if (minutes < 0) {
        minutes = -minutes;
    }
    const int32_t hours = minutes / 60;
    const int32_t remainder = minutes % 60;
    const int written = remainder == 0
        ? (sign == '-' ? snprintf(buffer, buffer_size, "UTC-%" PRId32, hours)
                       : snprintf(buffer, buffer_size, "UTC%" PRId32, hours))
        : (sign == '-' ? snprintf(buffer, buffer_size, "UTC-%" PRId32 ":%02" PRId32, hours, remainder)
                       : snprintf(buffer, buffer_size, "UTC%" PRId32 ":%02" PRId32, hours, remainder));
    return written >= 0 && (size_t)written < buffer_size;
}

static bool time_policy_is_valid_rtc_calendar(const time_policy_calendar_t *calendar)
{
    return calendar != NULL && calendar->year >= TIME_POLICY_RTC_MIN_YEAR &&
           calendar->year <= TIME_POLICY_MAX_YEAR &&
           time_policy_calendar_is_valid(calendar);
}

bool time_policy_is_valid_offset(int32_t offset_minutes)
{
    return offset_minutes >= TIME_POLICY_MIN_OFFSET_MINUTES &&
           offset_minutes <= TIME_POLICY_MAX_OFFSET_MINUTES &&
           offset_minutes % TIME_POLICY_OFFSET_STEP_MINUTES == 0;
}

bool time_policy_format_display_offset(int32_t offset_minutes, char *buffer, size_t buffer_size)
{
    if (!time_policy_is_valid_offset(offset_minutes) || buffer == NULL || buffer_size == 0U) {
        return false;
    }

    const char sign = offset_minutes < 0 ? '-' : '+';
    int32_t minutes = offset_minutes < 0 ? -offset_minutes : offset_minutes;
    const int32_t hours = minutes / 60;
    const int32_t remainder = minutes % 60;
    const int written = snprintf(buffer, buffer_size, "UTC%c%02" PRId32 ":%02" PRId32, sign, hours, remainder);
    return written >= 0 && (size_t)written < buffer_size;
}

bool time_policy_format_posix_tz(int32_t offset_minutes, char *buffer, size_t buffer_size)
{
    return format_offset(offset_minutes, true, buffer, buffer_size);
}

bool time_policy_is_leap_year(uint16_t year)
{
    return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}

uint8_t time_policy_days_in_month(uint16_t year, uint8_t month)
{
    if (month < 1U || month > 12U) {
        return 0U;
    }
    if (month == 2U && time_policy_is_leap_year(year)) {
        return 29U;
    }
    return s_days_in_month[month - 1U];
}

static uint8_t weekday_for_days(int64_t days_since_epoch)
{
    int64_t weekday = (days_since_epoch + 4) % 7;
    if (weekday < 0) {
        weekday += 7;
    }
    return (uint8_t)weekday;
}

static int64_t days_before_year(uint16_t year)
{
    int64_t days = 0;
    for (uint16_t current = 1970U; current < year; ++current) {
        days += time_policy_is_leap_year(current) ? 366 : 365;
    }
    return days;
}

bool time_policy_calendar_is_valid(const time_policy_calendar_t *calendar)
{
    if (calendar == NULL || calendar->year < TIME_POLICY_MIN_YEAR ||
        calendar->year > TIME_POLICY_MAX_YEAR || calendar->month < 1U ||
        calendar->month > 12U || calendar->day < 1U ||
        calendar->day > time_policy_days_in_month(calendar->year, calendar->month) ||
        calendar->weekday > 6U || calendar->hour > 23U || calendar->minute > 59U ||
        calendar->second > 59U) {
        return false;
    }

    int64_t epoch = 0;
    return time_policy_calendar_to_epoch(calendar, &epoch);
}

bool time_policy_calendar_to_epoch(const time_policy_calendar_t *calendar, int64_t *epoch)
{
    if (calendar == NULL || epoch == NULL || calendar->year < TIME_POLICY_MIN_YEAR ||
        calendar->year > TIME_POLICY_MAX_YEAR || calendar->month < 1U ||
        calendar->month > 12U || calendar->day < 1U ||
        calendar->day > time_policy_days_in_month(calendar->year, calendar->month) ||
        calendar->weekday > 6U || calendar->hour > 23U || calendar->minute > 59U ||
        calendar->second > 59U) {
        return false;
    }

    int64_t days = days_before_year(calendar->year);
    for (uint8_t month = 1U; month < calendar->month; ++month) {
        days += time_policy_days_in_month(calendar->year, month);
    }
    days += (int64_t)calendar->day - 1;
    if (weekday_for_days(days) != calendar->weekday) {
        return false;
    }
    *epoch = days * 86400 + (int64_t)calendar->hour * 3600 +
             (int64_t)calendar->minute * 60 + calendar->second;
    return true;
}

bool time_policy_epoch_to_calendar(int64_t epoch, time_policy_calendar_t *calendar)
{
    const int64_t min_epoch = 946684800;
    const int64_t max_epoch = 4102444799;
    if (calendar == NULL || epoch < min_epoch || epoch > max_epoch) {
        return false;
    }

    int64_t days = epoch / 86400;
    int64_t seconds = epoch % 86400;
    if (seconds < 0) {
        seconds += 86400;
        --days;
    }

    uint16_t year = 1970U;
    while (year < TIME_POLICY_MAX_YEAR && days >= (time_policy_is_leap_year(year) ? 366 : 365)) {
        days -= time_policy_is_leap_year(year) ? 366 : 365;
        ++year;
    }
    if (year < TIME_POLICY_MIN_YEAR || days < 0) {
        return false;
    }

    uint8_t month = 1U;
    while (month < 12U && days >= time_policy_days_in_month(year, month)) {
        days -= time_policy_days_in_month(year, month);
        ++month;
    }

    calendar->year = year;
    calendar->month = month;
    calendar->day = (uint8_t)(days + 1);
    calendar->weekday = weekday_for_days(epoch / 86400);
    calendar->hour = (uint8_t)(seconds / 3600);
    calendar->minute = (uint8_t)((seconds % 3600) / 60);
    calendar->second = (uint8_t)(seconds % 60);
    return true;
}

bool time_policy_is_sane_system_epoch(int64_t epoch)
{
    return epoch >= TIME_POLICY_VALID_EPOCH && epoch <= 4102444799;
}

int64_t time_policy_absolute_drift_seconds(int64_t left_epoch, int64_t right_epoch)
{
    if (left_epoch >= right_epoch) {
        if (right_epoch < 0 && left_epoch > INT64_MAX + right_epoch) {
            return INT64_MAX;
        }
        return left_epoch - right_epoch;
    }
    if (left_epoch < 0 && right_epoch > INT64_MAX + left_epoch) {
        return INT64_MAX;
    }
    return right_epoch - left_epoch;
}

time_policy_rtc_decision_t time_policy_decide_rtc(
    const time_policy_rtc_observation_t *observation,
    int64_t synchronized_epoch)
{
    if (observation == NULL || !observation->available) {
        return TIME_POLICY_RTC_UNAVAILABLE;
    }
    if (!observation->status_read_ok || !observation->calendar_read_ok) {
        return TIME_POLICY_RTC_ERROR;
    }
    if (!time_policy_is_sane_system_epoch(synchronized_epoch)) {
        return TIME_POLICY_RTC_ERROR;
    }
    if (!observation->calendar_valid || observation->power_on_reset ||
        observation->oscillator_stopped || observation->voltage_drop) {
        return TIME_POLICY_RTC_REPAIR_REQUIRED;
    }

    int64_t rtc_epoch = 0;
    if (!time_policy_is_valid_rtc_calendar(&observation->calendar) ||
        !time_policy_calendar_to_epoch(&observation->calendar, &rtc_epoch)) {
        return TIME_POLICY_RTC_REPAIR_REQUIRED;
    }
    return time_policy_absolute_drift_seconds(rtc_epoch, synchronized_epoch) <=
               TIME_POLICY_RTC_DRIFT_THRESHOLD_SECONDS
        ? TIME_POLICY_RTC_HEALTHY
        : TIME_POLICY_RTC_REPAIR_REQUIRED;
}

bool time_policy_is_rtc_readback_acceptable(
    const time_policy_rtc_observation_t *observation,
    int64_t synchronized_epoch)
{
    int64_t rtc_epoch = 0;

    if (observation == NULL || !observation->available || !observation->status_read_ok ||
        !observation->calendar_read_ok || !observation->calendar_valid ||
        observation->power_on_reset || observation->oscillator_stopped ||
        observation->voltage_drop || !time_policy_is_sane_system_epoch(synchronized_epoch) ||
        !time_policy_is_valid_rtc_calendar(&observation->calendar) ||
        !time_policy_calendar_to_epoch(&observation->calendar, &rtc_epoch)) {
        return false;
    }

    return time_policy_absolute_drift_seconds(rtc_epoch, synchronized_epoch) <=
           TIME_POLICY_RTC_READBACK_TOLERANCE_SECONDS;
}
