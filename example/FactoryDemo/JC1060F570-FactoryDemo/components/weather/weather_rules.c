/** SPDX-License-Identifier: GPL-2.0-only */
#include "weather_rules.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief OpenWeather groups its condition codes by hundreds: 2xx thunder, 3xx drizzle, 5xx rain,
 * 6xx snow, 7xx atmosphere, 800 clear, 801-804 clouds.
 */
weather_condition_t weather_condition_from_openweather(int code)
{
    if (code >= 200 && code <= 232) {
        return WEATHER_CONDITION_THUNDER;
    }
    if (code >= 300 && code <= 321) {
        return WEATHER_CONDITION_RAIN;
    }
    if (code >= 500 && code <= 531) {
        return WEATHER_CONDITION_RAIN;
    }
    if (code >= 600 && code <= 622) {
        return WEATHER_CONDITION_SNOW;
    }
    switch (code) {
    case 701: /** mist */
    case 741: /** fog */
        return WEATHER_CONDITION_FOG;
    case 711: /** smoke */
    case 721: /** haze */
    case 731: /** sand */
    case 751: /** dust */
    case 761: /** ash */
        return WEATHER_CONDITION_HAZE;
    case 762: /** volcanic ash */
        return WEATHER_CONDITION_HAZE;
    case 771: /** squall */
    case 781: /** tornado */
        return WEATHER_CONDITION_WIND;
    case 800:
        return WEATHER_CONDITION_CLEAR;
    case 801:
    case 802:
        return WEATHER_CONDITION_PARTLY_CLOUDY;
    case 803:
        return WEATHER_CONDITION_CLOUDY;
    case 804:
        return WEATHER_CONDITION_OVERCAST;
    default:
        return WEATHER_CONDITION_UNKNOWN;
    }
}

/**
 * @brief Seniverse's `now.code` is a flat 0-39 list with 99 meaning "none".
 * @param code Seniverse weather condition code.
 * @return Mapped weather condition.
 */
weather_condition_t weather_condition_from_seniverse(int code)
{
    switch (code) {
    case 0:
        return WEATHER_CONDITION_CLEAR;
    case 1:
        return WEATHER_CONDITION_PARTLY_CLOUDY;
    case 2:
        return WEATHER_CONDITION_OVERCAST;
    case 4:
        return WEATHER_CONDITION_THUNDER;
    case 5:  /** hail */
    case 6:  /** sleet */
    case 13: /** light snow */
    case 14:
    case 15:
    case 16:
    case 17:
    case 26:
    case 27:
    case 28:
        return WEATHER_CONDITION_SNOW;
    case 18: /** fog */
    case 32:
    case 33:
    case 34:
    case 35:
        return WEATHER_CONDITION_FOG;
    case 20: /** sandstorm */
    case 29: /** floating dust */
    case 30: /** blowing sand */
    case 31:
    case 36: /** haze */
    case 37:
    case 38:
    case 39:
        return WEATHER_CONDITION_HAZE;
    case 3:  /** shower */
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 19: /** freezing rain */
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
        return WEATHER_CONDITION_RAIN;
    default:
        return WEATHER_CONDITION_UNKNOWN;
    }
}

/**
 * @brief Keyword order carries the meaning: 雷 (thunder) before 雨 so 雷阵雨 is not read as rain, 雪 before 雨 so 雨夹雪 is snow, and 多云 before 阴.
 * @param text Provider weather description.
 * @return Mapped weather condition.
 */
weather_condition_t weather_condition_from_text(const char *text)
{
    static const struct {
        const char *token;
        weather_condition_t condition;
    } keywords[] = {
        {"雷", WEATHER_CONDITION_THUNDER}, /** 雷阵雨 */
        {"雪", WEATHER_CONDITION_SNOW},    /** 小雪 / 中雪 / 大雪 / 雨夹雪 */
        {"雨", WEATHER_CONDITION_RAIN},    /** 阵雨 / 小雨 / 大雨 / 冻雨 */
        {"雹", WEATHER_CONDITION_SNOW},    /** hail is ice */
        {"雾", WEATHER_CONDITION_FOG},
        {"霾", WEATHER_CONDITION_HAZE},
        {"沙", WEATHER_CONDITION_HAZE},    /** 扬沙 / 沙尘暴 */
        {"尘", WEATHER_CONDITION_HAZE},    /** 浮尘 */
        {"风", WEATHER_CONDITION_WIND},    /** 大风 / 台风 */
        {"多云", WEATHER_CONDITION_CLOUDY},
        {"阴", WEATHER_CONDITION_OVERCAST},
        {"晴", WEATHER_CONDITION_CLEAR},
        {"无", WEATHER_CONDITION_UNKNOWN},
    };

    if (text == NULL || text[0] == '\0') {
        return WEATHER_CONDITION_UNKNOWN;
    }
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i) {
        if (strstr(text, keywords[i].token) != NULL) {
            return keywords[i].condition;
        }
    }
    return WEATHER_CONDITION_UNKNOWN;
}

weather_wind_dir_t weather_wind_dir_from_degrees(int degrees)
{
    int bearing = degrees % 360;
    if (bearing < 0) {
        bearing += 360;
    }
    /** 45-degree sectors centred on each point: 337.5..22.5 is north. */
    return (weather_wind_dir_t)(((bearing + 22) / 45) % 8);
}

/**
 * @brief ASCII-only, locale-free prefix compare: the provider answers upper case.
 * @param text Text to inspect.
 * @param token Uppercase prefix to match.
 * @return true if the text begins with the token.
 */
static bool starts_with_upper(const char *text, const char *token)
{
    for (size_t index = 0; token[index] != '\0'; ++index) {
        char character = text[index];
        if (character >= 'a' && character <= 'z') {
            character = (char)(character - 'a' + 'A');
        }
        if (character != token[index]) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Seniverse answers the direction as text.
 * @param text Provider wind direction description.
 * @return Mapped wind direction.
 */
weather_wind_dir_t weather_wind_dir_from_text(const char *text)
{
    static const struct {
        const char *token;
        weather_wind_dir_t direction;
    } english[] = {
        /** Longest first: "SSE" must win over "S", and "SE" over "S". */
        {"NNE", WEATHER_WIND_NORTH_EAST}, {"ENE", WEATHER_WIND_NORTH_EAST},
        {"NNW", WEATHER_WIND_NORTH_WEST}, {"WNW", WEATHER_WIND_NORTH_WEST},
        {"ESE", WEATHER_WIND_SOUTH_EAST}, {"SE", WEATHER_WIND_SOUTH_EAST},
        {"SSE", WEATHER_WIND_SOUTH_EAST}, {"SSW", WEATHER_WIND_SOUTH_WEST},
        {"WSW", WEATHER_WIND_SOUTH_WEST},
        {"NE", WEATHER_WIND_NORTH_EAST},  {"NW", WEATHER_WIND_NORTH_WEST},
        {"SE", WEATHER_WIND_SOUTH_EAST},  {"SW", WEATHER_WIND_SOUTH_WEST},
        {"N", WEATHER_WIND_NORTH},        {"E", WEATHER_WIND_EAST},
        {"S", WEATHER_WIND_SOUTH},        {"W", WEATHER_WIND_WEST},
    };
    static const struct {
        const char *token;
        weather_wind_dir_t direction;
    } chinese[] = {
        {"东北", WEATHER_WIND_NORTH_EAST}, {"西北", WEATHER_WIND_NORTH_WEST},
        {"东南", WEATHER_WIND_SOUTH_EAST}, {"西南", WEATHER_WIND_SOUTH_WEST},
        {"北", WEATHER_WIND_NORTH},        {"东", WEATHER_WIND_EAST},
        {"南", WEATHER_WIND_SOUTH},        {"西", WEATHER_WIND_WEST},
    };

    if (text == NULL || text[0] == '\0') {
        return WEATHER_WIND_NORTH;
    }

    for (size_t i = 0; i < sizeof(english) / sizeof(english[0]); ++i) {
        if (starts_with_upper(text, english[i].token)) {
            return english[i].direction;
        }
    }
    for (size_t i = 0; i < sizeof(chinese) / sizeof(chinese[0]); ++i) {
        if (strstr(text, chinese[i].token) != NULL) {
            return chinese[i].direction;
        }
    }
    return WEATHER_WIND_NORTH;
}

int weather_wind_dmps_from_kmh(int kmh)
{
    if (kmh <= 0) {
        return 0;
    }
    /** km/h -> m/s is /3.6, and the status bar keeps one decimal: *25/9. */
    return (kmh * 25 + 4) / 9;
}

int weather_wind_dmps_from_mps_milli(int milli_mps)
{
    if (milli_mps <= 0) {
        return 0;
    }
    return (milli_mps + 50) / 100;
}

const char *weather_condition_name(weather_condition_t condition)
{
    switch (condition) {
    case WEATHER_CONDITION_CLEAR: return "clear";
    case WEATHER_CONDITION_PARTLY_CLOUDY: return "partly-cloudy";
    case WEATHER_CONDITION_CLOUDY: return "cloudy";
    case WEATHER_CONDITION_OVERCAST: return "overcast";
    case WEATHER_CONDITION_RAIN: return "rain";
    case WEATHER_CONDITION_THUNDER: return "thunder";
    case WEATHER_CONDITION_SNOW: return "snow";
    case WEATHER_CONDITION_FOG: return "fog";
    case WEATHER_CONDITION_HAZE: return "haze";
    case WEATHER_CONDITION_WIND: return "wind";
    default: return "unknown";
    }
}

const char *weather_wind_dir_name(weather_wind_dir_t direction)
{
    switch (direction) {
    case WEATHER_WIND_NORTH_EAST: return "ne";
    case WEATHER_WIND_EAST: return "e";
    case WEATHER_WIND_SOUTH_EAST: return "se";
    case WEATHER_WIND_SOUTH: return "s";
    case WEATHER_WIND_SOUTH_WEST: return "sw";
    case WEATHER_WIND_WEST: return "w";
    case WEATHER_WIND_NORTH_WEST: return "nw";
    default: return "n";
    }
}

/**
 * @brief Append `text` behind a single space; silently skipped when it cannot fit.
 * @param [in,out] out Destination text buffer.
 * @param capacity Destination buffer capacity in bytes.
 * @param [in,out] used Current text length, updated after appending.
 * @param text Field to append.
 */
static void append_field(char *out, size_t capacity, size_t *used, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    const size_t length = strlen(text);
    const size_t separator = *used > 0 ? 1u : 0u;
    if (*used + separator + length + 1u > capacity) {
        return;
    }
    if (separator != 0u) {
        out[(*used)++] = ' ';
    }
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = '\0';
}

/**
 * @brief One decimal of m/s from the stored deci-metres-per-second.
 * @param [in,out] out Destination text buffer.
 * @param capacity Destination buffer capacity in bytes.
 * @param [in,out] used Current text length, updated after appending.
 * @param wind_dmps Wind speed in tenths of a metre per second.
 */
static void append_wind_speed(char *out, size_t capacity, size_t *used, int wind_dmps)
{
    char text[16];
    const int magnitude = wind_dmps < 0 ? -wind_dmps : wind_dmps;
    snprintf(text, sizeof(text), "%s%d.%dm/s", wind_dmps < 0 ? "-" : "",
             magnitude / 10, magnitude % 10);
    append_field(out, capacity, used, text);
}

static size_t display_reset(char *out, size_t capacity)
{
    if (out == NULL || capacity == 0u) {
        return 0u;
    }
    out[0] = '\0';
    return capacity;
}

size_t weather_display_place_format(char *out, size_t capacity, uint8_t fields,
                                    const weather_display_t *display)
{
    if (display_reset(out, capacity) == 0u || display == NULL ||
        (fields & WEATHER_FIELD_PLACE) == 0u) {
        return 0u;
    }
    size_t used = 0u;
    append_field(out, capacity, &used, display->place);
    return used;
}

size_t weather_display_text_format(char *out, size_t capacity, uint8_t fields,
                                   const weather_display_t *display)
{
    if (display_reset(out, capacity) == 0u || display == NULL) {
        return 0u;
    }
    char text[16];
    size_t used = 0u;

    if ((fields & WEATHER_FIELD_CONDITION_TEXT) != 0u) {
        append_field(out, capacity, &used, display->condition);
    }
    if ((fields & WEATHER_FIELD_TEMPERATURE) != 0u) {
        snprintf(text, sizeof(text), "%d°C", display->temp_c);
        append_field(out, capacity, &used, text);
    }
    if ((fields & WEATHER_FIELD_HUMIDITY) != 0u) {
        /** A provider that does not report humidity leaves the field out rather than claiming 0%. */
        if (display->humidity >= 0) {
            snprintf(text, sizeof(text), "%d%%", display->humidity);
            append_field(out, capacity, &used, text);
        }
    }
    return used;
}

size_t weather_display_wind_format(char *out, size_t capacity, uint8_t fields,
                                   const weather_display_t *display)
{
    if (display_reset(out, capacity) == 0u || display == NULL) {
        return 0u;
    }
    size_t used = 0u;

    if ((fields & WEATHER_FIELD_WIND_TEXT) != 0u) {
        append_field(out, capacity, &used, display->wind_direction);
    }
    if ((fields & WEATHER_FIELD_WIND_SPEED) != 0u) {
        if (display->wind_dmps >= 0) {
            append_wind_speed(out, capacity, &used, display->wind_dmps);
        }
    }
    return used;
}
