/**
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <sys/socket.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/queue.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "iperf.h"
#include "iperf_private.h"

static const char *TAG = "iperf";

#ifdef CONFIG_FREERTOS_NUMBER_OF_CORES

#define NUMBER_OF_CORES CONFIG_FREERTOS_NUMBER_OF_CORES
#else
#define NUMBER_OF_CORES portNUM_PROCESSORS
#endif

#define IPERF_TICK_US                       (1000000)
#define IPERF_TIME_FORCE_ELAPSED            (0)
#define IPERF_TCP_CONNECT_TIMEOUT_MS        (1000)
#define IPERF_TCP_CONNECT_POLL_MS           (100)
#define IPERF_LIST_LOCK_TMO_RTOS_TICKS      portMAX_DELAY

#define TAG_ID iperf_instance->tag

typedef esp_err_t (*iperf_list_exec_fn)(iperf_instance_data_t *iperf_instance, void *ctx);

DRAM_ATTR static LIST_HEAD(iperfs_list, iperf_instance_data_struct) running_iperfs_list = LIST_HEAD_INITIALIZER(running_iperfs_list);
static struct iperfs_list *p_running_iperfs_list = &running_iperfs_list;

static StaticSemaphore_t s_list_lock_buffer; /**< Static storage for the mutex */
static SemaphoreHandle_t s_list_lock = NULL;
static portMUX_TYPE s_list_lock_init_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t iperf_stop_exec(iperf_instance_data_t *iperf_instance);
static esp_err_t iperf_request_stop(iperf_instance_data_t *iperf_instance);
static bool iperf_ensure_list_lock(void);
static esp_err_t iperf_close_socket(iperf_instance_data_t *iperf_instance, _Atomic int *socket_handle);
static int iperf_create_socket(iperf_instance_data_t *iperf_instance, _Atomic int *socket_handle,
                               int domain, int type, int protocol);
static esp_err_t iperf_bind_socket(iperf_instance_data_t *iperf_instance,
                                   _Atomic int *socket_handle,
                                   const struct sockaddr *address, socklen_t address_length);
static esp_err_t iperf_set_socket_option(iperf_instance_data_t *iperf_instance,
                                         _Atomic int *socket_handle,
                                         int level, int option, const void *value, socklen_t value_length);
static esp_err_t iperf_listen_socket(iperf_instance_data_t *iperf_instance,
                                     _Atomic int *socket_handle, int backlog);
static int iperf_accept_socket(iperf_instance_data_t *iperf_instance,
                               struct sockaddr *address, socklen_t *address_length);
static esp_err_t iperf_publish_target_addr(iperf_instance_data_t *iperf_instance,
                                            const struct sockaddr *address,
                                            socklen_t address_length);
static ssize_t iperf_send_socket(iperf_instance_data_t *iperf_instance,
                                 const void *buffer, size_t length);
static ssize_t iperf_receive_socket(iperf_instance_data_t *iperf_instance,
                                    void *buffer, size_t length);
static esp_err_t iperf_connect_with_timeout(iperf_instance_data_t *iperf_instance,
                                             const struct sockaddr *address,
                                             socklen_t address_length);
static esp_err_t iperf_delete_instance(iperf_instance_data_t *iperf_instance);
static iperf_traffic_type_t iperf_get_traffic_type_internal(iperf_instance_data_t *iperf_instance);
extern void iperf_report_task(void *arg);

static bool iperf_socket_slot_is_listen(iperf_instance_data_t *iperf_instance,
                                        _Atomic int *socket_handle)
{
    return socket_handle == &iperf_instance->listen_socket;
}

static uint32_t *iperf_socket_io_users(iperf_instance_data_t *iperf_instance,
                                       _Atomic int *socket_handle)
{
    return iperf_socket_slot_is_listen(iperf_instance, socket_handle) ?
           &iperf_instance->listen_socket_io_users : &iperf_instance->socket_io_users;
}

static bool *iperf_socket_closing(iperf_instance_data_t *iperf_instance,
                                  _Atomic int *socket_handle)
{
    return iperf_socket_slot_is_listen(iperf_instance, socket_handle) ?
           &iperf_instance->listen_socket_closing : &iperf_instance->socket_closing;
}

static void iperf_record_error(esp_err_t *ret, esp_err_t candidate)
{
    if (*ret == ESP_OK && candidate != ESP_OK) {
        *ret = candidate;
    }
}

static bool iperf_ensure_list_lock(void)
{
    bool initialized;

    portENTER_CRITICAL(&s_list_lock_init_mux);
    if (s_list_lock == NULL) {
        s_list_lock = xSemaphoreCreateMutexStatic(&s_list_lock_buffer);
    }
    initialized = s_list_lock != NULL;
    portEXIT_CRITICAL(&s_list_lock_init_mux);
    return initialized;
}

static esp_err_t iperf_close_socket(iperf_instance_data_t *iperf_instance, _Atomic int *socket_handle)
{
    if (iperf_instance->socket_lock == NULL ||
        xSemaphoreTake(iperf_instance->socket_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const int socket_fd = atomic_load(socket_handle);
    if (socket_fd < 0) {
        xSemaphoreGive(iperf_instance->socket_lock);
        return ESP_OK;
    }
    *iperf_socket_closing(iperf_instance, socket_handle) = true;
    xSemaphoreGive(iperf_instance->socket_lock);

    esp_err_t ret = ESP_OK;
    if (shutdown(socket_fd, SHUT_RDWR) < 0 && errno != ENOTCONN) {
        ret = ESP_FAIL;
    }

    for (;;) {
        if (xSemaphoreTake(iperf_instance->socket_lock, portMAX_DELAY) != pdTRUE) {
            return ret == ESP_OK ? ESP_ERR_TIMEOUT : ret;
        }
        const bool idle = *iperf_socket_io_users(iperf_instance, socket_handle) == 0U;
        if (idle) {
            /** Retain socket ownership until close succeeds. */
            if (atomic_load(socket_handle) == socket_fd) {
                if (close(socket_fd) < 0) {
                    ret = ESP_FAIL;
                } else {
                    atomic_store(socket_handle, -1);
                    *iperf_socket_closing(iperf_instance, socket_handle) = false;
                }
            } else if (atomic_load(socket_handle) < 0) {
                *iperf_socket_closing(iperf_instance, socket_handle) = false;
            }
            xSemaphoreGive(iperf_instance->socket_lock);
            return ret;
        }
        xSemaphoreGive(iperf_instance->socket_lock);
        vTaskDelay(1);
    }
}

static bool iperf_begin_socket_io(iperf_instance_data_t *iperf_instance,
                                  _Atomic int *socket_handle, int *socket_fd)
{
    if (iperf_instance->socket_lock == NULL ||
        xSemaphoreTake(iperf_instance->socket_lock, portMAX_DELAY) != pdTRUE) {
        errno = EBUSY;
        return false;
    }
    const int fd = atomic_load(socket_handle);
    if (!atomic_load(&iperf_instance->is_running) ||
        *iperf_socket_closing(iperf_instance, socket_handle) || fd < 0) {
        errno = fd < 0 ? EBADF : ECANCELED;
        xSemaphoreGive(iperf_instance->socket_lock);
        return false;
    }
    ++*iperf_socket_io_users(iperf_instance, socket_handle);
    *socket_fd = fd;
    xSemaphoreGive(iperf_instance->socket_lock);
    return true;
}

static void iperf_end_socket_io(iperf_instance_data_t *iperf_instance,
                                _Atomic int *socket_handle)
{
    if (iperf_instance->socket_lock == NULL ||
        xSemaphoreTake(iperf_instance->socket_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    uint32_t *users = iperf_socket_io_users(iperf_instance, socket_handle);
    if (*users > 0U) {
        --*users;
    }
    xSemaphoreGive(iperf_instance->socket_lock);
}

static bool iperf_publish_accepted_socket(iperf_instance_data_t *iperf_instance,
                                          int listen_fd, int accepted_fd)
{
    bool published = false;
    if (xSemaphoreTake(iperf_instance->socket_lock, portMAX_DELAY) == pdTRUE) {
        if (atomic_load(&iperf_instance->is_running) &&
            !iperf_instance->listen_socket_closing &&
            atomic_load(&iperf_instance->listen_socket) == listen_fd &&
            atomic_load(&iperf_instance->socket) < 0) {
            /** The traffic task owns listener closure and any required retries. */
            atomic_store(&iperf_instance->socket, accepted_fd);
            iperf_instance->listen_socket_closing = false;
            published = true;
        }
        xSemaphoreGive(iperf_instance->socket_lock);
    }
    return published;
}

static int iperf_create_socket(iperf_instance_data_t *iperf_instance, _Atomic int *socket_handle,
                               int domain, int type, int protocol)
{
    if (iperf_instance->socket_lock == NULL ||
        xSemaphoreTake(iperf_instance->socket_lock, portMAX_DELAY) != pdTRUE) {
        errno = EBUSY;
        return -1;
    }
    int socket_fd = -1;
    if (atomic_load(&iperf_instance->is_running) && atomic_load(socket_handle) < 0) {
        socket_fd = socket(domain, type, protocol);
        if (socket_fd >= 0 && atomic_load(&iperf_instance->is_running)) {
            atomic_store(socket_handle, socket_fd);
        } else if (socket_fd >= 0) {
            close(socket_fd);
            socket_fd = -1;
            errno = ECANCELED;
        }
    } else if (atomic_load(socket_handle) >= 0) {
        errno = EBUSY;
    } else {
        errno = ECANCELED;
    }
    xSemaphoreGive(iperf_instance->socket_lock);
    return socket_fd;
}

static esp_err_t iperf_bind_socket(iperf_instance_data_t *iperf_instance,
                                   _Atomic int *socket_handle,
                                   const struct sockaddr *address, socklen_t address_length)
{
    if (iperf_instance->socket_lock == NULL ||
        xSemaphoreTake(iperf_instance->socket_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    const int socket_fd = atomic_load(socket_handle);
    const int result = socket_fd >= 0 && atomic_load(&iperf_instance->is_running) ?
                       bind(socket_fd, address, address_length) : -1;
    if (result < 0 && socket_fd < 0) {
        errno = EBADF;
    }
    xSemaphoreGive(iperf_instance->socket_lock);
    return result == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t iperf_set_socket_option(iperf_instance_data_t *iperf_instance,
                                         _Atomic int *socket_handle,
                                         int level, int option, const void *value, socklen_t value_length)
{
    if (iperf_instance->socket_lock == NULL ||
        xSemaphoreTake(iperf_instance->socket_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    const int socket_fd = atomic_load(socket_handle);
    const int result = socket_fd >= 0 && atomic_load(&iperf_instance->is_running) ?
                       setsockopt(socket_fd, level, option, value, value_length) : -1;
    if (result < 0 && socket_fd < 0) {
        errno = EBADF;
    }
    xSemaphoreGive(iperf_instance->socket_lock);
    return result == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t iperf_listen_socket(iperf_instance_data_t *iperf_instance,
                                     _Atomic int *socket_handle, int backlog)
{
    if (iperf_instance->socket_lock == NULL ||
        xSemaphoreTake(iperf_instance->socket_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const int socket_fd = atomic_load(socket_handle);
    const int result = socket_fd >= 0 && atomic_load(&iperf_instance->is_running) ?
                       listen(socket_fd, backlog) : -1;
    if (result < 0 && socket_fd < 0) {
        errno = EBADF;
    }
    xSemaphoreGive(iperf_instance->socket_lock);
    return result == 0 ? ESP_OK : ESP_FAIL;
}

static int iperf_accept_socket(iperf_instance_data_t *iperf_instance,
                               struct sockaddr *address, socklen_t *address_length)
{
    int listen_fd = -1;
    if (!iperf_begin_socket_io(iperf_instance, &iperf_instance->listen_socket, &listen_fd)) {
        return -1;
    }
    const int accepted_fd = accept(listen_fd, address, address_length);
    iperf_end_socket_io(iperf_instance, &iperf_instance->listen_socket);
    if (accepted_fd < 0) {
        return -1;
    }

    /** Keep the listener published for closure by the traffic task. */
    if (!iperf_publish_accepted_socket(iperf_instance, listen_fd, accepted_fd)) {
        (void)shutdown(accepted_fd, SHUT_RDWR);
        (void)close(accepted_fd);
        errno = ECANCELED;
        return -1;
    }
    return accepted_fd;
}

static esp_err_t iperf_publish_target_addr(iperf_instance_data_t *iperf_instance,
                                            const struct sockaddr *address,
                                            socklen_t address_length)
{
    if (iperf_instance->socket_lock == NULL ||
        xSemaphoreTake(iperf_instance->socket_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(&iperf_instance->socket_info.target_addr, address, address_length);
    iperf_instance->socket_info.target_addr_len = address_length;
    xSemaphoreGive(iperf_instance->socket_lock);
    return ESP_OK;
}

static ssize_t iperf_send_socket(iperf_instance_data_t *iperf_instance,
                                 const void *buffer, size_t length)
{
    int socket_fd = -1;
    if (!iperf_begin_socket_io(iperf_instance, &iperf_instance->socket, &socket_fd)) {
        return -1;
    }
    struct sockaddr_storage target_addr = {0};
    socklen_t target_addr_len = 0;
    if (xSemaphoreTake(iperf_instance->socket_lock, portMAX_DELAY) == pdTRUE) {
        target_addr = iperf_instance->socket_info.target_addr;
        target_addr_len = iperf_instance->socket_info.target_addr_len;
        xSemaphoreGive(iperf_instance->socket_lock);
    }
    const ssize_t result = sendto(socket_fd, buffer, length, 0,
                                  (const struct sockaddr *)&target_addr, target_addr_len);
    iperf_end_socket_io(iperf_instance, &iperf_instance->socket);
    return result;
}

static ssize_t iperf_receive_socket(iperf_instance_data_t *iperf_instance,
                                    void *buffer, size_t length)
{
    int socket_fd = -1;
    if (!iperf_begin_socket_io(iperf_instance, &iperf_instance->socket, &socket_fd)) {
        return -1;
    }
    socklen_t address_length = sizeof(iperf_instance->socket_info.target_addr);
    struct sockaddr_storage target_addr = {0};
    const ssize_t result = recvfrom(socket_fd, buffer, length, 0,
                                    (struct sockaddr *)&target_addr, &address_length);
    if (result >= 0 && xSemaphoreTake(iperf_instance->socket_lock, portMAX_DELAY) == pdTRUE) {
        iperf_instance->socket_info.target_addr = target_addr;
        iperf_instance->socket_info.target_addr_len = address_length;
        xSemaphoreGive(iperf_instance->socket_lock);
    }
    iperf_end_socket_io(iperf_instance, &iperf_instance->socket);
    return result;
}

static esp_err_t iperf_connect_with_timeout(iperf_instance_data_t *iperf_instance,
                                             const struct sockaddr *address,
                                             socklen_t address_length)
{
    esp_err_t ret = ESP_OK;
    bool connected = false;
    int socket_fd = -1;
    int original_flags = -1;
    bool nonblocking = false;
    if (!iperf_begin_socket_io(iperf_instance, &iperf_instance->socket, &socket_fd)) {
        return !atomic_load(&iperf_instance->is_running) ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    original_flags = fcntl(socket_fd, F_GETFL, 0);
    if (original_flags < 0 || fcntl(socket_fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        ret = ESP_FAIL;
        goto out;
    }
    nonblocking = true;
    if (connect(socket_fd, address, address_length) == 0) {
        connected = true;
    } else if (errno == EINPROGRESS || errno == EALREADY) {
        const TickType_t timeout_ticks = pdMS_TO_TICKS(IPERF_TCP_CONNECT_TIMEOUT_MS);
        const TickType_t start_ticks = xTaskGetTickCount();
        while (atomic_load(&iperf_instance->is_running)) {
            if (xTaskGetTickCount() - start_ticks >= timeout_ticks) {
                errno = ETIMEDOUT;
                ret = ESP_ERR_TIMEOUT;
                break;
            }

            fd_set write_set;
            fd_set error_set;
            FD_ZERO(&write_set);
            FD_ZERO(&error_set);
            FD_SET(socket_fd, &write_set);
            FD_SET(socket_fd, &error_set);
            struct timeval wait = {
                .tv_sec = 0,
                .tv_usec = IPERF_TCP_CONNECT_POLL_MS * 1000,
            };
            const int ready = select(socket_fd + 1, NULL, &write_set, &error_set, &wait);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ret = ESP_FAIL;
                break;
            }
            if (ready == 0) {
                continue;
            }

            int socket_error = 0;
            socklen_t socket_error_length = sizeof(socket_error);
            if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error,
                           &socket_error_length) < 0) {
                ret = ESP_FAIL;
                break;
            }
            if (socket_error != 0) {
                errno = socket_error;
                ret = ESP_FAIL;
                break;
            }
            connected = true;
            break;
        }
        if (!connected && !atomic_load(&iperf_instance->is_running) && ret == ESP_OK) {
            errno = ECANCELED;
            ret = ESP_ERR_INVALID_STATE;
        }
    } else {
        ret = ESP_FAIL;
    }

out:
    if (nonblocking && fcntl(socket_fd, F_SETFL, original_flags) < 0 && ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    iperf_end_socket_io(iperf_instance, &iperf_instance->socket);
    return ret;
}

/**
 * @brief iperf state: IPERF_STARTED, IPERF_RUNNING, IPERF_STOPPED, IPERF_CLOSED
 * @param iperf_state State to deliver to the handler.
 * @param iperf_instance Instance whose handler is called.
 */
void iperf_state_action(iperf_state_t iperf_state, iperf_instance_data_t *iperf_instance)
{
    if (iperf_instance->state_handler) {
        iperf_state_data_t data = {};
        data.state = iperf_state;
        data.traffic_type = iperf_get_traffic_type_internal(iperf_instance);
        iperf_instance->state_handler(iperf_instance->id, &data, iperf_instance->state_handler_priv);
    }
    /** release report task to print connect info */
    if (iperf_state == IPERF_STARTED && iperf_instance->report_task_hdl) {
        xTaskNotifyGive(iperf_instance->report_task_hdl);
    }
}

IRAM_ATTR static void tx_timer_cb(void *arg)
{
    iperf_instance_data_t *iperf_instance = (iperf_instance_data_t *)arg;
    /** notify traffic task that it's time to transmit */
    xTaskNotifyGive(iperf_instance->traffic_task_hdl);
}

IRAM_ATTR static void tick_timer_cb(void *arg)
{
    iperf_instance_data_t *iperf_instance = (iperf_instance_data_t *)arg;

    iperf_instance->timers.ticks++;
    iperf_instance->timers.to_report_ticks++;

    if (iperf_instance->timers.to_report_ticks >= iperf_instance->interval ||
        iperf_instance->timers.ticks >= iperf_instance->time) {
        iperf_report_task_data_t report_task_data_tmp = atomic_load(&iperf_instance->report_task_data);
        /** zero indicates the report task processed data */
        if (report_task_data_tmp.period_sec == 0) {
            report_task_data_tmp.period_data_snapshot = atomic_exchange(&iperf_instance->period_data_passed, 0);
            report_task_data_tmp.period_sec = iperf_instance->timers.to_report_ticks;
        } else {
            /** if report task hasn't executed yet, try extend the report period */
            report_task_data_tmp.period_data_snapshot += atomic_load(&iperf_instance->period_data_passed);
            report_task_data_tmp.period_sec += iperf_instance->timers.to_report_ticks;
            ESP_LOGW(TAG_ID, "report_task is starving for execution time!");
        }
        atomic_store(&iperf_instance->report_task_data, report_task_data_tmp);
        iperf_instance->timers.to_report_ticks = 0;

        if (iperf_instance->timers.ticks >= iperf_instance->time) {
            atomic_store(&iperf_instance->timers.expired, true);
            xTaskNotifyGive(iperf_instance->traffic_task_hdl);
        } else {
            /** notify report task that it's time to report */
            xTaskNotifyGive(iperf_instance->report_task_hdl);
        }
    }
}

static esp_err_t iperf_create_timers(iperf_instance_data_t *iperf_instance)
{
    /** setup timers */
    esp_timer_create_args_t tick_timer_args = {
        .callback = &tick_timer_cb,
        .arg = iperf_instance,
        .name = "iperf_tick_timer"
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &(iperf_instance->timers.tick_timer)), TAG_ID, "failed to create tick timer");

    if (iperf_instance->timers.tx_period_us > 0) {
        esp_timer_create_args_t tx_timer_args = {
            .callback = &tx_timer_cb,
            .arg = iperf_instance,
            .name = "iperf_tx_timer"
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&tx_timer_args, &(iperf_instance->timers.tx_timer)), TAG_ID, "failed to create Tx timer");
    }

    return ESP_OK;
}

static esp_err_t iperf_start_timers(iperf_instance_data_t *iperf_instance)
{
    if (iperf_instance->timers.tx_timer) {
        ESP_RETURN_ON_ERROR(esp_timer_start_periodic(iperf_instance->timers.tx_timer, iperf_instance->timers.tx_period_us),
                            TAG_ID, "failed to start Tx timer");
    }
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(iperf_instance->timers.tick_timer, IPERF_TICK_US),
                        TAG_ID, "failed to start tick timer");
    return ESP_OK;
}

static esp_err_t iperf_stop_timers(iperf_instance_data_t *iperf_instance)
{
    esp_err_t ret = ESP_OK;
    if (iperf_instance->timers.tx_timer) {
        ret = esp_timer_stop_blocking(iperf_instance->timers.tx_timer, portMAX_DELAY);
        if (ret == ESP_ERR_INVALID_STATE && !esp_timer_is_active(iperf_instance->timers.tx_timer)) {
            ret = ESP_OK;
        }
    }
    if (iperf_instance->timers.tick_timer) {
        const esp_err_t tick_ret = esp_timer_stop_blocking(iperf_instance->timers.tick_timer, portMAX_DELAY);
        const esp_err_t normalized_tick_ret =
            tick_ret == ESP_ERR_INVALID_STATE && !esp_timer_is_active(iperf_instance->timers.tick_timer) ?
            ESP_OK : tick_ret;
        if (ret == ESP_OK) {
            ret = normalized_tick_ret;
        }
    }
    return ret;
}

static esp_err_t iperf_delete_timers(iperf_instance_data_t *iperf_instance)
{
    esp_err_t ret = ESP_OK;
    if (iperf_instance->timers.tx_timer) {
        ret = esp_timer_delete(iperf_instance->timers.tx_timer);
        if (ret == ESP_OK) {
            iperf_instance->timers.tx_timer = NULL;
        }
    }
    if (iperf_instance->timers.tick_timer) {
        const esp_err_t tick_ret = esp_timer_delete(iperf_instance->timers.tick_timer);
        if (ret == ESP_OK) {
            ret = tick_ret;
        }
        if (tick_ret == ESP_OK) {
            iperf_instance->timers.tick_timer = NULL;
        }
    }
    return ret;
}

static int iperf_show_socket_error_reason(iperf_instance_data_t *iperf_instance, const char *str)
{
    int err = errno;
    if (err != 0) {
        ESP_LOGW(TAG_ID, "%s error, error code: %d, reason: %s", str, err, strerror(err));
    }

    return err;
}

inline static bool iperf_is_udp_client(iperf_instance_data_t *iperf_instance)
{
    return ((iperf_instance->flags & IPERF_FLAG_CLIENT) && (iperf_instance->flags & IPERF_FLAG_UDP));
}

inline static bool iperf_is_udp_server(iperf_instance_data_t *iperf_instance)
{
    return ((iperf_instance->flags & IPERF_FLAG_SERVER) && (iperf_instance->flags & IPERF_FLAG_UDP));
}

inline static bool iperf_is_tcp_client(iperf_instance_data_t *iperf_instance)
{
    return ((iperf_instance->flags & IPERF_FLAG_CLIENT) && (iperf_instance->flags & IPERF_FLAG_TCP));
}

inline static bool iperf_is_tcp_server(iperf_instance_data_t *iperf_instance)
{
    return ((iperf_instance->flags & IPERF_FLAG_SERVER) && (iperf_instance->flags & IPERF_FLAG_TCP));
}

IRAM_ATTR static esp_err_t iperf_client_loop(iperf_instance_data_t *iperf_instance)
{
    esp_err_t ret = ESP_OK;
    uint32_t pkt_cnt = 0;
    const char* error_log = "sendto";
    int want_send = iperf_instance->socket_info.buffer_len;
    uint32_t* pkt_id_p = (uint32_t *) iperf_instance->socket_info.buffer;
    bool is_started = false;

    uint32_t pending_tx_count = 0;
    for (;;) {
        if (!atomic_load(&iperf_instance->is_running) || atomic_load(&iperf_instance->timers.expired)) {
            break;
        }
        if (iperf_instance->timers.tx_timer == NULL) {
            pending_tx_count = 1U;
        } else {
            pending_tx_count = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (pending_tx_count == 0U) {
                continue;
            }
        }
        if (pending_tx_count > IPERF_CLIENT_PENDING_TX_BURST) {
            pending_tx_count = IPERF_CLIENT_PENDING_TX_BURST;
        }
        while (pending_tx_count--) {
            *pkt_id_p = htonl(pkt_cnt++); /**< datagrams need to be sequentially numbered */
            const int actual_send = iperf_send_socket(iperf_instance,
                                                      iperf_instance->socket_info.buffer,
                                                      want_send);
            if (actual_send != want_send) {
                if (!atomic_load(&iperf_instance->is_running)) {
                    break;
                }
                if (iperf_instance->flags & IPERF_FLAG_UDP) {
                    /** ENOMEM & ENOBUFS is expected under heavy load => do not print it */
                    if ((errno != ENOMEM) && (errno != ENOBUFS)) {
                        iperf_show_socket_error_reason(iperf_instance, error_log);
                        ret = ESP_FAIL;
                        goto err;
                    }
                } else if (iperf_instance->flags & IPERF_FLAG_TCP) {
                    iperf_show_socket_error_reason(iperf_instance, error_log);
                    ret = ESP_FAIL;
                    goto err;
                }
            } else {
                atomic_fetch_add(&(iperf_instance->period_data_passed), actual_send);
                if (!is_started) {
                    iperf_state_action(IPERF_STARTED, iperf_instance);
                    is_started = true;
                }
            }
        }
        if (iperf_instance->timers.tx_timer == NULL) {
            vTaskDelay(1);
        }
    }
err:
    if (is_started) {
        iperf_state_action(IPERF_STOPPED, iperf_instance);
    }
    return ret;
}

IRAM_ATTR static esp_err_t iperf_server_loop(iperf_instance_data_t *iperf_instance)
{
    esp_err_t ret = ESP_OK;
    int want_recv = iperf_instance->socket_info.buffer_len;
    int actual_recv = 0;
    bool is_started = false;

    const char *error_log = "recvfrom";

    while (atomic_load(&iperf_instance->is_running) && !atomic_load(&iperf_instance->timers.expired)) {
        actual_recv = iperf_receive_socket(iperf_instance, iperf_instance->socket_info.buffer,
                                           want_recv);

        if (actual_recv < 0) {
            if (!atomic_load(&iperf_instance->is_running)) {
                break;
            }
            iperf_show_socket_error_reason(iperf_instance, error_log);
            ret = ESP_FAIL;
            goto err;
        }
        atomic_fetch_add(&(iperf_instance->period_data_passed), actual_recv);
        if (!is_started) {
            ESP_RETURN_ON_ERROR(iperf_start_timers(iperf_instance), TAG_ID, "failed to start internal timers");
            iperf_state_action(IPERF_STARTED, iperf_instance);
            is_started = true;
        }
    }
err:
    if (is_started) {
        iperf_state_action(IPERF_STOPPED, iperf_instance);
    }
    return ret;
}

static esp_err_t iperf_start_and_run_tcp_server(iperf_instance_data_t *iperf_instance)
{
    int opt = 1;
    esp_err_t ret = ESP_OK;
    struct timeval timeout = { 0 };
    socklen_t addr_len = sizeof(struct sockaddr);
    struct sockaddr_storage listen_addr = { 0 };
#if IPERF_IPV4_ENABLED
    struct sockaddr_in listen_addr4 = { 0 };
    struct sockaddr_in remote_addr = { 0 };
#endif
#if IPERF_IPV6_ENABLED
    struct sockaddr_in6 listen_addr6 = { 0 };
    struct sockaddr_in6 remote_addr6 = { 0 };
#endif
    if (iperf_instance->socket_info.source.type == ESP_IPADDR_TYPE_V6) {
#if IPERF_IPV6_ENABLED
        memcpy(&listen_addr6.sin6_addr, iperf_instance->socket_info.source.u_addr.ip6.addr, sizeof(listen_addr6.sin6_addr));
        listen_addr6.sin6_family = AF_INET6;
        listen_addr6.sin6_port = htons(iperf_instance->socket_info.sport);

        ESP_GOTO_ON_FALSE(iperf_create_socket(iperf_instance, &iperf_instance->listen_socket,
                                              AF_INET6, SOCK_STREAM, IPPROTO_IPV6) >= 0,
                          ESP_FAIL, err, TAG_ID, "cannot start TCP server: unable to create socket - errno %d", errno);
        ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->listen_socket,
                                                  SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)),
                          err, TAG_ID, "failed to set SO_REUSEADDR - errno %d", errno);
        ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->listen_socket,
                                                  IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt)),
                          err, TAG_ID, "failed to set IPV6_V6ONLY - errno %d", errno);
        ESP_GOTO_ON_ERROR(iperf_bind_socket(iperf_instance, &iperf_instance->listen_socket,
                                            (struct sockaddr *)&listen_addr6, sizeof(listen_addr6)),
                          err, TAG_ID, "cannot start TCP server: socket is unable to bind - errno %d, IPPROTO: %d", errno, AF_INET6);
        ESP_GOTO_ON_ERROR(iperf_listen_socket(iperf_instance, &iperf_instance->listen_socket, 0),
                          err, TAG_ID, "cannot start TCP server: an error occurred during listen - errno %d", errno);

        memcpy(&listen_addr, &listen_addr6, sizeof(listen_addr6));
#else
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG_ID, "cannot start TCP server: invalid address type");
#endif
    } else if (iperf_instance->socket_info.source.type == ESP_IPADDR_TYPE_V4) {
#if IPERF_IPV4_ENABLED
        listen_addr4.sin_family = AF_INET;
        listen_addr4.sin_port = htons(iperf_instance->socket_info.sport);
        listen_addr4.sin_addr.s_addr = iperf_instance->socket_info.source.u_addr.ip4.addr;

        ESP_GOTO_ON_FALSE(iperf_create_socket(iperf_instance, &iperf_instance->listen_socket,
                                              AF_INET, SOCK_STREAM, IPPROTO_TCP) >= 0,
                          ESP_FAIL, err, TAG_ID, "cannot start TCP server: unable to create socket - errno %d", errno);
        ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->listen_socket,
                                                  SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)),
                          err, TAG_ID, "failed to set SO_REUSEADDR - errno %d", errno);
        ESP_GOTO_ON_ERROR(iperf_bind_socket(iperf_instance, &iperf_instance->listen_socket,
                                            (struct sockaddr *)&listen_addr4, sizeof(listen_addr4)),
                          err, TAG_ID, "cannot start TCP server: socket is unable to bind - errno %d, IPPROTO: %d", errno, AF_INET);
        ESP_GOTO_ON_ERROR(iperf_listen_socket(iperf_instance, &iperf_instance->listen_socket, 0),
                          err, TAG_ID, "cannot start TCP server: an error occurred during listen - errno %d", errno);
        memcpy(&listen_addr, &listen_addr4, sizeof(listen_addr4));
#else
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG_ID, "cannot start TCP server: invalid address type");
#endif
    }
    ESP_LOGI(TAG_ID, "[TCP Server] Socket created");

    timeout.tv_sec = IPERF_SOCKET_ACCEPT_TIMEOUT;
    ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->listen_socket,
                                              SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)),
                      err, TAG_ID, "failed to set SO_RCVTIMEO - errno %d", errno);

    if (iperf_instance->socket_info.source.type == ESP_IPADDR_TYPE_V6) {
#if IPERF_IPV6_ENABLED
        addr_len = sizeof(remote_addr6);
        const int accepted_socket = iperf_accept_socket(iperf_instance,
                                                        (struct sockaddr *)&remote_addr6, &addr_len);
        ESP_GOTO_ON_FALSE(accepted_socket >= 0, ESP_FAIL, err, TAG_ID,
                          "cannot start TCP server: socket is unable to accept connection - errno %d", errno);
        char addr_str[INET6_ADDRSTRLEN];
        inet6_ntoa_r(remote_addr6.sin6_addr, addr_str, sizeof(addr_str));
        ESP_LOGD(TAG_ID, "accept: [%s]:%d", addr_str, htons(remote_addr6.sin6_port));
#endif
    } else if (iperf_instance->socket_info.source.type == ESP_IPADDR_TYPE_V4) {
#if IPERF_IPV4_ENABLED
        addr_len = sizeof(remote_addr);
        const int accepted_socket = iperf_accept_socket(iperf_instance,
                                                        (struct sockaddr *)&remote_addr, &addr_len);
        ESP_GOTO_ON_FALSE(accepted_socket >= 0, ESP_FAIL, err, TAG_ID,
                          "cannot start TCP server: socket is unable to accept connection - errno %d", errno);
        char addr_str[INET_ADDRSTRLEN];
        inet_ntoa_r(remote_addr.sin_addr, addr_str, sizeof(addr_str));
        ESP_LOGD(TAG_ID, "accept: %s:%d", addr_str, htons(remote_addr.sin_port));
#endif
    }

    timeout.tv_sec = IPERF_SOCKET_RX_TIMEOUT;
    ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->socket,
                                              SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)),
                      err, TAG_ID, "failed to set SO_RCVTIMEO - errno %d", errno);
    ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->socket,
                                              IPPROTO_IP, IP_TOS, &(iperf_instance->socket_info.tos),
                                              sizeof(iperf_instance->socket_info.tos)),
                      err, TAG_ID, "failed to set IP_TOS - errno %d", errno);
    ESP_GOTO_ON_ERROR(iperf_publish_target_addr(iperf_instance, (struct sockaddr *)&listen_addr,
                                                iperf_instance->socket_info.source.type == ESP_IPADDR_TYPE_V6 ?
                                                sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in)),
                      err, TAG_ID, "failed to publish TCP server target address");

    /** if loop finished prematurely due to error */
    ret = iperf_server_loop(iperf_instance);
    if (ret != ESP_OK) {
        goto err;
    }
err:
    return ret;
}

static esp_err_t iperf_start_and_run_tcp_client(iperf_instance_data_t *iperf_instance)
{
    esp_err_t ret = ESP_OK;
    struct timeval timeout = { 0 };
#if IPERF_IPV4_ENABLED
    struct sockaddr_in dest_addr4 = { 0 };
#endif
#if IPERF_IPV6_ENABLED
    struct sockaddr_in6 dest_addr6 = { 0 };
#endif
    if (iperf_instance->socket_info.destination.type == ESP_IPADDR_TYPE_V6) {
#if IPERF_IPV6_ENABLED
        const int socket_fd = iperf_create_socket(iperf_instance, &iperf_instance->socket, AF_INET6, SOCK_STREAM, IPPROTO_IPV6);
        ESP_GOTO_ON_FALSE(socket_fd >= 0, ESP_FAIL, err, TAG_ID, "cannot start TCP client: unable to create socket - errno %d", errno);

        memcpy(&dest_addr6.sin6_addr, iperf_instance->socket_info.destination.u_addr.ip6.addr, 16); /**< ipv6 address is 16 bytes */
        dest_addr6.sin6_family = AF_INET6;
        dest_addr6.sin6_port = htons(iperf_instance->socket_info.dport);

        if (!ip6_addr_isany(&iperf_instance->socket_info.source.u_addr.ip6) || iperf_instance->socket_info.sport) {
            struct sockaddr_in6 src_addr6 = { 0 };
            src_addr6.sin6_family = AF_INET6;
            src_addr6.sin6_port = htons(iperf_instance->socket_info.sport);
            memcpy(&src_addr6.sin6_addr, iperf_instance->socket_info.source.u_addr.ip6.addr, 16);
            ESP_GOTO_ON_ERROR(iperf_bind_socket(iperf_instance, &iperf_instance->socket, (struct sockaddr *)&src_addr6, sizeof(src_addr6)),
                              err, TAG_ID, "cannot start TCP client: unable to bind socket - errno %d", errno);
        }

        ESP_GOTO_ON_ERROR(iperf_connect_with_timeout(iperf_instance, (struct sockaddr *)&dest_addr6,
                                                     sizeof(struct sockaddr_in6)), err, TAG_ID,
                          "cannot start TCP client: socket is unable to connect - errno %d", errno);
        ESP_LOGD(TAG_ID, "Successfully connected");
        ESP_GOTO_ON_ERROR(iperf_publish_target_addr(iperf_instance,
                                                    (struct sockaddr *)&dest_addr6,
                                                    sizeof(dest_addr6)),
                          err, TAG_ID, "failed to publish TCP client target address");
#else
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG_ID, "cannot start TCP client: invalid address type");
#endif
    } else if (iperf_instance->socket_info.destination.type == ESP_IPADDR_TYPE_V4) {
#if IPERF_IPV4_ENABLED
        const int socket_fd = iperf_create_socket(iperf_instance, &iperf_instance->socket, AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ESP_GOTO_ON_FALSE(socket_fd >= 0, ESP_FAIL, err, TAG_ID, "cannot start TCP client: unable to create socket - errno %d", errno);

        dest_addr4.sin_family = AF_INET;
        dest_addr4.sin_port = htons(iperf_instance->socket_info.dport);
        dest_addr4.sin_addr.s_addr = iperf_instance->socket_info.destination.u_addr.ip4.addr;

        if (!ip4_addr_isany(&iperf_instance->socket_info.source.u_addr.ip4) || iperf_instance->socket_info.sport) {
            struct sockaddr_in src_addr = { 0 };
            src_addr.sin_family = AF_INET;
            src_addr.sin_port = htons(iperf_instance->socket_info.sport);
            src_addr.sin_addr.s_addr = iperf_instance->socket_info.source.u_addr.ip4.addr;
            ESP_GOTO_ON_ERROR(iperf_bind_socket(iperf_instance, &iperf_instance->socket, (struct sockaddr *)&src_addr, sizeof(src_addr)),
                              err, TAG_ID, "cannot start TCP client: unable to bind socket - errno %d", errno);
        }

        ESP_GOTO_ON_ERROR(iperf_connect_with_timeout(iperf_instance, (struct sockaddr *)&dest_addr4,
                                                     sizeof(struct sockaddr_in)), err, TAG_ID,
                          "cannot start TCP client: socket is unable to connect - errno %d", errno);
        ESP_LOGD(TAG_ID, "Successfully connected");
        ESP_GOTO_ON_ERROR(iperf_publish_target_addr(iperf_instance,
                                                    (struct sockaddr *)&dest_addr4,
                                                    sizeof(dest_addr4)),
                          err, TAG_ID, "failed to publish TCP client target address");
#else
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG_ID, "cannot start TCP client: invalid address type");
#endif
    }
    timeout.tv_sec = IPERF_SOCKET_TCP_TX_TIMEOUT;
    ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)),
                      err, TAG_ID, "failed to set IPERF_SOCKET_TCP_TX_TIMEOUT - errno %d", errno);
    ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->socket, IPPROTO_IP, IP_TOS,
                                              &(iperf_instance->socket_info.tos), sizeof(iperf_instance->socket_info.tos)),
                      err, TAG_ID, "failed to set IP_TOS - errno %d", errno);

    ESP_GOTO_ON_ERROR(iperf_start_timers(iperf_instance), err, TAG_ID, "failed to start internal timers");
    /** if loop finished prematurely due to error */
    ret = iperf_client_loop(iperf_instance);
    if (ret != ESP_OK) {
        goto err;
    }
err:
    return ret;
}

static esp_err_t iperf_start_and_run_udp_server(iperf_instance_data_t *iperf_instance)
{
    int opt = 1;
    esp_err_t ret = ESP_OK;
    struct timeval timeout = { 0 };
    struct sockaddr_storage listen_addr = { 0 };
#if IPERF_IPV4_ENABLED
    struct sockaddr_in listen_addr4 = { 0 };
#endif
#if IPERF_IPV6_ENABLED
    struct sockaddr_in6 listen_addr6 = { 0 };
#endif
    if (iperf_instance->socket_info.source.type == ESP_IPADDR_TYPE_V6) {
#if IPERF_IPV6_ENABLED
        memcpy(&listen_addr6.sin6_addr, iperf_instance->socket_info.source.u_addr.ip6.addr, sizeof(listen_addr6.sin6_addr));
        listen_addr6.sin6_family = AF_INET6;
        listen_addr6.sin6_port = htons(iperf_instance->socket_info.sport);

        ESP_GOTO_ON_FALSE(iperf_create_socket(iperf_instance, &iperf_instance->socket,
                                              AF_INET6, SOCK_DGRAM, IPPROTO_UDP) >= 0,
                          ESP_FAIL, err, TAG_ID, "cannot start UDP server: unable to create socket - errno %d", errno);
        ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->socket,
                                                  SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)),
                          err, TAG_ID, "failed to set SO_REUSEADDR - errno %d", errno);
        ESP_GOTO_ON_ERROR(iperf_bind_socket(iperf_instance, &iperf_instance->socket,
                                            (struct sockaddr *)&listen_addr6, sizeof(listen_addr6)),
                          err, TAG_ID, "cannot start UDP server: socket is unable to bind - errno %d", errno);
        char addr_str[INET6_ADDRSTRLEN];
        inet6_ntoa_r(listen_addr6.sin6_addr, addr_str, sizeof(addr_str));
        ESP_LOGD(TAG_ID, "UDP server socket bound, [%s]:%" PRIu16, addr_str, ntohs(listen_addr6.sin6_port));

        memcpy(&listen_addr, &listen_addr6, sizeof(listen_addr6));
#else
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG_ID, "cannot start UDP server: invalid address type");
#endif
    } else if (iperf_instance->socket_info.source.type == ESP_IPADDR_TYPE_V4) {
#if IPERF_IPV4_ENABLED
        listen_addr4.sin_family = AF_INET;
        listen_addr4.sin_port = htons(iperf_instance->socket_info.sport);
        listen_addr4.sin_addr.s_addr = iperf_instance->socket_info.source.u_addr.ip4.addr;
        ESP_GOTO_ON_FALSE(iperf_create_socket(iperf_instance, &iperf_instance->socket,
                                              AF_INET, SOCK_DGRAM, IPPROTO_UDP) >= 0,
                          ESP_FAIL, err, TAG_ID, "cannot start UDP server: unable to create socket - errno %d", errno);
        ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->socket,
                                                  SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)),
                          err, TAG_ID, "failed to set SO_REUSEADDR - errno %d", errno);
        ESP_GOTO_ON_ERROR(iperf_bind_socket(iperf_instance, &iperf_instance->socket,
                                            (struct sockaddr *)&listen_addr4, sizeof(listen_addr4)),
                          err, TAG_ID, "cannot start UDP server: socket is unable to bind - errno %d", errno);
        char addr_str[INET_ADDRSTRLEN];
        inet_ntoa_r(listen_addr4.sin_addr, addr_str, sizeof(addr_str));
        ESP_LOGD(TAG_ID, "UDP server socket bound, %s:%" PRIu16, addr_str, ntohs(listen_addr4.sin_port));
        memcpy(&listen_addr, &listen_addr4, sizeof(listen_addr4));
#else
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG_ID, "cannot start UDP server: invalid address type");
#endif
    }
    ESP_LOGI(TAG_ID, "[UDP Server] Socket created");

    timeout.tv_sec = IPERF_SOCKET_RX_TIMEOUT;
    ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->socket,
                                              SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)),
                      err, TAG_ID, "failed to set SO_RCVTIMEO - errno %d", errno);
    ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->socket,
                                              IPPROTO_IP, IP_TOS, &(iperf_instance->socket_info.tos),
                                              sizeof(iperf_instance->socket_info.tos)),
                      err, TAG_ID, "failed to set IP_TOS - errno %d", errno);
    ESP_GOTO_ON_ERROR(iperf_publish_target_addr(iperf_instance, (struct sockaddr *)&listen_addr,
                                                iperf_instance->socket_info.source.type == ESP_IPADDR_TYPE_V6 ?
                                                sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in)),
                      err, TAG_ID, "failed to publish UDP server target address");

    /** if loop finished prematurely due to error */
    ret = iperf_server_loop(iperf_instance);
    if (ret != ESP_OK) {
        goto err;
    }
err:
    return ret;
}

static esp_err_t iperf_start_and_run_udp_client(iperf_instance_data_t *iperf_instance)
{
    int opt = 1;
    esp_err_t ret = ESP_OK;
#if IPERF_IPV4_ENABLED
    struct sockaddr_in dest_addr4 = { 0 };
#endif
#if IPERF_IPV6_ENABLED
    struct sockaddr_in6 dest_addr6 = { 0 };
#endif

    if (iperf_instance->socket_info.destination.type == ESP_IPADDR_TYPE_V6) {
#if IPERF_IPV6_ENABLED
        memcpy(&dest_addr6.sin6_addr, iperf_instance->socket_info.destination.u_addr.ip6.addr, 16); /**< ipv6 address is 16 bytes */
        dest_addr6.sin6_family = AF_INET6;
        dest_addr6.sin6_port = htons(iperf_instance->socket_info.dport);

        ESP_GOTO_ON_FALSE(iperf_create_socket(iperf_instance, &iperf_instance->socket,
                                              AF_INET6, SOCK_DGRAM, IPPROTO_IPV6) >= 0,
                          ESP_FAIL, err, TAG_ID, "cannot start UDP client: unable to create socket - errno %d", errno);
        ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->socket,
                                                  SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)),
                          err, TAG_ID, "failed to set SO_REUSEADDR - errno %d", errno);
        ESP_GOTO_ON_ERROR(iperf_publish_target_addr(iperf_instance, (struct sockaddr *)&dest_addr6,
                                                    sizeof(dest_addr6)),
                          err, TAG_ID, "failed to publish UDP client target address");

        if (!ip6_addr_isany(&iperf_instance->socket_info.source.u_addr.ip6) || iperf_instance->socket_info.sport) {
            struct sockaddr_in6 src_addr6 = { 0 };
            src_addr6.sin6_family = AF_INET6;
            src_addr6.sin6_port = htons(iperf_instance->socket_info.sport);
            memcpy(&src_addr6.sin6_addr, iperf_instance->socket_info.source.u_addr.ip6.addr, 16);

            ESP_GOTO_ON_ERROR(iperf_bind_socket(iperf_instance, &iperf_instance->socket,
                                                (struct sockaddr *)&src_addr6, sizeof(src_addr6)),
                              err, TAG_ID, "cannot start UDP client: unable to bind socket - errno %d", errno);
            char addr_str[INET6_ADDRSTRLEN];
            inet6_ntoa_r(src_addr6.sin6_addr, addr_str, sizeof(addr_str));
            ESP_LOGD(TAG_ID, "UDP client socket bound [%s]:%d", addr_str, iperf_instance->socket_info.sport);
        }
        ESP_LOGD(TAG_ID, "[UDP Client] Socket created, sending to [" IPV6STR "]:%d", IPV62STR(iperf_instance->socket_info.destination.u_addr.ip6), iperf_instance->socket_info.dport);
#else
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG_ID, "cannot start UDP client: invalid address type");
#endif
    } else if (iperf_instance->socket_info.destination.type == ESP_IPADDR_TYPE_V4) {
#if IPERF_IPV4_ENABLED
        dest_addr4.sin_family = AF_INET;
        dest_addr4.sin_port = htons(iperf_instance->socket_info.dport);
        dest_addr4.sin_addr.s_addr = iperf_instance->socket_info.destination.u_addr.ip4.addr;

        ESP_GOTO_ON_FALSE(iperf_create_socket(iperf_instance, &iperf_instance->socket,
                                              AF_INET, SOCK_DGRAM, IPPROTO_UDP) >= 0,
                          ESP_FAIL, err, TAG_ID, "cannot start UDP client: unable to create socket - errno %d", errno);
        ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->socket,
                                                  SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)),
                          err, TAG_ID, "failed to set SO_REUSEADDR - errno %d", errno);
        ESP_GOTO_ON_ERROR(iperf_publish_target_addr(iperf_instance, (struct sockaddr *)&dest_addr4,
                                                    sizeof(dest_addr4)),
                          err, TAG_ID, "failed to publish UDP client target address");

        if (!ip4_addr_isany(&iperf_instance->socket_info.source.u_addr.ip4) || iperf_instance->socket_info.sport) {
            struct sockaddr_in src_addr = { 0 };
            src_addr.sin_family = AF_INET;
            src_addr.sin_port = htons(iperf_instance->socket_info.sport);
            src_addr.sin_addr.s_addr = iperf_instance->socket_info.source.u_addr.ip4.addr;

            ESP_GOTO_ON_ERROR(iperf_bind_socket(iperf_instance, &iperf_instance->socket,
                                                (struct sockaddr *)&src_addr, sizeof(src_addr)),
                              err, TAG_ID, "cannot start UDP client: unable to bind socket - errno %d", errno);
            char addr_str[INET_ADDRSTRLEN];
            inet_ntoa_r(src_addr.sin_addr, addr_str, sizeof(addr_str));
            ESP_LOGD(TAG_ID, "UDP client socket bound, %s:%d", addr_str, ntohs(src_addr.sin_port));
        }
        ESP_LOGD(TAG_ID, "[UDP Client] Socket created, sending to " IPSTR ":%d", IP2STR(&iperf_instance->socket_info.destination.u_addr.ip4), iperf_instance->socket_info.dport);
#else
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG_ID, "cannot start UDP client: invalid address type");
#endif
    }

    ESP_GOTO_ON_ERROR(iperf_set_socket_option(iperf_instance, &iperf_instance->socket,
                                              IPPROTO_IP, IP_TOS, &(iperf_instance->socket_info.tos),
                                              sizeof(iperf_instance->socket_info.tos)),
                      err, TAG_ID, "failed to set IP_TOS - errno %d", errno);

    ESP_GOTO_ON_ERROR(iperf_start_timers(iperf_instance), err, TAG_ID, "failed to start internal timers");
    /** if loop finished prematurely due to error */
    ret = iperf_client_loop(iperf_instance);
    if (ret != ESP_OK) {
        goto err;
    }
err:
    return ret;
}

static void iperf_traffic_task(void *arg)
{
    iperf_instance_data_t *iperf_instance = (iperf_instance_data_t *)arg;
    char tag[sizeof(iperf_instance->tag)];
    const iperf_id_t instance_id = iperf_instance->id;
    esp_err_t ret = ESP_OK;
    memcpy(tag, iperf_instance->tag, sizeof(tag));
    if (!atomic_load(&iperf_instance->is_running)) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (iperf_is_tcp_server(iperf_instance)) {
        ret = iperf_start_and_run_tcp_server(iperf_instance);
    } else if (iperf_is_tcp_client(iperf_instance)) {
        ret = iperf_start_and_run_tcp_client(iperf_instance);
    } else if (iperf_is_udp_server(iperf_instance)) {
        ret = iperf_start_and_run_udp_server(iperf_instance);
    } else if (iperf_is_udp_client(iperf_instance)) {
        ret = iperf_start_and_run_udp_client(iperf_instance);
    } else {
        ret = ESP_ERR_INVALID_ARG;
        ESP_LOGE(TAG_ID, "cannot start iperf-related tasks: invalid mode (TCP/UDP) or role (client/server)");
    }

    /** The traffic task owns blocking stop and final instance reclamation. */
    const esp_err_t stop_ret = iperf_stop_exec(iperf_instance);
    iperf_record_error(&ret, stop_ret);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_ID, "traffic task for id=%" PRIi8 " completed with error: %s",
                 instance_id, esp_err_to_name(ret));
    }
    while (iperf_delete_instance(iperf_instance) != ESP_OK) {
        /** Retain ownership until timers, report acknowledgement and CLOSED publication complete. */
        vTaskDelay(1);
    }
    ESP_LOGD(tag, "traffic task for id=%" PRIi8 " is to be deleted", instance_id);
    vTaskDelete(NULL);
}

static esp_err_t iperf_request_stop(iperf_instance_data_t *iperf_instance)
{
    if (iperf_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_store(&iperf_instance->is_running, false);
    if (iperf_instance->traffic_task_hdl != NULL) {
        xTaskNotifyGive(iperf_instance->traffic_task_hdl);
    }
    if (iperf_instance->report_task_hdl != NULL) {
        xTaskNotifyGive(iperf_instance->report_task_hdl);
    }
    return ESP_OK;
}

static esp_err_t iperf_stop_exec(iperf_instance_data_t *iperf_instance)
{
    /** prevent further exec of while loops and wake the owner */
    const esp_err_t request_ret = iperf_request_stop(iperf_instance);

    /** Stop timers before reclamation; retain their handles for retry if quiescence is unconfirmed. */

    esp_err_t ret = request_ret;
    iperf_record_error(&ret, iperf_stop_timers(iperf_instance));

    /** release possibly blocking client loop */
    if (iperf_instance->flags & IPERF_FLAG_CLIENT) {
        if (iperf_instance->traffic_task_hdl) {
            xTaskNotifyGive(iperf_instance->traffic_task_hdl);
        }
    }
    /** release possibly blocking server/client socket loops */
    const esp_err_t listen_ret = iperf_close_socket(iperf_instance, &iperf_instance->listen_socket);
    const esp_err_t socket_ret = iperf_close_socket(iperf_instance, &iperf_instance->socket);
    if (ret == ESP_OK && listen_ret != ESP_OK) {
        ret = listen_ret;
    }
    if (ret == ESP_OK && socket_ret != ESP_OK) {
        ret = socket_ret;
    }

    /** release report task */
    if (iperf_instance->report_task_hdl) {
        xTaskNotifyGive(iperf_instance->report_task_hdl);
    }

    return ret;
}

static esp_err_t iperf_start_tasks(iperf_instance_data_t *iperf_instance, const iperf_cfg_t *cfg)
{
    esp_err_t ret = ESP_OK;
    BaseType_t xResult = pdPASS;

    /** Create tasks in cleanup dependency order. */
    /** Start report task */
    iperf_instance->report_task_hdl = iperf_create_report_task(iperf_instance);
    ESP_GOTO_ON_FALSE(iperf_instance->report_task_hdl != NULL, ESP_FAIL, err_report, TAG_ID, "cannot start iperf-related tasks: could not start report task");

    /** Set task priority to default if not specified */
    uint8_t traffic_task_priority = IPERF_DEFAULT_TRAFFIC_TASK_PRIORITY;
    if (cfg->traffic_task_priority != 0) {
        traffic_task_priority = cfg->traffic_task_priority;
    }
    iperf_instance->is_running = true;
    xResult = xTaskCreatePinnedToCore(iperf_traffic_task, IPERF_TRAFFIC_TASK_NAME, IPERF_TRAFFIC_TASK_STACK,
                                      (void*)iperf_instance, traffic_task_priority, &(iperf_instance->traffic_task_hdl),
                                      NUMBER_OF_CORES - 1);
    ESP_GOTO_ON_FALSE(xResult == pdPASS, ESP_FAIL, err_traffic, TAG_ID, "cannot start iperf-related tasks: could not start traffic task");
    return ESP_OK;
err_traffic:
    iperf_stop_exec(iperf_instance);
err_report:
    return ret;
}

static int iperf_esp_err_to_errno(esp_err_t esp_err)
{
    switch (esp_err) {
    case ESP_ERR_NO_MEM:
        return ENOBUFS;
    case ESP_ERR_INVALID_ARG:
        return EINVAL;
    case ESP_ERR_TIMEOUT:
        return EBUSY;
    case ESP_ERR_INVALID_STATE:
        return ENODEV;
    }
    return EIO;
}

static esp_err_t iperf_list_exec_for_each(iperf_list_exec_fn fn, void *ctx)
{
    esp_err_t ret = ESP_OK;
    if (xSemaphoreTake(s_list_lock, IPERF_LIST_LOCK_TMO_RTOS_TICKS) == pdTRUE) {
        iperf_instance_data_t *instance;
        LIST_FOREACH(instance, p_running_iperfs_list, _list_entry) {
            const esp_err_t instance_ret = fn(instance, ctx);
            iperf_record_error(&ret, instance_ret);
        }
        xSemaphoreGive(s_list_lock);
    } else {
        ret = ESP_ERR_TIMEOUT;
    }
    return ret;
}

static esp_err_t iperf_list_exec_by_id(iperf_id_t id, iperf_list_exec_fn fn, void *ctx)
{
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_list_lock, IPERF_LIST_LOCK_TMO_RTOS_TICKS) == pdTRUE) {
        iperf_instance_data_t *instance;
        LIST_FOREACH(instance, p_running_iperfs_list, _list_entry) {
            if (instance->id == id) {
                ret = fn(instance, ctx);
                break;
            }
        }
        xSemaphoreGive(s_list_lock);
    } else {
        ret = ESP_ERR_TIMEOUT;
    }
    return ret;
}

static iperf_id_t iperf_list_get_next_available_id_unsafe(void)
{
    iperf_id_t smallest_id = 0;
    iperf_instance_data_t *instance;

    LIST_FOREACH(instance, p_running_iperfs_list, _list_entry) {
        if (instance->id > smallest_id) {
            smallest_id = instance->id;
        }
    }
    return smallest_id + 1;
}

static bool iperf_list_check_id_available_unsafe(iperf_id_t id)
{
    iperf_instance_data_t *instance;
    LIST_FOREACH(instance, p_running_iperfs_list, _list_entry) {
        if (instance->id == id) {
            return false;
        }
    }
    return true;
}

iperf_instance_data_t* iperf_list_get_instance_by_id(iperf_id_t id)
{
    iperf_instance_data_t *result = NULL;
    iperf_instance_data_t *instance;
    if (xSemaphoreTake(s_list_lock, IPERF_LIST_LOCK_TMO_RTOS_TICKS) == pdTRUE) {
        LIST_FOREACH(instance, p_running_iperfs_list, _list_entry) {
            if (instance->id == id) {
                result = instance;
                break;
            }
        }
        xSemaphoreGive(s_list_lock);
    }
    return result;
}

static esp_err_t iperf_list_add_instance(iperf_instance_data_t *iperf_instance, iperf_id_t id)
{
    esp_err_t ret = ESP_OK;
    if (xSemaphoreTake(s_list_lock, IPERF_LIST_LOCK_TMO_RTOS_TICKS) == pdTRUE) {
        if (id <= 0) {
            /** find current biggest ID and increase by one */
            id = iperf_list_get_next_available_id_unsafe();
            if (id < 0) {
                ESP_LOGE(TAG, "failed to get next available iperf instance id");
                ret = ESP_FAIL;
            }
        } else {
            /** Bind to given ID */
            if (iperf_list_check_id_available_unsafe(id) == false) {
                ESP_LOGE(TAG, "iperf instance id %d already in use.", id);
                ret = ESP_FAIL;
            }
        }
        /** unlock iperf instance list */
        if (ret != ESP_OK) {
            xSemaphoreGive(s_list_lock);
            goto err;
        }

        iperf_instance->id = id;
        LIST_INSERT_HEAD(p_running_iperfs_list, iperf_instance, _list_entry);
        xSemaphoreGive(s_list_lock);
    } else {
        ret = ESP_ERR_TIMEOUT;
    }
err:
    return ret;
}

static esp_err_t iperf_list_remove_instance(iperf_instance_data_t *iperf_instance)
{
    esp_err_t ret = ESP_OK;
    if (xSemaphoreTake(s_list_lock, IPERF_LIST_LOCK_TMO_RTOS_TICKS) == pdTRUE) {
        LIST_REMOVE(iperf_instance, _list_entry);
        xSemaphoreGive(s_list_lock);
    } else {
        ret = ESP_ERR_TIMEOUT;
    }
    return ret;
}

static uint32_t iperf_get_buffer_len(iperf_instance_data_t *iperf_instance, uint16_t data_len)
{
    if (iperf_is_udp_client(iperf_instance)) {
#if IPERF_IPV6_ENABLED
        if (data_len) {
            return data_len;
        } else if (iperf_instance->socket_info.destination.type == ESP_IPADDR_TYPE_V6) {
            return IPERF_DEFAULT_IPV6_UDP_TX_LEN;
        } else {
            return IPERF_DEFAULT_IPV4_UDP_TX_LEN;
        }
#else
        return (data_len == 0 ? IPERF_DEFAULT_IPV4_UDP_TX_LEN : data_len);
#endif
    } else if (iperf_is_udp_server(iperf_instance)) {
        return IPERF_DEFAULT_UDP_RX_LEN;
    } else if (iperf_is_tcp_client(iperf_instance)) {
        return (data_len == 0 ? IPERF_DEFAULT_TCP_TX_LEN : data_len);
    } else {
        return IPERF_DEFAULT_TCP_RX_LEN;
    }
    return 0;
}

static esp_err_t iperf_force_stop(iperf_instance_data_t *iperf_instance, void *ctx)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(iperf_instance != NULL, ESP_FAIL, err, TAG, "cannot stop instance: instance not found");
    ESP_LOGI(TAG, "waiting for iperf instance id=%" PRIi8 " to stop...", iperf_instance->id);
    iperf_instance->time = IPERF_TIME_FORCE_ELAPSED;
    ret = iperf_request_stop(iperf_instance);
err:
    return ret;
}

static esp_err_t iperf_delete_instance(iperf_instance_data_t *iperf_instance)
{
    if (iperf_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char tag[sizeof(iperf_instance->tag)];
    memcpy(tag, iperf_instance->tag, sizeof(tag));
    if (iperf_stop_exec(iperf_instance) != ESP_OK) {
        ESP_LOGE(tag, "cannot reclaim iperf instance until timers are quiescent");
        return ESP_ERR_INVALID_STATE;
    }

    /** Clear the report-task handle only after its final instance-access acknowledgement. */
    if (iperf_instance->report_task_hdl != NULL) {
        if (iperf_instance->report_task_exit_sem == NULL ||
            xSemaphoreTake(iperf_instance->report_task_exit_sem, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(tag, "cannot reclaim iperf instance until report task exit is acknowledged");
            return ESP_ERR_TIMEOUT;
        }
        iperf_instance->report_task_hdl = NULL;
    }

    ESP_LOGD(tag, "deleting iperf instance");
    if (iperf_delete_timers(iperf_instance) != ESP_OK) {
        ESP_LOGE(tag, "cannot reclaim iperf instance until timer deletion completes");
        return ESP_ERR_INVALID_STATE;
    }
    iperf_state_action(IPERF_CLOSED, iperf_instance);
    if (iperf_instance->list_registered) {
        const esp_err_t remove_ret = iperf_list_remove_instance(iperf_instance);
        if (remove_ret != ESP_OK) {
            ESP_LOGE(tag, "cannot reclaim iperf instance until list removal completes");
            return remove_ret;
        }
        iperf_instance->list_registered = false;
    }

    if (iperf_instance->report_task_exit_sem != NULL) {
        vSemaphoreDelete(iperf_instance->report_task_exit_sem);
        iperf_instance->report_task_exit_sem = NULL;
    }
    free(iperf_instance->socket_info.buffer);
    free(iperf_instance);
    return ESP_OK;
}

static iperf_traffic_type_t iperf_get_traffic_type_internal(iperf_instance_data_t *iperf_instance)
{

    __attribute__((unused)) esp_err_t ret;

    ESP_GOTO_ON_ERROR(iperf_instance == NULL, err, TAG, "invalid argument");
    if (iperf_is_tcp_server(iperf_instance)) {
        return IPERF_TCP_SERVER;
    } else if (iperf_is_tcp_client(iperf_instance)) {
        return IPERF_TCP_CLIENT;
    }  else if (iperf_is_udp_server(iperf_instance)) {
        return IPERF_UDP_SERVER;
    }  else if (iperf_is_udp_client(iperf_instance)) {
        return IPERF_UDP_CLIENT;
    } else {
        ESP_LOGE(TAG, "can not get iperf traffic type, id=%d", iperf_instance->id);
    }
err:
    return IPERF_TRAFFIC_TYPE_INVALID;
}

iperf_id_t iperf_start_instance(const iperf_cfg_t *cfg)
{
    esp_err_t ret = ESP_OK;
    iperf_instance_data_t *iperf_instance = NULL;

    ESP_GOTO_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, err, TAG, "cannot create iperf instance: no config provided");
    ESP_GOTO_ON_FALSE(cfg->tos >= 0 && cfg->tos <= 255, ESP_ERR_INVALID_ARG, err, TAG, "Invalid TOS value, should be in range 0x00~0xFF");

    ESP_GOTO_ON_FALSE(iperf_ensure_list_lock(), ESP_FAIL, err, TAG, "failed to create static mutex");

    /** allocate iperf control structure */
    iperf_instance = heap_caps_calloc(1, sizeof(iperf_instance_data_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_GOTO_ON_FALSE(iperf_instance, ESP_ERR_NO_MEM, err, TAG, "cannot create iperf instance: not enough memory");
    atomic_store(&iperf_instance->socket, -1);
    atomic_store(&iperf_instance->listen_socket, -1);
    iperf_instance->socket_lock = xSemaphoreCreateMutexStatic(&iperf_instance->socket_lock_storage);
    ESP_GOTO_ON_FALSE(iperf_instance->socket_lock != NULL, ESP_ERR_NO_MEM, err, TAG,
                      "cannot create iperf instance socket lock");
    iperf_instance->report_task_exit_sem = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(iperf_instance->report_task_exit_sem != NULL, ESP_ERR_NO_MEM, err, TAG,
                      "cannot create iperf instance: not enough memory for task exit semaphore");

    /** Add pointer to the array of started iperfs */
    ESP_GOTO_ON_ERROR(iperf_list_add_instance(iperf_instance, cfg->instance_id), err, TAG, "cannot add new instance to instances list");
    iperf_instance->list_registered = true;

    /** create instance specific tag used in logs */
    snprintf(iperf_instance->tag, sizeof(iperf_instance->tag), TAG_ID_STR, iperf_instance->id);

    iperf_instance->traffic.output_format = cfg->format;
    iperf_instance->time = cfg->time;
    iperf_instance->interval = cfg->interval;
    iperf_instance->flags = cfg->flag;
    if (cfg->state_handler != NULL) {
        iperf_instance->state_handler = cfg->state_handler;
        iperf_instance->state_handler_priv = cfg->state_handler_priv;
    }

    /** populate config information about socket */
    iperf_instance->socket_info.destination = cfg->destination;
    iperf_instance->socket_info.source = cfg->source;
    iperf_instance->socket_info.dport = cfg->dport;
    iperf_instance->socket_info.sport = cfg->sport;
    iperf_instance->socket_info.tos = cfg->tos;
    iperf_instance->socket_info.buffer_len = iperf_get_buffer_len(iperf_instance, cfg->len_send_buf);
    iperf_instance->socket_info.buffer = (uint8_t *) calloc(iperf_instance->socket_info.buffer_len, sizeof(uint8_t));
    ESP_GOTO_ON_FALSE(iperf_instance->socket_info.buffer, ESP_ERR_NO_MEM, err, TAG_ID, "cannot create iperf instance: not enough memory for buffer allocation");
    /** calculate timer period or set default */
    if (cfg->bw_lim > 0) {
        iperf_instance->timers.tx_period_us = (uint64_t)iperf_instance->socket_info.buffer_len * 8 * 1000 * 1000 / cfg->bw_lim;
    }
    ESP_GOTO_ON_ERROR(iperf_create_timers(iperf_instance), err, TAG_ID, "failed to create timers");

    /** Start tasks associated with iperf */
    ESP_GOTO_ON_ERROR(iperf_start_tasks(iperf_instance, cfg), err, TAG_ID, "cannot start iperf-related tasks: an error has occurred");

    /** return the id assigned to this instance */
    return iperf_instance->id;
err:
    while (iperf_delete_instance(iperf_instance) != ESP_OK) {
        vTaskDelay(1);
    }
    errno = iperf_esp_err_to_errno(ret);
    return -1;
}

esp_err_t iperf_stop_instance(iperf_id_t id)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(id > 0 || id == IPERF_ALL_INSTANCES_ID, ESP_ERR_INVALID_ARG, err, TAG, "cannot stop instance: invalid id provided (id=%" PRIi8 ")", id);

    /** if user called stop prior start */
    ESP_GOTO_ON_FALSE(iperf_ensure_list_lock(), ESP_FAIL, err, TAG, "failed to create static mutex");

    if (id == IPERF_ALL_INSTANCES_ID) {
        return iperf_list_exec_for_each(iperf_force_stop, NULL);
    }
    return iperf_list_exec_by_id(id, iperf_force_stop, NULL);
err:
    return ret;
}
