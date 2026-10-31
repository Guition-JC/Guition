#include "dns_captive.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#define DNS_PORT 53
#define DNS_PACKET_MAX 512
#define DNS_TTL_SECONDS 60
#define DNS_HEADER_LENGTH 12
#define DNS_QUESTION_FIXED_LENGTH 4
#define DNS_LABEL_MAX_LENGTH 63
#define DNS_NAME_MAX_LENGTH 255
#define DNS_STOP_TIMEOUT_MS 2000

static const char *TAG = "dns_captive";

typedef struct dns_captive_server {
    TaskHandle_t task;
    SemaphoreHandle_t exit_semaphore;
    volatile bool running;
    volatile int socket_fd;
    esp_netif_t *netif;
} dns_captive_server_t;

static bool dns_skip_name(const uint8_t *packet, size_t length, size_t *cursor)
{
    if (packet == NULL || cursor == NULL || *cursor >= length) {
        return false;
    }

    const size_t start = *cursor;
    size_t index = start;
    size_t encoded_length = 0;
    size_t expanded_length = 0;
    bool jumped = false;
    size_t jumps = 0;
    while (true) {
        if (index >= length) {
            return false;
        }
        const uint8_t label_length = packet[index];
        if (label_length == 0) {
            if (!jumped) {
                encoded_length++;
            }
            if (expanded_length >= DNS_NAME_MAX_LENGTH || start > length - encoded_length) {
                return false;
            }
            *cursor = start + encoded_length;
            return true;
        }
        if ((label_length & 0xc0U) == 0xc0U) {
            if (length - index < 2) {
                return false;
            }
            const size_t pointer = ((size_t)(label_length & 0x3fU) << 8) | packet[index + 1];
            if (pointer < DNS_HEADER_LENGTH || pointer >= index || ++jumps > length) {
                return false;
            }
            if (!jumped) {
                encoded_length += 2;
                jumped = true;
            }
            index = pointer;
            continue;
        }
        if ((label_length & 0xc0U) != 0 || label_length > DNS_LABEL_MAX_LENGTH ||
            length - index - 1 < label_length ||
            expanded_length + (size_t)label_length + 1 > (size_t)DNS_NAME_MAX_LENGTH) {
            return false;
        }
        index += label_length + 1;
        expanded_length += label_length + 1;
        if (!jumped) {
            encoded_length += label_length + 1;
        }
    }
}

static uint16_t dns_read_u16(const uint8_t *value)
{
    return ((uint16_t)value[0] << 8) | value[1];
}

static bool dns_query_question_end(const uint8_t *packet, size_t length, size_t *question_end)
{
    if (packet == NULL || question_end == NULL || length < DNS_HEADER_LENGTH ||
        (packet[2] & 0x80U) != 0 || (packet[2] & 0x78U) != 0 || (packet[2] & 0x02U) != 0 ||
        packet[3] != 0 || dns_read_u16(packet + 4) != 1 ||
        dns_read_u16(packet + 6) != 0 || dns_read_u16(packet + 8) != 0) {
        return false;
    }

    size_t cursor = DNS_HEADER_LENGTH;
    if (!dns_skip_name(packet, length, &cursor) || length - cursor < DNS_QUESTION_FIXED_LENGTH) {
        return false;
    }
    const size_t end_of_question = cursor + DNS_QUESTION_FIXED_LENGTH;
    if (dns_read_u16(packet + cursor) != 1 || dns_read_u16(packet + cursor + 2) != 1) {
        return false;
    }
    cursor = end_of_question;

    for (uint16_t index = 0; index < dns_read_u16(packet + 10); ++index) {
        if (!dns_skip_name(packet, length, &cursor) || length - cursor < 10) {
            return false;
        }
        cursor += 8;
        const uint16_t data_length = dns_read_u16(packet + cursor);
        cursor += 2;
        if (data_length > length - cursor) {
            return false;
        }
        cursor += data_length;
    }
    if (cursor != length) {
        return false;
    }

    *question_end = end_of_question;
    return true;
}

static int dns_build_reply(const uint8_t *request, size_t request_length, uint8_t *reply,
                           size_t reply_capacity, uint32_t answer_ip)
{
    size_t question_end;
    if (!dns_query_question_end(request, request_length, &question_end) ||
        question_end > reply_capacity || reply_capacity - question_end < 16) {
        return -1;
    }

    memcpy(reply, request, question_end);
    reply[2] = 0x84 | (request[2] & 0x01U);
    reply[3] = 0;
    reply[4] = 0;
    reply[5] = 1;
    reply[6] = 0;
    reply[7] = 1;
    reply[8] = 0;
    reply[9] = 0;
    reply[10] = 0;
    reply[11] = 0;

    uint8_t *answer = reply + question_end;
    answer[0] = 0xc0;
    answer[1] = 0x0c;
    answer[2] = 0;
    answer[3] = 1;
    answer[4] = 0;
    answer[5] = 1;
    answer[6] = 0;
    answer[7] = 0;
    answer[8] = 0;
    answer[9] = DNS_TTL_SECONDS;
    answer[10] = 0;
    answer[11] = 4;
    memcpy(answer + 12, &answer_ip, sizeof(answer_ip));
    return (int)(question_end + 16);
}

static void dns_captive_task_signal_exit(dns_captive_server_t *server)
{
    server->running = false;
    xSemaphoreGive(server->exit_semaphore);
    /** The owner reaps this suspended task after observing socket cleanup. */
    vTaskSuspend(NULL);
}

static void dns_captive_task(void *argument)
{
    dns_captive_server_t *server = argument;
    uint8_t request[DNS_PACKET_MAX];
    uint8_t reply[DNS_PACKET_MAX];
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

    if (socket_fd < 0) {
        ESP_LOGE(TAG, "socket failed: errno=%d", errno);
        dns_captive_task_signal_exit(server);
        return;
    }

    server->socket_fd = socket_fd;
    if (!server->running) {
        close(socket_fd);
        server->socket_fd = -1;
        dns_captive_task_signal_exit(server);
        return;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        ESP_LOGE(TAG, "bind failed: errno=%d", errno);
        close(socket_fd);
        server->socket_fd = -1;
        dns_captive_task_signal_exit(server);
        return;
    }
    ESP_LOGI(TAG, "DNS captive server listening on UDP/%d", DNS_PORT);

    while (server->running) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket_fd, &read_set);
        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        const int ready = select(socket_fd + 1, &read_set, NULL, NULL, &timeout);
        if (!server->running || ready <= 0 || !FD_ISSET(socket_fd, &read_set)) {
            continue;
        }

        struct sockaddr_in source;
        socklen_t source_length = sizeof(source);
        const ssize_t request_length = recvfrom(socket_fd, request, sizeof(request), 0,
                                                (struct sockaddr *)&source, &source_length);
        if (!server->running || request_length <= 0) {
            continue;
        }
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(server->netif, &ip_info) != ESP_OK) {
            continue;
        }
        const int reply_length = dns_build_reply(request, (size_t)request_length, reply,
                                                 sizeof(reply), ip_info.ip.addr);
        if (reply_length > 0) {
            sendto(socket_fd, reply, (size_t)reply_length, 0,
                   (struct sockaddr *)&source, source_length);
        }
    }

    close(socket_fd);
    server->socket_fd = -1;
    dns_captive_task_signal_exit(server);
}

esp_err_t dns_captive_start(esp_netif_t *netif, dns_captive_handle_t *handle)
{
    if (netif == NULL || handle == NULL || *handle != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    dns_captive_server_t *server = calloc(1, sizeof(*server));
    if (server == NULL) {
        return ESP_ERR_NO_MEM;
    }
    server->exit_semaphore = xSemaphoreCreateBinary();
    if (server->exit_semaphore == NULL) {
        free(server);
        return ESP_ERR_NO_MEM;
    }
    server->netif = netif;
    server->running = true;
    server->socket_fd = -1;
    if (xTaskCreate(dns_captive_task, "dns_captive", 3072, server, 5, &server->task) != pdPASS) {
        vSemaphoreDelete(server->exit_semaphore);
        free(server);
        return ESP_ERR_NO_MEM;
    }
    *handle = server;
    return ESP_OK;
}

esp_err_t dns_captive_stop(dns_captive_handle_t *handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*handle == NULL) {
        return ESP_OK;
    }

    dns_captive_server_t *server = *handle;
    server->running = false;
    if (server->socket_fd >= 0) {
        shutdown(server->socket_fd, SHUT_RDWR);
    }
    if (xSemaphoreTake(server->exit_semaphore, pdMS_TO_TICKS(DNS_STOP_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "DNS task exit confirmation timed out");
        return ESP_ERR_TIMEOUT;
    }

    vTaskDelete(server->task);
    vSemaphoreDelete(server->exit_semaphore);
    free(server);
    *handle = NULL;
    return ESP_OK;
}
