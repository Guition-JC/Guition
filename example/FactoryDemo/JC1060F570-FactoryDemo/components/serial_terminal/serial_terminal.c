#include "serial_terminal.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define TAG "SerialTerminal"

#if CONFIG_FACTORY_UART_ENABLE
#define SERIAL_TERMINAL_HISTORY_LIMIT 100
#define SERIAL_TERMINAL_RX_TASK_STACK_SIZE 4096
#define SERIAL_TERMINAL_RX_TASK_PRIORITY 5
#define SERIAL_TERMINAL_READ_TIMEOUT_MS 50
#define SERIAL_TERMINAL_LOCK_TIMEOUT_MS 2000U

typedef struct {
    serial_terminal_direction_t direction;
    size_t length;
    char text[CONFIG_FACTORY_UART_MESSAGE_MAX_LENGTH + 1];
} serial_terminal_message_t;

/** The UART driver copies into a stack buffer and this history is handed to the UI through the mutex, so no DMA or ISR touches it: it can live in PSRAM. */
EXT_RAM_BSS_ATTR static serial_terminal_message_t s_history[SERIAL_TERMINAL_HISTORY_LIMIT];
static size_t s_history_head;
static size_t s_history_count;
static unsigned int s_history_revision;
static SemaphoreHandle_t s_history_mutex;
static StaticSemaphore_t s_history_mutex_storage;
static bool s_ready;

static void history_push(serial_terminal_direction_t direction, const char *text, size_t length)
{
    if (length == 0 || s_history_mutex == NULL) {
        return;
    }
    if (length > CONFIG_FACTORY_UART_MESSAGE_MAX_LENGTH) {
        length = CONFIG_FACTORY_UART_MESSAGE_MAX_LENGTH;
    }

    if (xSemaphoreTake(s_history_mutex, pdMS_TO_TICKS(SERIAL_TERMINAL_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    size_t index = (s_history_head + s_history_count) % SERIAL_TERMINAL_HISTORY_LIMIT;
    if (s_history_count == SERIAL_TERMINAL_HISTORY_LIMIT) {
        index = s_history_head;
        s_history_head = (s_history_head + 1) % SERIAL_TERMINAL_HISTORY_LIMIT;
    } else {
        s_history_count++;
    }
    serial_terminal_message_t *message = &s_history[index];
    message->direction = direction;
    message->length = length;
    memcpy(message->text, text, length);
    message->text[length] = '\0';
    s_history_revision++;
    xSemaphoreGive(s_history_mutex);
}

static void receive_task(void *arg)
{
    (void)arg;
    char line[CONFIG_FACTORY_UART_MESSAGE_MAX_LENGTH + 1];
    size_t line_length = 0;
    uint8_t data[256];

    while (true) {
        int received = uart_read_bytes(CONFIG_FACTORY_UART_PORT, data, sizeof(data),
                                       pdMS_TO_TICKS(SERIAL_TERMINAL_READ_TIMEOUT_MS));
        if (received <= 0) {
            if (line_length > 0) {
                history_push(SERIAL_TERMINAL_DIRECTION_RX, line, line_length);
                line_length = 0;
            }
            continue;
        }

        for (int i = 0; i < received; i++) {
            char byte = (char)data[i];
            if (byte == '\r' || byte == '\n') {
                if (line_length > 0) {
                    history_push(SERIAL_TERMINAL_DIRECTION_RX, line, line_length);
                    line_length = 0;
                }
                continue;
            }
            if (line_length == CONFIG_FACTORY_UART_MESSAGE_MAX_LENGTH) {
                history_push(SERIAL_TERMINAL_DIRECTION_RX, line, line_length);
                line_length = 0;
            }
            line[line_length++] = byte;
        }
    }
}

esp_err_t serial_terminal_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_history_mutex = xSemaphoreCreateMutexStatic(&s_history_mutex_storage);
    if (s_history_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uart_config_t config = {
        .baud_rate = CONFIG_FACTORY_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(CONFIG_FACTORY_UART_PORT, CONFIG_FACTORY_UART_RX_BUFFER_SIZE,
                                        CONFIG_FACTORY_UART_RX_BUFFER_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        s_history_mutex = NULL;
        return err;
    }
    err = uart_param_config(CONFIG_FACTORY_UART_PORT, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        uart_driver_delete(CONFIG_FACTORY_UART_PORT);
        s_history_mutex = NULL;
        return err;
    }
    err = uart_set_pin(CONFIG_FACTORY_UART_PORT, CONFIG_FACTORY_UART_TX_GPIO,
                       CONFIG_FACTORY_UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        uart_driver_delete(CONFIG_FACTORY_UART_PORT);
        s_history_mutex = NULL;
        return err;
    }
    if (xTaskCreate(receive_task, "uart_rx", SERIAL_TERMINAL_RX_TASK_STACK_SIZE, NULL,
                    SERIAL_TERMINAL_RX_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UART RX task");
        uart_driver_delete(CONFIG_FACTORY_UART_PORT);
        s_history_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "UART%d ready at %d baud, TX=%d RX=%d",
             CONFIG_FACTORY_UART_PORT, CONFIG_FACTORY_UART_BAUD_RATE,
             CONFIG_FACTORY_UART_TX_GPIO, CONFIG_FACTORY_UART_RX_GPIO);
    return ESP_OK;
}

bool serial_terminal_is_ready(void)
{
    return s_ready;
}

esp_err_t serial_terminal_send(const char *text)
{
    if (!s_ready || text == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t length = strnlen(text, CONFIG_FACTORY_UART_MESSAGE_MAX_LENGTH + 1);
    if (length == 0 || length > CONFIG_FACTORY_UART_MESSAGE_MAX_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }

    char line[CONFIG_FACTORY_UART_MESSAGE_MAX_LENGTH + 3];
    memcpy(line, text, length);
    size_t wire_length = length;
    if (line[length - 1] != '\r' && line[length - 1] != '\n') {
        line[wire_length++] = '\r';
        line[wire_length++] = '\n';
    }
    int written = uart_write_bytes(CONFIG_FACTORY_UART_PORT, line, wire_length);
    if (written != (int)wire_length) {
        return ESP_FAIL;
    }
    history_push(SERIAL_TERMINAL_DIRECTION_TX, text, length);
    return ESP_OK;
}

void serial_terminal_history_clear(void)
{
    if (s_history_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_history_mutex, pdMS_TO_TICKS(SERIAL_TERMINAL_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    s_history_head = 0;
    s_history_count = 0;
    s_history_revision++;
    xSemaphoreGive(s_history_mutex);
}

unsigned int serial_terminal_history_revision(void)
{
    unsigned int revision;
    if (s_history_mutex == NULL) {
        return 0;
    }
    if (xSemaphoreTake(s_history_mutex, pdMS_TO_TICKS(SERIAL_TERMINAL_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return 0;
    }
    revision = s_history_revision;
    xSemaphoreGive(s_history_mutex);
    return revision;
}

size_t serial_terminal_history_format(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0 || s_history_mutex == NULL) {
        return 0;
    }

    size_t written = 0;
    if (xSemaphoreTake(s_history_mutex, pdMS_TO_TICKS(SERIAL_TERMINAL_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return 0;
    }
    for (size_t i = 0; i < s_history_count; i++) {
        size_t index = (s_history_head + i) % SERIAL_TERMINAL_HISTORY_LIMIT;
        const serial_terminal_message_t *message = &s_history[index];
        const char *prefix = message->direction == SERIAL_TERMINAL_DIRECTION_RX ? "[RX] " : "[TX] ";
        int line_length = snprintf(buffer + written, buffer_size - written, "%s%.*s\n",
                                   prefix, (int)message->length, message->text);
        if (line_length < 0 || (size_t)line_length >= buffer_size - written) {
            written = buffer_size - 1;
            break;
        }
        written += (size_t)line_length;
    }
    buffer[written] = '\0';
    xSemaphoreGive(s_history_mutex);
    return written;
}

#else

esp_err_t serial_terminal_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool serial_terminal_is_ready(void)
{
    return false;
}

esp_err_t serial_terminal_send(const char *text)
{
    (void)text;
    return ESP_ERR_NOT_SUPPORTED;
}

void serial_terminal_history_clear(void)
{
}

unsigned int serial_terminal_history_revision(void)
{
    return 0;
}

size_t serial_terminal_history_format(char *buffer, size_t buffer_size)
{
    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    return 0;
}

#endif
