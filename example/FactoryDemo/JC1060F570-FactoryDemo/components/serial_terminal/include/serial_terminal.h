#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define SERIAL_TERMINAL_HISTORY_LIMIT 100
#define SERIAL_TERMINAL_HISTORY_TEXT_SIZE (SERIAL_TERMINAL_HISTORY_LIMIT * 520 + 1)

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Direction of a recorded UART message. */
typedef enum {
    SERIAL_TERMINAL_DIRECTION_RX, /**< Received message. */
    SERIAL_TERMINAL_DIRECTION_TX, /**< Transmitted message. */
} serial_terminal_direction_t;

/**
 * @brief Initialize the configured UART and start receiving terminal messages.
 * @return ESP_OK when ready, or an error from UART or task initialization.
 */
esp_err_t serial_terminal_init(void);

/** @brief Return whether the UART driver and RX task are ready.
 * @return True when the terminal is ready.
 */
bool serial_terminal_is_ready(void);

/**
 * @brief Send one line and record it in the terminal history on success.
 * @param text Null-terminated message; CRLF is appended if it has no line ending.
 * @return ESP_OK on success, or an error if the terminal is unavailable or sending fails.
 */
esp_err_t serial_terminal_send(const char *text);

/**
 * @brief Remove all retained messages.
 */
void serial_terminal_history_clear(void);

/**
 * @brief Return a monotonically increasing history revision for UI refresh checks.
 * @return A monotonically increasing history revision for UI refresh checks.
 */
unsigned int serial_terminal_history_revision(void);

/**
 * @brief Format the retained history into a caller-provided text buffer.
 * @param[out] buffer Destination for the null-terminated text.
 * @param buffer_size Destination capacity in bytes, including the terminator.
 * @return Number of bytes written, excluding the terminator, or zero if unavailable.
 */
size_t serial_terminal_history_format(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif
