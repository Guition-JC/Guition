#include "i2ctools_view.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"

#include "esp_board_manager.h"
#include "i2ctools_app.h"
#include "i2ctools_view_input.h"
#include "i2ctools_view_internal.h"
#include "i2ctools_view_widgets.h"
#include "desktop_font.h"
#include "desktop_language.h"
#include "soc/soc_caps.h"

#define L(key) desktop_language_lookup(key)
#define C_BG lv_color_hex(0xDCE8F5)
#define C_CARD lv_color_hex(0xF7FAFD)
#define C_INK lv_color_hex(0x122033)
#define C_MUTED lv_color_hex(0x53667B)
#define C_BUTTON lv_color_hex(0x124E78)
#define C_BUTTON_TEXT lv_color_hex(0xFFFFFF)
#define C_ACCENT lv_color_hex(0x0B4F6C)
#define C_SUCCESS lv_color_hex(0x197A52)
#define C_ERROR lv_color_hex(0xA62C2C)
#define TAG "I2CToolsUI"
#define I2CTOOLS_VIEW_MAX_COMM_LINES 30U
#define I2CTOOLS_VIEW_COMM_LINE_MAX 160U
#define I2CTOOLS_VIEW_COMM_TEXT_MAX \
    (I2CTOOLS_VIEW_MAX_COMM_LINES * I2CTOOLS_VIEW_COMM_LINE_MAX)

i2ctools_app_t *s_app;
i2ctools_view_input_t s_input;
lv_obj_t *s_i2c_num;
lv_obj_t *s_sda;
lv_obj_t *s_scl;
lv_obj_t *s_freq;
lv_obj_t *s_address;
lv_obj_t *s_register;
lv_obj_t *s_write_data;
lv_obj_t *s_length;
lv_obj_t *s_chunk;
lv_obj_t *s_status;
lv_obj_t *s_output;
bool s_configured;
i2c_master_bus_config_t s_board_bus_config;
i2c_master_bus_handle_t s_board_bus;
bool s_board_bus_available;

/** @brief LVGL-owned communication log storage, approximately 9.6 KB; not accessed by DMA or ISRs. */
EXT_RAM_BSS_ATTR static char s_comm_lines[I2CTOOLS_VIEW_MAX_COMM_LINES]
                                        [I2CTOOLS_VIEW_COMM_LINE_MAX];
static size_t s_comm_line_count;
EXT_RAM_BSS_ATTR static char s_comm_text[I2CTOOLS_VIEW_COMM_TEXT_MAX + 1U];

static void communication_refresh(void) {
  size_t used = 0;
  s_comm_text[0] = '\0';
  for (size_t index = 0; index < s_comm_line_count; ++index) {
    const size_t line_length = strlen(s_comm_lines[index]);
    if (used != 0U && used + 1U < sizeof(s_comm_text)) {
      s_comm_text[used++] = '\n';
    }
    if (line_length >= sizeof(s_comm_text) - used) {
      break;
    }
    memcpy(s_comm_text + used, s_comm_lines[index], line_length);
    used += line_length;
  }
  s_comm_text[used] = '\0';
  if (s_output == NULL) {
    return;
  }
  lv_label_set_text(s_output, s_comm_text);
  lv_obj_t *output_card = lv_obj_get_parent(s_output);
  lv_obj_update_layout(output_card);
  lv_obj_scroll_to_y(output_card, LV_COORD_MAX, LV_ANIM_OFF);
}

static void communication_clear(void) {
  s_comm_line_count = 0;
  s_comm_text[0] = '\0';
  if (s_output != NULL) {
    lv_label_set_text(s_output, "");
  }
}

static void communication_append_line(const char *line, size_t length) {
  if (line == NULL || length == 0U) {
    return;
  }
  if (s_comm_line_count == I2CTOOLS_VIEW_MAX_COMM_LINES) {
    memmove(s_comm_lines[0], s_comm_lines[1],
            (I2CTOOLS_VIEW_MAX_COMM_LINES - 1U) *
                sizeof(s_comm_lines[0]));
    s_comm_line_count--;
  }
  const size_t copy_length = length < I2CTOOLS_VIEW_COMM_LINE_MAX - 1U
                                 ? length
                                 : I2CTOOLS_VIEW_COMM_LINE_MAX - 1U;
  memcpy(s_comm_lines[s_comm_line_count], line, copy_length);
  s_comm_lines[s_comm_line_count][copy_length] = '\0';
  s_comm_line_count++;
}

static void communication_append(const char *text) {
  if (text == NULL || *text == '\0') {
    return;
  }
  const char *line_start = text;
  while (*line_start != '\0') {
    const char *line_end = strchr(line_start, '\n');
    const size_t line_length = line_end == NULL
                                   ? strlen(line_start)
                                   : (size_t)(line_end - line_start);
    communication_append_line(line_start, line_length);
    if (line_end == NULL) {
      break;
    }
    line_start = line_end + 1;
  }
  communication_refresh();
}

void i2ctools_view_status_text(const char *text, lv_color_t color) {
  if (s_status == NULL) {
    return;
  }
  lv_label_set_text(s_status, text);
  lv_obj_set_style_text_color(s_status, color, 0);
}

static bool read_config(i2ctools_app_config_t *config) {
  int32_t value;
  if (!i2ctools_view_input_read_decimal(s_i2c_num, 0, SOC_HP_I2C_NUM - 1,
                                        &value)) {
    return false;
  }
  config->i2c_port = (i2c_port_num_t)value;
  if (!i2ctools_view_input_read_decimal(s_sda, 0, 61, &value)) {
    return false;
  }
  config->sda_io_num = (gpio_num_t)value;
  if (!i2ctools_view_input_read_decimal(s_scl, 0, 61, &value)) {
    return false;
  }
  config->scl_io_num = (gpio_num_t)value;
  if (!i2ctools_view_input_read_decimal(s_freq, 10, 1000, &value)) {
    return false;
  }
  config->frequency_hz = (uint32_t)value * 1000U;
  return true;
}

static bool read_operation(uint8_t *address, uint8_t *register_address,
                           size_t *length, uint8_t *chunk_size) {
  uint32_t hex_value;
  int32_t decimal_value;
  if (!i2ctools_view_input_read_hex(s_address, 0, 127, &hex_value)) {
    ESP_LOGW(TAG, "invalid address text=\"%s\"",
             lv_textarea_get_text(s_address));
    return false;
  }
  *address = (uint8_t)hex_value;
  if (!i2ctools_view_input_read_hex(s_register, 0, 255, &hex_value)) {
    ESP_LOGW(TAG, "invalid register text=\"%s\"",
             lv_textarea_get_text(s_register));
    return false;
  }
  *register_address = (uint8_t)hex_value;
  if (!i2ctools_view_input_read_decimal(s_length, 1, 32, &decimal_value)) {
    return false;
  }
  *length = (size_t)decimal_value;
  if (!i2ctools_view_input_read_decimal(s_chunk, 1, 4, &decimal_value) ||
      (decimal_value != 1 && decimal_value != 2 && decimal_value != 4)) {
    return false;
  }
  *chunk_size = (uint8_t)decimal_value;
  return true;
}

static bool app_ready(void) {
  if (s_app == NULL) {
    i2ctools_view_status_text(L(DESKTOP_TEXT_I2CTOOLS_UNAVAILABLE), C_ERROR);
    return false;
  }
  if (!s_configured) {
    i2ctools_view_status_text(L(DESKTOP_TEXT_I2CTOOLS_BUS), C_ERROR);
    return false;
  }
  return true;
}

void i2ctools_view_configure_cb(lv_event_t *event) {
  (void)event;
  i2ctools_app_config_t config;
  i2ctools_app_config_init(&config);
  if (!read_config(&config)) {
    i2ctools_view_status_text(L(DESKTOP_TEXT_I2CTOOLS_INVALID_INPUT), C_ERROR);
    return;
  }
  const bool use_board_bus =
      s_board_bus_available && s_board_bus != NULL &&
      config.i2c_port == s_board_bus_config.i2c_port &&
      config.sda_io_num == s_board_bus_config.sda_io_num &&
      config.scl_io_num == s_board_bus_config.scl_io_num;
  const esp_err_t result =
      use_board_bus ? i2ctools_app_attach_bus(s_app, &config, s_board_bus)
                    : i2ctools_app_configure(s_app, &config);
  if (result == ESP_OK) {
    s_configured = true;
    i2ctools_view_status_text(L(DESKTOP_TEXT_I2CTOOLS_READY), C_SUCCESS);
  } else {
    i2ctools_view_status_text(i2ctools_app_error_name(result), C_ERROR);
  }
}

void i2ctools_view_scan_cb(lv_event_t *event) {
  (void)event;
  if (!app_ready()) {
    return;
  }
  i2ctools_app_scan_result_t result;
  i2ctools_view_status_text(L(DESKTOP_TEXT_I2CTOOLS_SCANNING), C_MUTED);
  const esp_err_t error = i2ctools_app_scan(s_app, &result);
  char output[1024];
  size_t used = (size_t)snprintf(output, sizeof(output),
                                 L(DESKTOP_TEXT_I2CTOOLS_FOUND_COUNT),
                                 (unsigned)result.count);
  for (size_t i = 0; i < result.count && used < sizeof(output); ++i) {
    used += (size_t)snprintf(output + used, sizeof(output) - used, "0x%02X%s",
                             result.addresses[i],
                             (i + 1U) % 8U == 0U ? "\n" : "  ");
  }
  communication_append(output);
  i2ctools_view_status_text(error == ESP_OK
                                ? L(DESKTOP_TEXT_I2CTOOLS_DEVICE_FOUND)
                                : i2ctools_app_error_name(error),
                            error == ESP_OK ? C_SUCCESS : C_ERROR);
}

void i2ctools_view_read_cb(lv_event_t *event) {
  (void)event;
  if (!app_ready()) {
    return;
  }
  uint8_t address;
  uint8_t register_address;
  uint8_t chunk_size;
  size_t length;
  if (!read_operation(&address, &register_address, &length, &chunk_size)) {
    i2ctools_view_status_text(L(DESKTOP_TEXT_I2CTOOLS_INVALID_INPUT), C_ERROR);
    return;
  }
  (void)chunk_size;
  uint8_t data[32];
  const esp_err_t error = i2ctools_app_read_registers(
      s_app, address, register_address, data, length);
  if (error != ESP_OK) {
    i2ctools_view_status_text(i2ctools_app_error_name(error), C_ERROR);
    return;
  }
  char output[160] = {0};
  size_t used = 0;
  for (size_t i = 0; i < length && used < sizeof(output); ++i) {
    used += (size_t)snprintf(output + used, sizeof(output) - used, "%02X%s",
                             data[i], i + 1U == length ? "" : " ");
  }
  communication_append(output);
  i2ctools_view_status_text(L(DESKTOP_TEXT_I2CTOOLS_SUCCESS), C_SUCCESS);
}

void i2ctools_view_write_cb(lv_event_t *event) {
  (void)event;
  if (!app_ready()) {
    return;
  }
  uint8_t address;
  uint8_t register_address;
  uint8_t chunk_size;
  size_t length;
  if (!read_operation(&address, &register_address, &length, &chunk_size)) {
    i2ctools_view_status_text(L(DESKTOP_TEXT_I2CTOOLS_INVALID_INPUT), C_ERROR);
    return;
  }
  (void)length;
  (void)chunk_size;
  uint8_t data[I2CTOOLS_APP_MAX_WRITE_DATA];
  size_t data_length;
  const char *write_text = lv_textarea_get_text(s_write_data);
  if (!i2ctools_view_input_parse_hex(write_text, data, sizeof(data),
                                     &data_length) ||
      data_length == 0U) {
    ESP_LOGW(TAG, "invalid write data text=\"%s\"", write_text);
    i2ctools_view_status_text(L(DESKTOP_TEXT_I2CTOOLS_INVALID_INPUT), C_ERROR);
    return;
  }
  const esp_err_t error = i2ctools_app_write_registers(
      s_app, address, register_address, data, data_length);
  i2ctools_view_status_text(error == ESP_OK ? L(DESKTOP_TEXT_I2CTOOLS_SUCCESS)
                                            : i2ctools_app_error_name(error),
                            error == ESP_OK ? C_SUCCESS : C_ERROR);
}

void i2ctools_view_dump_cb(lv_event_t *event) {
  (void)event;
  if (!app_ready()) {
    return;
  }
  uint8_t address;
  uint8_t register_address;
  uint8_t chunk_size;
  size_t length;
  if (!read_operation(&address, &register_address, &length, &chunk_size)) {
    i2ctools_view_status_text(L(DESKTOP_TEXT_I2CTOOLS_INVALID_INPUT), C_ERROR);
    return;
  }
  (void)register_address;
  (void)length;
  i2ctools_app_dump_result_t result;
  const esp_err_t error =
      i2ctools_app_dump_registers(s_app, address, chunk_size, &result);
  char output[1400] = {0};
  size_t used = 0;
  for (size_t offset = 0;
       offset < I2CTOOLS_APP_DUMP_SIZE && used + 4U < sizeof(output);
       offset += 16U) {
    used += (size_t)snprintf(output + used, sizeof(output) - used,
                             "%02X: ", (unsigned)offset);
    for (size_t index = 0; index < 16U; ++index) {
      const size_t pos = offset + index;
      used += (size_t)snprintf(output + used, sizeof(output) - used, "%s%02X",
                               result.valid[pos] ? "" : "XX", result.data[pos]);
      if (index != 15U) {
        used += (size_t)snprintf(output + used, sizeof(output) - used, " ");
      }
    }
    used += (size_t)snprintf(output + used, sizeof(output) - used, "\n");
  }
  communication_append(output);
  i2ctools_view_status_text(error == ESP_OK ? L(DESKTOP_TEXT_I2CTOOLS_SUCCESS)
                                            : i2ctools_app_error_name(error),
                            error == ESP_OK ? C_SUCCESS : C_ERROR);
}

void i2ctools_view_clear_cb(lv_event_t *event) {
  (void)event;
  communication_clear();
  i2ctools_view_status_text(L(DESKTOP_TEXT_I2CTOOLS_READY), C_SUCCESS);
}

void i2ctools_view_cleanup(void) {
  i2ctools_view_input_cleanup(&s_input);
  if (s_app != NULL) {
    (void)i2ctools_app_destroy(s_app);
    s_app = NULL;
  }
  s_i2c_num = NULL;
  s_sda = NULL;
  s_scl = NULL;
  s_freq = NULL;
  s_address = NULL;
  s_register = NULL;
  s_write_data = NULL;
  s_length = NULL;
  s_chunk = NULL;
  s_status = NULL;
  s_output = NULL;
  s_configured = false;
  communication_clear();
}
