/**
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#include "i2ctools_app_internal.h"

#include <limits.h>
#include <stdlib.h>

esp_err_t i2ctools_app_require_configured(const i2ctools_app_t *app) {
  if (app == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  return app->configured && app->bus != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t i2ctools_app_validate_device_address(uint8_t device_address) {
  return device_address < I2CTOOLS_APP_ADDRESS_COUNT ? ESP_OK
                                                     : ESP_ERR_INVALID_ARG;
}

static esp_err_t validate_timeout(uint32_t timeout_ms) {
  if (timeout_ms == 0U || timeout_ms > (uint32_t)INT_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

static esp_err_t validate_config(const i2ctools_app_config_t *config) {
  if (config == NULL || config->sda_io_num == GPIO_NUM_NC ||
      config->scl_io_num == GPIO_NUM_NC ||
      config->sda_io_num == config->scl_io_num || config->frequency_hz == 0U) {
    return ESP_ERR_INVALID_ARG;
  }
  return validate_timeout(config->timeout_ms);
}

esp_err_t i2ctools_app_add_device(const i2ctools_app_t *app,
                                  uint8_t device_address,
                                  i2c_master_dev_handle_t *out_device) {
  if (out_device == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t ret = i2ctools_app_validate_device_address(device_address);
  if (ret != ESP_OK) {
    return ret;
  }

  const i2c_device_config_t device_config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = device_address,
      .scl_speed_hz = app->config.frequency_hz,
  };
  return i2c_master_bus_add_device(app->bus, &device_config, out_device);
}

esp_err_t i2ctools_app_remove_device(i2c_master_dev_handle_t device,
                                     esp_err_t operation_error) {
  const esp_err_t remove_error = i2c_master_bus_rm_device(device);
  return operation_error != ESP_OK ? operation_error : remove_error;
}

void i2ctools_app_config_init(i2ctools_app_config_t *config) {
  if (config == NULL) {
    return;
  }

  *config = (i2ctools_app_config_t){
      .i2c_port = -1,
      .sda_io_num = GPIO_NUM_NC,
      .scl_io_num = GPIO_NUM_NC,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .frequency_hz = I2CTOOLS_APP_DEFAULT_FREQUENCY_HZ,
      .timeout_ms = I2CTOOLS_APP_DEFAULT_TIMEOUT_MS,
      .glitch_ignore_cnt = I2CTOOLS_APP_DEFAULT_GLITCH_IGNORE_COUNT,
      .enable_internal_pullup = true,
  };
}

esp_err_t i2ctools_app_create(i2ctools_app_t **out_app) {
  if (out_app == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *out_app = calloc(1, sizeof(i2ctools_app_t));
  return *out_app == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t i2ctools_app_configure(i2ctools_app_t *app,
                                 const i2ctools_app_config_t *config) {
  if (app == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t ret = validate_config(config);
  if (ret != ESP_OK) {
    return ret;
  }

  if (app->configured) {
    ret = i2ctools_app_deinit(app);
    if (ret != ESP_OK) {
      return ret;
    }
  }

  const i2c_master_bus_config_t bus_config = {
      .i2c_port = config->i2c_port,
      .sda_io_num = config->sda_io_num,
      .scl_io_num = config->scl_io_num,
      .clk_source = config->clk_source,
      .glitch_ignore_cnt = config->glitch_ignore_cnt,
      .flags.enable_internal_pullup = config->enable_internal_pullup,
  };

  i2c_master_bus_handle_t bus = NULL;
  ret = i2c_new_master_bus(&bus_config, &bus);
  if (ret != ESP_OK) {
    return ret;
  }

  app->bus = bus;
  app->config = *config;
  app->configured = true;
  app->owns_bus = true;
  return ESP_OK;
}

esp_err_t i2ctools_app_attach_bus(i2ctools_app_t *app,
                                  const i2ctools_app_config_t *config,
                                  i2c_master_bus_handle_t bus) {
  if (app == NULL || bus == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t ret = validate_config(config);
  if (ret != ESP_OK) {
    return ret;
  }

  if (app->configured) {
    ret = i2ctools_app_deinit(app);
    if (ret != ESP_OK) {
      return ret;
    }
  }

  app->bus = bus;
  app->config = *config;
  app->configured = true;
  app->owns_bus = false;
  return ESP_OK;
}

esp_err_t i2ctools_app_deinit(i2ctools_app_t *app) {
  if (app == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!app->configured) {
    return ESP_OK;
  }

  if (app->owns_bus) {
    const esp_err_t ret = i2c_del_master_bus(app->bus);
    if (ret != ESP_OK) {
      return ret;
    }
  }

  app->bus = NULL;
  app->configured = false;
  app->owns_bus = false;
  return ESP_OK;
}

esp_err_t i2ctools_app_destroy(i2ctools_app_t *app) {
  if (app == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  const esp_err_t ret = i2ctools_app_deinit(app);
  if (ret == ESP_OK) {
    free(app);
  }
  return ret;
}

const char *i2ctools_app_error_name(esp_err_t error) {
  return esp_err_to_name(error);
}
