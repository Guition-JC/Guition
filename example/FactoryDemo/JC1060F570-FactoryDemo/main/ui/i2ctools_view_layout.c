#include "i2ctools_view.h"

#include <stdio.h>
#include <string.h>

#include "esp_board_manager.h"
#include "i2ctools_view_internal.h"
#include "i2ctools_view_widgets.h"
#include "desktop_font.h"
#include "desktop_fullscreen_layout.h"
#include "desktop_language.h"
#include "soc/soc_caps.h"

#define L(key) desktop_language_lookup(key)
#define C_BG lv_color_hex(0xDCE8F5)
#define C_CARD lv_color_hex(0xF7FAFD)
#define C_INK lv_color_hex(0x122033)
#define C_MUTED lv_color_hex(0x53667B)
#define C_SUCCESS lv_color_hex(0x197A52)
#define C_ERROR lv_color_hex(0xA62C2C)

static lv_obj_t *hex_field(lv_obj_t *parent, const char *text,
                           int width_percent, int height_percent,
                           const char *initial) {
  lv_obj_t *field = i2ctools_view_flex_text_field(
      parent, text, width_percent, height_percent, NULL);
  lv_textarea_set_text(field, initial);
  lv_textarea_set_accepted_chars(field, "0123456789abcdefABCDEFxX");
  lv_textarea_set_max_length(field, 4);
  return field;
}

static lv_obj_t *decimal_field(lv_obj_t *parent, const char *text,
                               int width_percent, int height_percent,
                               int32_t value) {
  char initial[16];
  snprintf(initial, sizeof(initial), "%ld", (long)value);
  lv_obj_t *field = i2ctools_view_flex_text_field(
      parent, text, width_percent, height_percent, NULL);
  lv_textarea_set_text(field, initial);
  lv_textarea_set_accepted_chars(field, "0123456789");
  lv_textarea_set_max_length(field, 5);
  return field;
}

void i2ctools_view_build(lv_obj_t *parent, lv_obj_t *full_screen,
                         lv_obj_t *close_button) {
  i2ctools_view_cleanup();
  s_board_bus = NULL;
  s_board_bus_available = false;
  i2ctools_app_config_t defaults;
  i2ctools_app_config_init(&defaults);
  memset(&s_board_bus_config, 0, sizeof(s_board_bus_config));
  void *raw_board_config = NULL;
  if (esp_board_manager_get_periph_config("i2c_master", &raw_board_config) ==
          ESP_OK &&
      raw_board_config != NULL) {
    s_board_bus_config = *(const i2c_master_bus_config_t *)raw_board_config;
    defaults.i2c_port = s_board_bus_config.i2c_port;
    defaults.sda_io_num = s_board_bus_config.sda_io_num;
    defaults.scl_io_num = s_board_bus_config.scl_io_num;
    defaults.glitch_ignore_cnt = s_board_bus_config.glitch_ignore_cnt;
    defaults.enable_internal_pullup =
        s_board_bus_config.flags.enable_internal_pullup;
    defaults.clk_source = s_board_bus_config.clk_source;
    if (esp_board_manager_get_periph_handle("i2c_master",
                                            (void **)&s_board_bus) == ESP_OK) {
      s_board_bus_available = true;
    }
  }

  const desktop_fullscreen_layout_colors_t colors = {
      .canvas = C_BG,
      .surface = C_CARD,
      .ink = C_INK,
      .ink_muted = C_MUTED,
  };
  desktop_fullscreen_layout_t layout;
  if (!desktop_fullscreen_layout_create(parent, &colors, "", "", "", &layout)) {
    return;
  }
  /** Use the full page height for the I2C operation cards. */
  desktop_fullscreen_layout_remove_page_header(&layout);

  lv_obj_t *config_card = desktop_fullscreen_layout_create_panel(
      layout.content, C_CARD, 30, 12, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(config_card, 14, 0);
  lv_obj_set_style_pad_row(config_card, 0, 0);
  lv_obj_add_flag(config_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(config_card, LV_SCROLLBAR_MODE_OFF);
  lv_obj_t *config_title = i2ctools_view_label(
      config_card, L(DESKTOP_TEXT_I2CTOOLS_BUS), C_INK, 0, 0);
  lv_obj_set_width(config_title, LV_PCT(100));
  lv_obj_set_height(config_title, LV_PCT(8));
  lv_obj_t *config_fields = lv_obj_create(config_card);
  i2ctools_view_plain(config_fields, C_CARD, 0);
  lv_obj_set_width(config_fields, LV_PCT(100));
  lv_obj_set_height(config_fields, LV_PCT(92));
  lv_obj_set_layout(config_fields, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(config_fields, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(config_fields, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(config_fields, 3, 0);
  s_i2c_num = decimal_field(config_fields, L(DESKTOP_TEXT_I2CTOOLS_NUM), 100, 17,
                            defaults.i2c_port < 0 ? 0 : defaults.i2c_port);
  s_sda = decimal_field(config_fields, L(DESKTOP_TEXT_I2CTOOLS_SDA), 100, 17,
                        defaults.sda_io_num < 0 ? 0 : defaults.sda_io_num);
  s_scl = decimal_field(config_fields, L(DESKTOP_TEXT_I2CTOOLS_SCL), 100, 17,
                        defaults.scl_io_num < 0 ? 1 : defaults.scl_io_num);
  s_freq = decimal_field(
      config_fields, L(DESKTOP_TEXT_I2CTOOLS_FREQUENCY), 100, 17,
      defaults.frequency_hz == 0 ? 100
                                                    : defaults.frequency_hz / 1000);
  lv_obj_t *configure_button = i2ctools_view_flex_button(
      config_fields, L(DESKTOP_TEXT_I2CTOOLS_BUS), 100, 15,
      i2ctools_view_configure_cb);

  lv_obj_t *right_column = desktop_fullscreen_layout_create_panel(
      layout.content, C_BG, 0, 0, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(right_column, 8, 0);

  lv_obj_t *status_card = lv_obj_create(right_column);
  i2ctools_view_plain(status_card, C_CARD, 12);
  lv_obj_set_width(status_card, LV_PCT(44));
  lv_obj_set_height(status_card, LV_PCT(100));
  lv_obj_set_layout(status_card, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(status_card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(status_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_all(status_card, 14, 0);
  lv_obj_set_style_pad_row(status_card, 3, 0);
  lv_obj_add_flag(status_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(status_card, LV_SCROLLBAR_MODE_OFF);
  lv_obj_t *status_title = i2ctools_view_label(
      status_card, L(DESKTOP_TEXT_I2CTOOLS_STATUS), C_INK, 0, 0);
  lv_obj_set_width(status_title, LV_PCT(100));
  lv_obj_set_height(status_title, LV_PCT(8));

  lv_obj_t *operation_fields = lv_obj_create(status_card);
  i2ctools_view_plain(operation_fields, C_CARD, 0);
  lv_obj_set_width(operation_fields, LV_PCT(100));
  lv_obj_set_height(operation_fields, LV_PCT(92));
  lv_obj_set_layout(operation_fields, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(operation_fields, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(operation_fields, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(operation_fields, 3, 0);
  s_address = hex_field(
      operation_fields, L(DESKTOP_TEXT_I2CTOOLS_ADDRESS), 100, 17, "0x5B");
  s_register = hex_field(
      operation_fields, L(DESKTOP_TEXT_I2CTOOLS_REGISTER), 100, 17, "0x00");
  s_length = decimal_field(
      operation_fields, L(DESKTOP_TEXT_I2CTOOLS_LENGTH), 100, 17, 1);
  s_write_data = i2ctools_view_flex_text_field(
      operation_fields, L(DESKTOP_TEXT_I2CTOOLS_DATA), 100, 17, "00 01 FF");
  lv_textarea_set_accepted_chars(s_write_data, "0123456789abcdefABCDEF xX,;");
  lv_textarea_set_max_length(s_write_data, I2CTOOLS_APP_MAX_WRITE_DATA * 3U);
  s_chunk = decimal_field(operation_fields, L(DESKTOP_TEXT_I2CTOOLS_CHUNK_SIZE),
                          100, 17, 1);

  lv_obj_t *actions_card = lv_obj_create(right_column);
  i2ctools_view_plain(actions_card, C_CARD, 10);
  lv_obj_set_width(actions_card, LV_PCT(20));
  lv_obj_set_height(actions_card, LV_PCT(100));
  lv_obj_set_layout(actions_card, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(actions_card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(actions_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_all(actions_card, 8, 0);
  lv_obj_set_style_pad_row(actions_card, 3, 0);
  lv_obj_t *actions_title = i2ctools_view_label(
      actions_card, L(DESKTOP_TEXT_I2CTOOLS_STATUS), C_INK, 0, 0);
  lv_obj_set_width(actions_title, LV_PCT(100));
  lv_obj_set_height(actions_title, LV_PCT(8));
  i2ctools_view_flex_button(actions_card, L(DESKTOP_TEXT_I2CTOOLS_SCAN), 100, 16,
                            i2ctools_view_scan_cb);
  i2ctools_view_flex_button(actions_card, L(DESKTOP_TEXT_I2CTOOLS_READ), 100, 16,
                            i2ctools_view_read_cb);
  i2ctools_view_flex_button(actions_card, L(DESKTOP_TEXT_I2CTOOLS_WRITE), 100, 16,
                            i2ctools_view_write_cb);
  i2ctools_view_flex_button(actions_card, L(DESKTOP_TEXT_I2CTOOLS_DUMP), 100, 16,
                            i2ctools_view_dump_cb);
  i2ctools_view_flex_button(actions_card, L(DESKTOP_TEXT_I2CTOOLS_CLEAR), 100, 16,
                            i2ctools_view_clear_cb);

  lv_obj_t *output_card = lv_obj_create(right_column);
  i2ctools_view_plain(output_card, C_CARD, 12);
  lv_obj_set_width(output_card, LV_PCT(32));
  lv_obj_set_height(output_card, LV_PCT(100));
  lv_obj_set_layout(output_card, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(output_card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(output_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_all(output_card, 12, 0);
  lv_obj_set_style_pad_row(output_card, 8, 0);
  lv_obj_t *output_title = i2ctools_view_label(
      output_card, L(DESKTOP_TEXT_I2CTOOLS_COMMUNICATION), C_INK, 0, 0);
  lv_obj_set_width(output_title, LV_PCT(100));
  lv_obj_set_height(output_title, LV_PCT(10));
  s_status = i2ctools_view_label(output_card, L(DESKTOP_TEXT_I2CTOOLS_READY),
                                 C_SUCCESS, 0, 0);
  lv_obj_set_width(s_status, LV_PCT(100));
  lv_obj_set_height(s_status, LV_PCT(10));
  lv_obj_add_flag(output_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(output_card, LV_SCROLLBAR_MODE_OFF);
  s_output = i2ctools_view_label(output_card, "", C_INK, 0, 0);
  lv_obj_set_width(s_output, LV_PCT(100));
  lv_obj_set_height(s_output, LV_SIZE_CONTENT);
  lv_label_set_long_mode(s_output, LV_LABEL_LONG_WRAP);

  i2ctools_view_input_init(&s_input, config_card, status_card, s_write_data,
                           close_button);
  i2ctools_view_input_bind(&s_input, s_i2c_num, s_input.config_keyboard, false);
  i2ctools_view_input_bind(&s_input, s_sda, s_input.config_keyboard, false);
  i2ctools_view_input_bind(&s_input, s_scl, s_input.config_keyboard, false);
  i2ctools_view_input_bind(&s_input, s_freq, s_input.config_keyboard, false);
  i2ctools_view_input_bind(&s_input, s_address, s_input.operation_keyboard, true);
  i2ctools_view_input_bind(&s_input, s_register, s_input.operation_keyboard, true);
  i2ctools_view_input_bind(&s_input, s_length, s_input.operation_keyboard, false);
  i2ctools_view_input_bind(&s_input, s_write_data, s_input.operation_keyboard, true);
  i2ctools_view_input_bind(&s_input, s_chunk, s_input.operation_keyboard, false);
  lv_obj_set_style_text_font(parent, desktop_font_get_primary(), 0);
  if (i2ctools_app_create(&s_app) != ESP_OK) {
    i2ctools_view_status_text(L(DESKTOP_TEXT_I2CTOOLS_UNAVAILABLE), C_ERROR);
  } else if (s_board_bus_available && defaults.sda_io_num >= 0 &&
             defaults.scl_io_num >= 0) {
    defaults.timeout_ms = I2CTOOLS_APP_DEFAULT_TIMEOUT_MS;
    defaults.frequency_hz = I2CTOOLS_APP_DEFAULT_FREQUENCY_HZ;
    if (i2ctools_app_attach_bus(s_app, &defaults, s_board_bus) == ESP_OK) {
      s_configured = true;
    }
  }
}
