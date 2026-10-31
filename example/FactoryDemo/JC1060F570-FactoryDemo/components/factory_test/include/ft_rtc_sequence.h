/** SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t second;
    uint8_t transitions;
    bool initialized;
} ft_rtc_sequence_t;

/* Returns false for invalid BCD or a non-consecutive change. */
static inline bool ft_rtc_sequence_sample(ft_rtc_sequence_t *sequence, uint8_t raw)
{
    raw &= 0x7fU;
    if ((raw & 0x0fU) > 9U || (raw >> 4U) > 5U) {
        return false;
    }
    const uint8_t second = (uint8_t)((raw >> 4U) * 10U + (raw & 0x0fU));
    if (sequence->initialized && second != sequence->second) {
        if (second != (uint8_t)((sequence->second + 1U) % 60U)) {
            return false;
        }
        sequence->transitions++;
    }
    sequence->second = second;
    sequence->initialized = true;
    return true;
}
