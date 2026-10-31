/**
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include "i2ctools_app.h"

struct i2ctools_app {
  i2c_master_bus_handle_t bus;
  i2ctools_app_config_t config;
  bool configured;
  bool owns_bus;
};

/**
 * @brief Check that the backend exists and has a configured I2C bus.
 * @param app Backend to check.
 * @return ESP_OK if configured; otherwise an argument or state error.
 */
esp_err_t i2ctools_app_require_configured(const i2ctools_app_t *app);
/**
 * @brief Validate a seven-bit I2C device address.
 * @param device_address Seven-bit device address to validate.
 * @return ESP_OK for a valid address; otherwise an argument error.
 */
esp_err_t i2ctools_app_validate_device_address(uint8_t device_address);
/**
 * @brief Attach a temporary device at the configured bus frequency; the caller removes it after use.
 * @param app Configured backend.
 * @param device_address Seven-bit target address.
 * @param [out] out_device Receives the temporary device handle.
 * @return ESP_OK on success; otherwise an error code.
 */
esp_err_t i2ctools_app_add_device(const i2ctools_app_t *app,
                                  uint8_t device_address,
                                  i2c_master_dev_handle_t *out_device);
/**
 * @brief Remove the temporary device, preserving an earlier operation error when present.
 * @param device Temporary device to remove.
 * @param operation_error Result of the preceding operation.
 * @return The preceding error if present, otherwise the removal result.
 */
esp_err_t i2ctools_app_remove_device(i2c_master_dev_handle_t device,
                                     esp_err_t operation_error);
