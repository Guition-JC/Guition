/** SPDX-License-Identifier: GPL-2.0-only */
/** Screen diagnostic color sequence and dwell-time conversion. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FT_SCREEN_COLOR_COUNT 5U
#define FT_SCREEN_DURATION_MIN_SECONDS 1U
#define FT_SCREEN_DURATION_MAX_SECONDS 15U
#define FT_SCREEN_DURATION_DEFAULT_SECONDS 1U

/**
 * @brief 0xRRGGBB for a sequence index; out-of-range indexes resolve to black.
 * @param index Index of the item.
 * @return 0xRRGGBB for a sequence index; out-of-range indexes resolve to black.
 */
uint32_t ft_screen_color_hex(uint8_t index);

/**
 * @brief Parse one or two decimal digits in the 1..15 second range; leading zeros are accepted.
 * @param text Text to use.
 * @param[out] seconds Receives the parsed duration in seconds.
 * @return true if the text is a valid duration in the 1–15 second range.
 */
bool ft_screen_duration_parse(const char *text, uint8_t *seconds);

/**
 * @brief Dwell time in milliseconds for a value already accepted by the parser.
 * @param seconds Time in seconds.
 * @return Dwell time in milliseconds for a value already accepted by the parser.
 */
uint32_t ft_screen_duration_ms(uint8_t seconds);

/**
 * @brief Advance to the next colour; false when the last colour was already shown.
 * @param index Index of the item.
 * @return true if the sequence advanced; false if the last colour was already shown.
 */
bool ft_screen_sequence_advance(uint8_t *index);

#ifdef __cplusplus
}
#endif
