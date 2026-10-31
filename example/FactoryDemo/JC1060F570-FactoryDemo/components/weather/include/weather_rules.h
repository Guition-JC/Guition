/** SPDX-License-Identifier: GPL-2.0-only */
/** Provider-independent weather rules: condition codes, wind direction, unit conversion and the status-bar line. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Weather shown in the status bar and on the settings page. */
typedef enum {
    WEATHER_CONDITION_UNKNOWN = 0,
    WEATHER_CONDITION_CLEAR,
    WEATHER_CONDITION_PARTLY_CLOUDY,
    WEATHER_CONDITION_CLOUDY,
    WEATHER_CONDITION_OVERCAST,
    WEATHER_CONDITION_RAIN,
    WEATHER_CONDITION_THUNDER,
    WEATHER_CONDITION_SNOW,
    WEATHER_CONDITION_FOG,
    WEATHER_CONDITION_HAZE,
    WEATHER_CONDITION_WIND,
    WEATHER_CONDITION_COUNT,
} weather_condition_t;

/** @brief Eight-point compass. The provider's degrees or its own text both land here. */
typedef enum {
    WEATHER_WIND_NORTH = 0,
    WEATHER_WIND_NORTH_EAST,
    WEATHER_WIND_EAST,
    WEATHER_WIND_SOUTH_EAST,
    WEATHER_WIND_SOUTH,
    WEATHER_WIND_SOUTH_WEST,
    WEATHER_WIND_WEST,
    WEATHER_WIND_NORTH_WEST,
    WEATHER_WIND_COUNT,
} weather_wind_dir_t;

/** @brief Persistent field-mask bits; append new bits without changing existing values. */
typedef enum {
    WEATHER_FIELD_CONDITION_ICON = 1u << 0,
    WEATHER_FIELD_CONDITION_TEXT = 1u << 1,
    WEATHER_FIELD_TEMPERATURE = 1u << 2,
    WEATHER_FIELD_HUMIDITY = 1u << 3,
    WEATHER_FIELD_WIND_ARROW = 1u << 4,
    WEATHER_FIELD_WIND_TEXT = 1u << 5,
    WEATHER_FIELD_WIND_SPEED = 1u << 6,
    WEATHER_FIELD_PLACE = 1u << 7,
    WEATHER_FIELD_ALL = 0xFFu,
} weather_field_t;

/** @brief Everything the status bar lines need, already localised by the caller. */
typedef struct {
    const char *place;
    const char *condition;
    const char *wind_direction;
    int temp_c;
    int humidity; /**< Relative humidity in percent; negative when unavailable. */
    int wind_dmps; /**< Wind speed in 0.1 m/s; negative when unavailable. */
} weather_display_t;

/**
 * @brief Format the resolved location when enabled in the field mask.
 * @param[out] out Receives the formatted location.
 * @param capacity Capacity of the destination buffer.
 * @param fields Enabled display fields.
 * @param display Weather data to format.
 * @return Number of characters written.
 */
size_t weather_display_place_format(char *out, size_t capacity, uint8_t fields,
                                    const weather_display_t *display);

/**
 * @brief Map an OpenWeather `weather[0].id` to a condition.
 * @param code Condition code.
 * @return Condition corresponding to the OpenWeather code.
 */
weather_condition_t weather_condition_from_openweather(int code);

/**
 * @brief Map a Seniverse `now.code` to a condition.
 * @param code Condition code.
 * @return Condition corresponding to the Seniverse code.
 */
weather_condition_t weather_condition_from_seniverse(int code);

/**
 * @brief Map Chinese or English condition text; return UNKNOWN when unrecognized so callers may fall back to the code.
 * @param text Text to use.
 * @return Mapped condition, or UNKNOWN if the text is unrecognized.
 */
weather_condition_t weather_condition_from_text(const char *text);

/**
 * @brief Compass point for a meteorological bearing in degrees (0 = north).
 * @param degrees Bearing in degrees.
 * @return Compass point corresponding to the bearing.
 */
weather_wind_dir_t weather_wind_dir_from_degrees(int degrees);

/**
 * @brief Parse Chinese or English compass text; unknown values fall back to north.
 * @param text Text to use.
 * @return Parsed compass point, or north for unknown text.
 */
weather_wind_dir_t weather_wind_dir_from_text(const char *text);

/**
 * @brief Seniverse reports km/h; the status bar shows m/s with one decimal.
 * @param kmh Wind speed in kilometres per hour.
 * @return Wind speed in tenths of metres per second.
 */
int weather_wind_dmps_from_kmh(int kmh);

/**
 * @brief OpenWeather reports m/s as a float; callers hand over thousandths.
 * @param milli_mps Wind speed in thousandths of metres per second.
 * @return Wind speed in tenths of metres per second.
 */
int weather_wind_dmps_from_mps_milli(int milli_mps);

/**
 * @brief Stable machine names for the web API and the icon file names.
 * @param condition Weather condition.
 * @return Stable machine name of the condition.
 */
const char *weather_condition_name(weather_condition_t condition);
/**
 * @brief Return the stable compass-direction name.
 * @param direction Direction to use.
 * @return Stable compass-direction name.
 */
const char *weather_wind_dir_name(weather_wind_dir_t direction);

/**
 * @brief Format enabled condition, temperature and humidity fields; omit unavailable values and return the written length.
 * @param[out] out Receives the formatted weather text.
 * @param capacity Capacity of the destination buffer.
 * @param fields Enabled display fields.
 * @param display Weather data to format.
 * @return Number of characters written.
 */
size_t weather_display_text_format(char *out, size_t capacity, uint8_t fields,
                                   const weather_display_t *display);

/**
 * @brief Format enabled wind direction and speed fields, omitting unavailable values.
 * @param[out] out Receives the formatted wind text.
 * @param capacity Capacity of the destination buffer.
 * @param fields Enabled display fields.
 * @param display Weather data to format.
 * @return Length of the formatted wind text.
 */
size_t weather_display_wind_format(char *out, size_t capacity, uint8_t fields,
                                   const weather_display_t *display);

#ifdef __cplusplus
}
#endif
