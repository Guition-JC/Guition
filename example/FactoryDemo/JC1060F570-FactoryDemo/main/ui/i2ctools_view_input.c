#include "i2ctools_view_input.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void keyboard_hide(lv_obj_t *keyboard) {
  if (keyboard != NULL) {
    lv_keyboard_set_textarea(keyboard, NULL);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
  }
}

static lv_obj_t *keyboard_for_field(const i2ctools_view_input_t *input,
                                    const lv_obj_t *field) {
  for (size_t index = 0; index < input->field_count; ++index) {
    if (input->fields[index] == field) {
      return input->field_keyboards[index];
    }
  }
  return NULL;
}

static bool field_is_hex(const i2ctools_view_input_t *input,
                         const lv_obj_t *field) {
  for (size_t index = 0; index < input->field_count; ++index) {
    if (input->fields[index] == field) {
      return input->field_hex[index];
    }
  }
  return false;
}

/**
 * @brief Transfer focus to a field and show its configured keyboard.
 * @param input Keyboard binding state.
 * @param field Field receiving focus.
 * @param keyboard Keyboard to show.
 */
static void keyboard_show(i2ctools_view_input_t *input, lv_obj_t *field,
                          lv_obj_t *keyboard) {
  if (input == NULL || field == NULL || keyboard == NULL) {
    return;
  }
  if (input->active_keyboard != NULL && input->active_keyboard != keyboard) {
    keyboard_hide(input->active_keyboard);
  }
  lv_keyboard_set_mode(keyboard,
                        field == input->write_data || field_is_hex(input, field)
                            ? LV_KEYBOARD_MODE_TEXT_LOWER
                            : LV_KEYBOARD_MODE_NUMBER);
  lv_keyboard_set_textarea(keyboard, field);
  lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(keyboard);
  input->active_keyboard = keyboard;
  input->active_field = field;
  lv_obj_update_layout(lv_obj_get_parent(keyboard));
  lv_obj_scroll_to_view_recursive(keyboard, LV_ANIM_OFF);
  if (input->close_button != NULL) {
    lv_obj_move_foreground(input->close_button);
  }
}

static void keyboard_event_cb(lv_event_t *event) {
  i2ctools_view_input_t *input = lv_event_get_user_data(event);
  lv_obj_t *keyboard = lv_event_get_current_target_obj(event);
  const lv_event_code_t code = lv_event_get_code(event);
  if (input == NULL || (code != LV_EVENT_READY && code != LV_EVENT_CANCEL)) {
    return;
  }
  keyboard_hide(keyboard);
  if (input->active_keyboard == keyboard) {
    input->active_keyboard = NULL;
    input->active_field = NULL;
  }
}

static void field_event_cb(lv_event_t *event) {
  i2ctools_view_input_t *input = lv_event_get_user_data(event);
  lv_obj_t *field = lv_event_get_target_obj(event);
  if (input == NULL || field == NULL) {
    return;
  }
  keyboard_show(input, field, keyboard_for_field(input, field));
}

static lv_obj_t *create_card_keyboard(lv_obj_t *card,
                                      i2ctools_view_input_t *input) {
  lv_obj_t *keyboard = lv_keyboard_create(card);
  lv_obj_set_width(keyboard, LV_PCT(100));
  lv_obj_set_height(keyboard, LV_PCT(40));
  lv_obj_add_flag(keyboard, LV_OBJ_FLAG_FLOATING);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_READY, input);
  lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_CANCEL, input);
  return keyboard;
}

void i2ctools_view_input_init(i2ctools_view_input_t *input,
                              lv_obj_t *config_card, lv_obj_t *operation_card,
                              lv_obj_t *write_data, lv_obj_t *close_button) {
  memset(input, 0, sizeof(*input));
  input->write_data = write_data;
  input->close_button = close_button;
  input->config_keyboard = create_card_keyboard(config_card, input);
  input->operation_keyboard = create_card_keyboard(operation_card, input);
  lv_obj_add_flag(input->config_keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(input->operation_keyboard, LV_OBJ_FLAG_HIDDEN);
}

void i2ctools_view_input_bind(i2ctools_view_input_t *input, lv_obj_t *field,
                              lv_obj_t *keyboard, bool hex_input) {
  if (input->field_count >= sizeof(input->fields) / sizeof(input->fields[0])) {
    return;
  }
  input->fields[input->field_count] = field;
  input->field_keyboards[input->field_count] = keyboard;
  input->field_hex[input->field_count] = hex_input;
  input->field_count++;
  lv_obj_add_flag(field, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
  lv_obj_add_event_cb(field, field_event_cb, LV_EVENT_PRESSED, input);
  lv_obj_add_event_cb(field, field_event_cb, LV_EVENT_FOCUSED, input);
}

/**
 * @brief Hide and delete keyboards before clearing page input state.
 * @param input Input state whose keyboards and bindings are cleared.
 */
void i2ctools_view_input_cleanup(i2ctools_view_input_t *input) {
  keyboard_hide(input->config_keyboard);
  keyboard_hide(input->operation_keyboard);
  if (input->config_keyboard != NULL) {
    lv_obj_delete(input->config_keyboard);
  }
  if (input->operation_keyboard != NULL) {
    lv_obj_delete(input->operation_keyboard);
  }
  input->config_keyboard = NULL;
  input->operation_keyboard = NULL;
  input->active_keyboard = NULL;
  input->active_field = NULL;
  input->write_data = NULL;
  input->close_button = NULL;
  input->field_count = 0;
}

bool i2ctools_view_input_read_decimal(const lv_obj_t *field, int32_t minimum,
                                      int32_t maximum, int32_t *value) {
  const char *text = lv_textarea_get_text(field);
  if (text == NULL || text[0] == '\0') {
    return false;
  }
  char *end = NULL;
  const long parsed = strtol(text, &end, 10);
  if (end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
    return false;
  }
  *value = (int32_t)parsed;
  return true;
}

bool i2ctools_view_input_read_hex(const lv_obj_t *field, uint32_t minimum,
                                  uint32_t maximum, uint32_t *value) {
  const char *text = lv_textarea_get_text(field);
  if (text == NULL) {
    return false;
  }
  while (isspace((unsigned char)*text)) {
    ++text;
  }
  if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text += 2;
  }
  if (!isxdigit((unsigned char)text[0]) ||
      !isxdigit((unsigned char)text[1])) {
    return false;
  }
  const unsigned int high = (unsigned int)(isdigit((unsigned char)text[0])
                                                ? text[0] - '0'
                                                : tolower((unsigned char)text[0]) - 'a' + 10);
  const unsigned int low = (unsigned int)(isdigit((unsigned char)text[1])
                                               ? text[1] - '0'
                                               : tolower((unsigned char)text[1]) - 'a' + 10);
  text += 2;
  while (isspace((unsigned char)*text)) {
    ++text;
  }
  if (*text != '\0') {
    return false;
  }
  const uint32_t parsed = (high << 4) | low;
  if (parsed < minimum || parsed > maximum) {
    return false;
  }
  *value = parsed;
  return true;
}

/**
 * @brief Decode separated two-digit byte tokens into the caller buffer.
 * @param text Text containing byte tokens.
 * @param [out] data Buffer receiving bytes.
 * @param capacity Buffer capacity in bytes.
 * @param [out] length Receives the byte count on success.
 * @return true when all tokens are valid and fit in the buffer.
 */
bool i2ctools_view_input_parse_hex(const char *text, uint8_t *data,
                                   size_t capacity, size_t *length) {
  const char *cursor = text;
  size_t count = 0;
  while (*cursor != '\0') {
    while (isspace((unsigned char)*cursor) || *cursor == ',' ||
           *cursor == ';') {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }
    if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
      cursor += 2;
    }
    if (!isxdigit((unsigned char)cursor[0]) ||
        !isxdigit((unsigned char)cursor[1])) {
      return false;
    }
    char byte_text[3] = {cursor[0], cursor[1], '\0'};
    char *end = NULL;
    const unsigned long value = strtoul(byte_text, &end, 16);
    if (end != byte_text + 2 || value > UINT8_MAX || count >= capacity) {
      return false;
    }
    data[count++] = (uint8_t)value;
    cursor += 2;
    if (*cursor != '\0' && !isspace((unsigned char)*cursor) &&
        *cursor != ',' && *cursor != ';') {
      return false;
    }
  }
  *length = count;
  return true;
}
