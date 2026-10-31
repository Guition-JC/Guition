#pragma once

#include "i2ctools_app.h"
#include "i2ctools_view_input.h"
#include "lvgl.h"

/** @brief Shared page backend and widget handles used by layout and operation callbacks. */
extern i2ctools_app_t *s_app;
extern i2ctools_view_input_t s_input;
extern lv_obj_t *s_i2c_num;
extern lv_obj_t *s_sda;
extern lv_obj_t *s_scl;
extern lv_obj_t *s_freq;
extern lv_obj_t *s_address;
extern lv_obj_t *s_register;
extern lv_obj_t *s_write_data;
extern lv_obj_t *s_length;
extern lv_obj_t *s_chunk;
extern lv_obj_t *s_status;
extern lv_obj_t *s_output;
extern bool s_configured;
extern i2c_master_bus_config_t s_board_bus_config;
extern i2c_master_bus_handle_t s_board_bus;
extern bool s_board_bus_available;

/**
 * @brief Set the page status text and color.
 * @param text Status text to display.
 * @param color Status text color.
 */
void i2ctools_view_status_text(const char *text, lv_color_t color);

/**
 * @brief Apply the selected bus configuration.
 * @param event LVGL configuration event.
 */
void i2ctools_view_configure_cb(lv_event_t *event);
/**
 * @brief Scan the configured I2C bus.
 * @param event LVGL scan event.
 */
void i2ctools_view_scan_cb(lv_event_t *event);
/**
 * @brief Read the selected register range.
 * @param event LVGL read event.
 */
void i2ctools_view_read_cb(lv_event_t *event);
/**
 * @brief Write the entered bytes to the selected register range.
 * @param event LVGL write event.
 */
void i2ctools_view_write_cb(lv_event_t *event);
/**
 * @brief Dump register contents using the selected chunk size.
 * @param event LVGL dump event.
 */
void i2ctools_view_dump_cb(lv_event_t *event);
/**
 * @brief Clear the communication log.
 * @param event LVGL clear event.
 */
void i2ctools_view_clear_cb(lv_event_t *event);
