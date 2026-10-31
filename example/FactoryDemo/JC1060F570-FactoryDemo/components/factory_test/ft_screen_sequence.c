/** SPDX-License-Identifier: GPL-2.0-only */
#include "ft_screen_sequence.h"

#include <string.h>

/** Screen color order: red, green, blue, white, black. */
static const uint32_t s_color_hex[FT_SCREEN_COLOR_COUNT] = {
    0xFF0000U,
    0x00FF00U,
    0x0000FFU,
    0xFFFFFFU,
    0x000000U,
};

uint32_t ft_screen_color_hex(uint8_t index)
{
    if (index >= FT_SCREEN_COLOR_COUNT) {
        return 0x000000U;
    }
    return s_color_hex[index];
}

bool ft_screen_duration_parse(const char *text, uint8_t *seconds)
{
    if (text == NULL || seconds == NULL) {
        return false;
    }

    const size_t length = strlen(text);
    /** Accept at most two decimal digits for the 1..15 second range. */
    if (length == 0U || length > 2U) {
        return false;
    }

    uint32_t value = 0U;
    for (size_t index = 0U; index < length; ++index) {
        const char digit = text[index];
        if (digit < '0' || digit > '9') {
            return false;
        }
        value = value * 10U + (uint32_t)(digit - '0');
    }

    if (value < FT_SCREEN_DURATION_MIN_SECONDS || value > FT_SCREEN_DURATION_MAX_SECONDS) {
        return false;
    }

    *seconds = (uint8_t)value;
    return true;
}

uint32_t ft_screen_duration_ms(uint8_t seconds)
{
    return (uint32_t)seconds * 1000U;
}

bool ft_screen_sequence_advance(uint8_t *index)
{
    if (index == NULL || (uint16_t)*index + 1U >= FT_SCREEN_COLOR_COUNT) {
        return false;
    }
    *index = (uint8_t)(*index + 1U);
    return true;
}
