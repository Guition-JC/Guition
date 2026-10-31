/** SPDX-License-Identifier: GPL-2.0-only */
/** Low-level RX8025 register access and status/offset conversion. */
#include "rx8025_internal.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "RX8025";

uint8_t rx8025_bin_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4U) | (value % 10U));
}

uint8_t rx8025_bcd_to_bin(uint8_t value)
{
    return (uint8_t)(((value >> 4U) * 10U) + (value & 0x0fU));
}

esp_err_t rx8025_read_registers(rx8025_handle_t handle, uint8_t reg, uint8_t *data, size_t length)
{
    ESP_RETURN_ON_FALSE(handle != NULL && data != NULL && length > 0 && length <= RX8025_MAX_TRANSFER,
                        ESP_ERR_INVALID_ARG, "RX8025", "invalid read arguments");

    const uint8_t command = (uint8_t)(reg << 4U);
    esp_err_t ret = i2c_master_transmit_receive(handle->i2c_device, &command, 1, data, length, handle->timeout_ms);
    esp_rom_delay_us(RX8025_STOP_TO_START_DELAY_US);
    return ret;
}

esp_err_t rx8025_write_registers(rx8025_handle_t handle, uint8_t reg, const uint8_t *data, size_t length)
{
    ESP_RETURN_ON_FALSE(handle != NULL && data != NULL && length > 0 && length <= RX8025_MAX_TRANSFER,
                        ESP_ERR_INVALID_ARG, "RX8025", "invalid write arguments");

    uint8_t transaction[1U + RX8025_MAX_TRANSFER];
    transaction[0] = (uint8_t)(reg << 4U);
    memcpy(&transaction[1], data, length);
    esp_err_t ret = i2c_master_transmit(handle->i2c_device, transaction, length + 1U, handle->timeout_ms);
    esp_rom_delay_us(RX8025_STOP_TO_START_DELAY_US);
    return ret;
}

esp_err_t rx8025_write_register(rx8025_handle_t handle, uint8_t reg, uint8_t value)
{
    return rx8025_write_registers(handle, reg, &value, 1);
}

esp_err_t rx8025_read_register(rx8025_handle_t handle, uint8_t reg, uint8_t *value)
{
    return rx8025_read_registers(handle, reg, value, 1);
}

bool rx8025_is_oscillator_stopped(const struct rx8025_dev_t *handle, uint8_t ctrl2)
{
    const bool xst = (ctrl2 & RX8025_BIT_CTRL2_XST) != 0;
    return handle->model == RX8025_MODEL_RX8025 ? !xst : xst;
}

esp_err_t rx8025_reset_validity(rx8025_handle_t handle)
{
    uint8_t ctrl2_before = 0;
    esp_err_t ret = rx8025_read_register(handle, RX8025_REG_CTRL2, &ctrl2_before);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t ctrl2_target = (uint8_t)(ctrl2_before &
                                     (uint8_t)~(RX8025_BIT_CTRL2_PON | RX8025_BIT_CTRL2_VDET));
    if (handle->model == RX8025_MODEL_RX8025) {
        ctrl2_target |= RX8025_BIT_CTRL2_XST;
    } else {
        ctrl2_target = (uint8_t)(ctrl2_target & (uint8_t)~RX8025_BIT_CTRL2_XST);
    }

    const esp_err_t write_result = rx8025_write_register(handle, RX8025_REG_CTRL2, ctrl2_target);
    uint8_t ctrl2_after = 0;
    esp_err_t after_result = write_result;
    if (write_result == ESP_OK) {
        esp_rom_delay_us(1000U);
        after_result = rx8025_read_register(handle, RX8025_REG_CTRL2, &ctrl2_after);
    }
    ESP_LOGI(TAG,
             "ctrl2 validity reset: ctrl2_before=0x%02x ctrl2_target=0x%02x "
             "ctrl2_write_result=%s ctrl2_after_result=%s ctrl2_after=0x%02x",
             ctrl2_before, ctrl2_target, esp_err_to_name(write_result),
             esp_err_to_name(after_result), ctrl2_after);

    if (write_result != ESP_OK) {
        return write_result;
    }
    return after_result;
}

esp_err_t rx8025_read_status(rx8025_handle_t handle, rx8025_status_t *status)
{
    ESP_RETURN_ON_FALSE(handle != NULL && status != NULL, ESP_ERR_INVALID_ARG, "RX8025", "invalid status arguments");

    uint8_t ctrl2 = 0;
    esp_err_t ret = rx8025_read_register(handle, RX8025_REG_CTRL2, &ctrl2);
    if (ret != ESP_OK) {
        return ret;
    }

    status->raw_control2 = ctrl2;
    status->voltage_drop = (ctrl2 & RX8025_BIT_CTRL2_VDET) != 0;
    status->power_on_reset = (ctrl2 & RX8025_BIT_CTRL2_PON) != 0;
    status->oscillator_stopped = rx8025_is_oscillator_stopped(handle, ctrl2);
    status->alarm_flag = (ctrl2 & RX8025_BIT_CTRL2_DAFG) != 0;
    status->weekday_alarm_flag = (ctrl2 & RX8025_BIT_CTRL2_WAFG) != 0;
    status->periodic_flag = (ctrl2 & RX8025_BIT_CTRL2_CTFG) != 0;
    return ESP_OK;
}

esp_err_t rx8025_clear_interrupt_flags(rx8025_handle_t handle)
{
    uint8_t ctrl2 = 0;
    esp_err_t ret = rx8025_read_register(handle, RX8025_REG_CTRL2, &ctrl2);
    if (ret != ESP_OK) {
        return ret;
    }

    ctrl2 = (uint8_t)(ctrl2 & (uint8_t)~(RX8025_BIT_CTRL2_DAFG | RX8025_BIT_CTRL2_WAFG | RX8025_BIT_CTRL2_CTFG));
    return rx8025_write_register(handle, RX8025_REG_CTRL2, ctrl2);
}

esp_err_t rx8025_read_offset(rx8025_handle_t handle, int32_t *offset_ppb)
{
    ESP_RETURN_ON_FALSE(handle != NULL && offset_ppb != NULL, ESP_ERR_INVALID_ARG, "RX8025", "invalid offset arguments");

    uint8_t raw = 0;
    esp_err_t ret = rx8025_read_register(handle, RX8025_REG_DIGOFF, &raw);
    if (ret != ESP_OK) {
        return ret;
    }

    int32_t offset = raw >= 64U ? (int32_t)raw - 128 : raw;
    if (offset > 0) {
        offset--;
    }
    *offset_ppb = offset * RX8025_ADJ_RESOLUTION_PPB;
    return ESP_OK;
}

esp_err_t rx8025_set_offset(rx8025_handle_t handle, int32_t offset_ppb)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, "RX8025", "invalid handle");

    int32_t data = offset_ppb / RX8025_ADJ_RESOLUTION_PPB;
    if (data > RX8025_ADJ_DATA_MAX) {
        data = RX8025_ADJ_DATA_MAX;
    } else if (data < RX8025_ADJ_DATA_MIN) {
        data = RX8025_ADJ_DATA_MIN;
    } else if (data > 0) {
        data++;
    } else if (data < 0) {
        data += 128;
    }
    return rx8025_write_register(handle, RX8025_REG_DIGOFF, (uint8_t)data);
}
