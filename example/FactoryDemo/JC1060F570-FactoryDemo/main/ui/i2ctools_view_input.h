#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Page-owned keyboard bindings and current input focus. */
typedef struct {
  lv_obj_t *config_keyboard;
  lv_obj_t *operation_keyboard;
  lv_obj_t *active_keyboard;
  lv_obj_t *active_field;
  lv_obj_t *write_data;
  lv_obj_t *field_keyboards[12];
  bool field_hex[12];
  lv_obj_t *close_button;
  lv_obj_t *fields[12];
  size_t field_count;
} i2ctools_view_input_t;

/**
 * @brief Create hidden keyboards for configuration and operation cards.
 * @param input Input state to initialize.
 * @param config_card Configuration card that owns its keyboard.
 * @param operation_card Operation card that owns its keyboard.
 * @param write_data Write-data field.
 * @param close_button Close button to coordinate with keyboard visibility.
 */
void i2ctools_view_input_init(i2ctools_view_input_t *input,
                              lv_obj_t *config_card, lv_obj_t *operation_card,
                              lv_obj_t *write_data, lv_obj_t *close_button);
/**
 * @brief Bind a field to its keyboard and decimal or hexadecimal input mode.
 * @param input Input state.
 * @param field Field to bind.
 * @param keyboard Keyboard to show for the field.
 * @param hex_input Whether the field accepts hexadecimal input.
 */
void i2ctools_view_input_bind(i2ctools_view_input_t *input, lv_obj_t *field,
                              lv_obj_t *keyboard, bool hex_input);
/**
 * @brief Delete page keyboards and clear input bindings.
 * @param input Input state whose keyboards and bindings are cleared.
 */
void i2ctools_view_input_cleanup(i2ctools_view_input_t *input);
/**
 * @brief Parse a decimal field within inclusive bounds.
 * @param field Text field to parse.
 * @param minimum Smallest accepted value.
 * @param maximum Largest accepted value.
 * @param[out] value Receives the parsed value on success.
 * @return true when the complete field is valid and value has been written.
 */
bool i2ctools_view_input_read_decimal(const lv_obj_t *field, int32_t minimum,
                                      int32_t maximum, int32_t *value);
/**
 * @brief Parse a two-digit hexadecimal field with optional 0x prefix and surrounding whitespace.
 * @param field Text field to parse.
 * @param minimum Smallest accepted value.
 * @param maximum Largest accepted value.
 * @param[out] value Receives the parsed value on success.
 * @return true when the byte lies within the inclusive bounds.
 */
bool i2ctools_view_input_read_hex(const lv_obj_t *field, uint32_t minimum,
                                  uint32_t maximum, uint32_t *value);
/**
 * @brief Parse hexadecimal bytes separated by whitespace, commas or semicolons.
 * @param text Input text to parse.
 * @param[out] data Destination buffer for parsed bytes.
 * @param capacity Maximum number of bytes accepted by data.
 * @param length Receives the byte count on success.
 * @return false on malformed input or insufficient capacity; data may be partially written.
 */
bool i2ctools_view_input_parse_hex(const char *text, uint8_t *data,
                                   size_t capacity, size_t *length);

#ifdef __cplusplus
}
#endif
