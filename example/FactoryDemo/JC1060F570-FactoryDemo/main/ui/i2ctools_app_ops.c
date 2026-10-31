/**
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#include "i2ctools_app_internal.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2CTOOLS_SCAN_FIRST_ADDRESS 0x08U
#define I2CTOOLS_SCAN_LAST_ADDRESS 0x77U

/* Send only the 7-bit write address; no device registers are touched. */
static esp_err_t scan_address(i2ctools_app_t *app,
                              i2c_master_dev_handle_t device,
                              uint8_t address) {
  const uint8_t address_byte = (uint8_t)(address << 1U);
  i2c_operation_job_t operations[] = {
      {.command = I2C_MASTER_CMD_START},
      {.command = I2C_MASTER_CMD_WRITE,
       .write = {.ack_check = true, .data = &address_byte, .total_bytes = 1U}},
      {.command = I2C_MASTER_CMD_STOP},
  };
  const esp_err_t ret = i2c_master_execute_defined_operations(
      device, operations, sizeof(operations) / sizeof(operations[0]),
      (int)app->config.timeout_ms);
  /* Allow STOP completion and the RX8025 bus-free interval before another START. */
  vTaskDelay(pdMS_TO_TICKS(2U) ? pdMS_TO_TICKS(2U) : 1U);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_RESPONSE) {
    return ret;
  }
  const esp_err_t check = i2c_master_probe(app->bus, address,
                                           (int)app->config.timeout_ms);
  vTaskDelay(pdMS_TO_TICKS(2U) ? pdMS_TO_TICKS(2U) : 1U);
  if (check == ESP_ERR_TIMEOUT || (check != ESP_OK && check != ESP_ERR_NOT_FOUND)) {
    return check;
  }
  if (ret == ESP_OK && check == ESP_OK) {
    return ESP_OK;
  }
  if (ret == ESP_ERR_INVALID_RESPONSE && check == ESP_ERR_NOT_FOUND) {
    return ESP_ERR_NOT_FOUND;
  }
  return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t i2ctools_app_scan(i2ctools_app_t *app,
                            i2ctools_app_scan_result_t *result) {
  if (result == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t ret = i2ctools_app_require_configured(app);
  if (ret != ESP_OK) {
    return ret;
  }

  memset(result, 0, sizeof(*result));
  result->first_error = ESP_OK;

  /* Unlike i2c_master_probe(), a device transaction sets the address width for
     every operation, even if a previous user of this bus used 10-bit mode. */
  const i2c_device_config_t config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = I2C_DEVICE_ADDRESS_NOT_USED,
      .scl_speed_hz = app->config.frequency_hz,
  };
  i2c_master_dev_handle_t device = NULL;
  ret = i2c_master_bus_add_device(app->bus, &config, &device);
  if (ret != ESP_OK) {
    result->first_error = ret;
    return ret;
  }

  for (uint16_t address = I2CTOOLS_SCAN_FIRST_ADDRESS;
       address <= I2CTOOLS_SCAN_LAST_ADDRESS; ++address) {
    ret = scan_address(app, device, (uint8_t)address);
    if (ret == ESP_OK) {
      result->state[address] = I2CTOOLS_APP_PROBE_PRESENT;
      result->addresses[result->count++] = (uint8_t)address;
    } else if (ret == ESP_ERR_NOT_FOUND) {
      result->state[address] = I2CTOOLS_APP_PROBE_ABSENT;
    } else if (ret == ESP_ERR_TIMEOUT) {
      result->state[address] = I2CTOOLS_APP_PROBE_TIMEOUT;
      if (result->first_error == ESP_OK) {
        result->first_error = ret;
      }
    } else {
      result->state[address] = I2CTOOLS_APP_PROBE_ERROR;
      if (result->first_error == ESP_OK) {
        result->first_error = ret;
      }
    }
  }

  ret = i2c_master_bus_rm_device(device);
  if (result->first_error == ESP_OK) {
    result->first_error = ret;
  }
  return result->first_error;
}

esp_err_t i2ctools_app_read_registers(i2ctools_app_t *app,
                                      uint8_t device_address,
                                      uint8_t register_address, uint8_t *data,
                                      size_t len) {
  if (data == NULL || len == 0U) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t ret = i2ctools_app_require_configured(app);
  if (ret != ESP_OK) {
    return ret;
  }

  i2c_master_dev_handle_t device = NULL;
  ret = i2ctools_app_add_device(app, device_address, &device);
  if (ret != ESP_OK) {
    return ret;
  }

  ret = i2c_master_transmit_receive(device, &register_address, 1U, data, len,
                                    (int)app->config.timeout_ms);
  return i2ctools_app_remove_device(device, ret);
}

esp_err_t i2ctools_app_write_registers(i2ctools_app_t *app,
                                       uint8_t device_address,
                                       uint8_t register_address,
                                       const uint8_t *data, size_t len) {
  if ((data == NULL && len != 0U) || len > I2CTOOLS_APP_MAX_WRITE_DATA) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t ret = i2ctools_app_require_configured(app);
  if (ret != ESP_OK) {
    return ret;
  }

  uint8_t write_buffer[I2CTOOLS_APP_MAX_WRITE_DATA + 1U];
  write_buffer[0] = register_address;
  if (len != 0U) {
    memcpy(&write_buffer[1], data, len);
  }

  i2c_master_dev_handle_t device = NULL;
  ret = i2ctools_app_add_device(app, device_address, &device);
  if (ret != ESP_OK) {
    return ret;
  }

  ret = i2c_master_transmit(device, write_buffer, len + 1U,
                            (int)app->config.timeout_ms);
  return i2ctools_app_remove_device(device, ret);
}

esp_err_t i2ctools_app_dump_registers(i2ctools_app_t *app,
                                      uint8_t device_address,
                                      uint8_t chunk_size,
                                      i2ctools_app_dump_result_t *result) {
  if (result == NULL ||
      (chunk_size != 1U && chunk_size != 2U && chunk_size != 4U)) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t ret = i2ctools_app_require_configured(app);
  if (ret != ESP_OK) {
    return ret;
  }

  memset(result, 0, sizeof(*result));
  result->chunk_size = chunk_size;
  result->first_error = ESP_OK;

  i2c_master_dev_handle_t device = NULL;
  ret = i2ctools_app_add_device(app, device_address, &device);
  if (ret != ESP_OK) {
    result->first_error = ret;
    return ret;
  }

  for (uint16_t offset = 0; offset < I2CTOOLS_APP_DUMP_SIZE;
       offset += chunk_size) {
    uint8_t register_address = (uint8_t)offset;
    ret = i2c_master_transmit_receive(device, &register_address, 1U,
                                      &result->data[offset], chunk_size,
                                      (int)app->config.timeout_ms);
    if (ret == ESP_OK) {
      for (uint8_t index = 0; index < chunk_size; ++index) {
        result->valid[offset + index] = true;
        ++result->valid_count;
      }
    } else if (result->first_error == ESP_OK) {
      result->first_error = ret;
    }
  }

  ret = i2ctools_app_remove_device(device, result->first_error);
  result->first_error = ret;
  return ret;
}
