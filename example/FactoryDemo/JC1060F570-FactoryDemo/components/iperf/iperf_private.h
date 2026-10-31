/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdatomic.h>
#include <inttypes.h>
#include <sys/queue.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "iperf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#if __has_include("esp_check.h")
#include "esp_check.h"
#else
/** @brief Error-handling helpers for ESP-IDF versions without esp_check.h. */
#define ESP_RETURN_ON_ERROR(x, log_tag, format, ...) do {                                       \
        esp_err_t err_rc_ = (x);                                                                \
        if (unlikely(err_rc_ != ESP_OK)) {                                                      \
            ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__);        \
            return err_rc_;                                                                     \
        }                                                                                       \
    } while(0)

#define ESP_GOTO_ON_ERROR(x, goto_tag, log_tag, format, ...) do {                               \
        esp_err_t err_rc_ = (x);                                                                \
        if (unlikely(err_rc_ != ESP_OK)) {                                                      \
            ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__);        \
            ret = err_rc_;                                                                      \
            goto goto_tag;                                                                      \
        }                                                                                       \
    } while(0)

#define ESP_GOTO_ON_FALSE(a, err_code, goto_tag, log_tag, format, ...) do {                                \
        if (unlikely(!(a))) {                                                                              \
            ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__);                   \
            ret = err_code;                                                                                \
            goto goto_tag;                                                                                 \
        }                                                                                                  \
    } while (0)
#endif

#define TAG_ID_STR "iperf(id=%" PRIi8 ")"

/** @brief Atomic period snapshot; 8-byte alignment is required for optimized builds. */
typedef struct {
    uint32_t period_data_snapshot;  /**< Period transfer count in bytes, bounded by uint32_t. */
    uint32_t period_sec;
}
#if CONFIG_COMPILER_OPTIMIZATION_SIZE || CONFIG_COMPILER_OPTIMIZATION_PERF
__attribute__((aligned(8)))
#endif
iperf_report_task_data_t;

/** @brief Transmission and reporting timers for an instance. */
typedef struct {
    esp_timer_handle_t tx_timer;
    esp_timer_handle_t tick_timer;
    uint64_t tx_period_us;
    uint32_t ticks;
    uint32_t to_report_ticks;
    _Atomic bool expired;
} iperf_timers_t;

/** @brief Socket addresses, options and transfer buffer. */
typedef struct {
    esp_ip_addr_t destination;
    esp_ip_addr_t source;
    struct sockaddr_storage target_addr; /**< Either the address to receive from if server, or send to if client */
    socklen_t target_addr_len;
    uint16_t dport;
    uint16_t sport;
    int tos;  /**< Integer-sized option required by setsockopt. */
    uint8_t *buffer;
    uint32_t buffer_len;
} iperf_socket_info_t;

/** @brief Owned iPerf tasks, sockets, timers and report state. */
typedef struct iperf_instance_data_struct {
    LIST_ENTRY(iperf_instance_data_struct) _list_entry;

    iperf_id_t id;
    bool list_registered;
    _Atomic bool is_running;
    char tag[sizeof(TAG_ID_STR) + 1];
    uint32_t flags;

    TaskHandle_t report_task_hdl;
    TaskHandle_t traffic_task_hdl;
    SemaphoreHandle_t report_task_exit_sem;
    StaticSemaphore_t socket_lock_storage;
    SemaphoreHandle_t socket_lock;
    uint32_t socket_io_users;
    uint32_t listen_socket_io_users;
    bool socket_closing;
    bool listen_socket_closing;

    iperf_timers_t timers;
    uint32_t interval;
    uint32_t time;

    _Atomic int socket;
    _Atomic int listen_socket;
    iperf_socket_info_t socket_info;

    _Atomic uint32_t period_data_passed;
    _Atomic iperf_report_task_data_t report_task_data;
    iperf_traffic_report_t traffic;  /**< Updated only by the report task. */

    iperf_state_handler_func_t state_handler;
    void* state_handler_priv;
} iperf_instance_data_t;

/**
 * @brief Publish an instance state transition to its registered handler.
 * @param iperf_state iPerf state.
 * @param iperf_instance iPerf instance.
 */
void iperf_state_action(iperf_state_t iperf_state, iperf_instance_data_t *iperf_instance);
/**
 * @brief Find an active instance by ID; the returned pointer is borrowed.
 * @param id Instance identifier.
 * @return Borrowed pointer to the matching active instance.
 */
iperf_instance_data_t* iperf_list_get_instance_by_id(iperf_id_t id);
/**
 * @brief Create the report worker for an instance.
 * @param iperf_instance iPerf instance.
 * @return Handle of the created report worker.
 */
TaskHandle_t iperf_create_report_task(iperf_instance_data_t *iperf_instance);

#ifdef __cplusplus
}
#endif
