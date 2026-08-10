/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "system/udp_nmea_server.h"

#include "config/config_nmea_tester.h"

#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_UDP_NMEA_SERVER
#include "config_logs.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

static const char *TAG = "udp_nmea";

static _Atomic(TaskHandle_t) s_task = NULL;
static atomic_bool s_stop = ATOMIC_VAR_INIT(false);
static atomic_int s_sock_fd = ATOMIC_VAR_INIT(-1);
static SemaphoreHandle_t s_sock_lock = NULL;
static udp_nmea_server_rx_cb_t s_rx_cb = NULL;
static void *s_rx_user = NULL;
static atomic_uint s_rx_inflight = ATOMIC_VAR_INIT(0);
static atomic_int s_start_result = ATOMIC_VAR_INIT(INT_MIN);
static portMUX_TYPE s_rx_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_last_peer_ip[16] = "";
static uint16_t s_last_peer_port = 0;
static int64_t s_last_peer_us = 0;
static struct sockaddr_in s_last_peer_addr = { 0 };

#define UDP_NMEA_STOP_WAIT_MS 7000U
#define UDP_NMEA_RX_DRAIN_POLL_MS 10U
#define UDP_NMEA_START_WAIT_MS 1500U
#define UDP_NMEA_START_PENDING INT_MIN

static udp_nmea_server_rx_cb_t udp_nmea_rx_acquire_(void **user)
{
    udp_nmea_server_rx_cb_t cb;

    portENTER_CRITICAL(&s_rx_lock);
    cb = s_rx_cb;
    *user = s_rx_user;
    if (cb) {
        atomic_fetch_add_explicit(&s_rx_inflight, 1u, memory_order_acq_rel);
    }
    portEXIT_CRITICAL(&s_rx_lock);
    return cb;
}

static void udp_nmea_rx_release_(void)
{
    atomic_fetch_sub_explicit(&s_rx_inflight, 1u, memory_order_release);
}

static void udp_nmea_rx_wait_idle_(void)
{
    while (atomic_load_explicit(&s_rx_inflight, memory_order_acquire) != 0u) {
        vTaskDelay(pdMS_TO_TICKS(UDP_NMEA_RX_DRAIN_POLL_MS));
    }
}

static bool stop_requested_(void)
{
    return atomic_load_explicit(&s_stop, memory_order_acquire);
}

static int socket_fd_(void)
{
    return atomic_load_explicit(&s_sock_fd, memory_order_acquire);
}

static void shutdown_socket_(void)
{
    const int sock_fd = socket_fd_();
    if (sock_fd >= 0) {
        (void)shutdown(sock_fd, SHUT_RDWR);
    }
}

static void close_socket_(void)
{
    const int sock_fd = atomic_exchange_explicit(
        &s_sock_fd, -1, memory_order_acq_rel);
    if (sock_fd >= 0) {
        (void)shutdown(sock_fd, SHUT_RDWR);
        close(sock_fd);
    }
}

static void set_recv_timeout_(int fd, int timeout_ms)
{
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static esp_err_t udp_nmea_wait_stopped_(uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (atomic_load_explicit(&s_task, memory_order_acquire)) {
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

static void udp_nmea_publish_start_(esp_err_t result)
{
    int expected = UDP_NMEA_START_PENDING;
    (void)atomic_compare_exchange_strong_explicit(
        &s_start_result, &expected, (int)result,
        memory_order_release, memory_order_relaxed);
}

static void udp_nmea_server_task_(void *arg)
{
    struct sockaddr_in addr = { 0 };
    int yes = 1;
    int sock_fd;

    (void)arg;

    /* start() publishes our handle before releasing this task. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    addr.sin_family = AF_INET;
    addr.sin_port = htons(WIFI_UDP_NMEA_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        goto done;
    }
    atomic_store_explicit(&s_sock_fd, sock_fd, memory_order_release);
    if (stop_requested_()) goto done;

    (void)setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    set_recv_timeout_(sock_fd, 1000);

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed: errno=%d", errno);
        goto done;
    }

    udp_nmea_publish_start_(ESP_OK);
    ESP_LOGI(TAG, "UDP NMEA server listening on port %u", (unsigned)WIFI_UDP_NMEA_PORT);

    while (!stop_requested_()) {
        uint8_t buf[512];
        struct sockaddr_in from = { 0 };
        socklen_t from_len = sizeof(from);
        sock_fd = socket_fd_();
        if (sock_fd < 0) break;
        int n = recvfrom(sock_fd, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &from_len);

        if (n > 0) {
            uint32_t ip = ntohl(from.sin_addr.s_addr);
            char peer_ip[16];
            snprintf(peer_ip, sizeof(peer_ip), "%lu.%lu.%lu.%lu",
                     (unsigned long)((ip >> 24) & 0xFFu),
                     (unsigned long)((ip >> 16) & 0xFFu),
                     (unsigned long)((ip >> 8) & 0xFFu),
                     (unsigned long)(ip & 0xFFu));
            if (s_sock_lock && xSemaphoreTake(s_sock_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                strlcpy(s_last_peer_ip, peer_ip, sizeof(s_last_peer_ip));
                s_last_peer_port = ntohs(from.sin_port);
                s_last_peer_us = esp_timer_get_time();
                s_last_peer_addr = from;
                xSemaphoreGive(s_sock_lock);
            }
            void *rx_user = NULL;
            udp_nmea_server_rx_cb_t rx_cb = udp_nmea_rx_acquire_(&rx_user);
            if (rx_cb) {
                rx_cb(buf, (size_t)n, rx_user);
                udp_nmea_rx_release_();
            }
            continue;
        }

        if (n == 0) {
            continue;
        }

        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
            continue;
        }
        if (stop_requested_()) break;

        ESP_LOGW(TAG, "recvfrom() failed: errno=%d", errno);
    }

done:
    udp_nmea_publish_start_(ESP_FAIL);
    if (s_sock_lock && xSemaphoreTake(s_sock_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        close_socket_();
        s_last_peer_ip[0] = 0;
        s_last_peer_port = 0;
        s_last_peer_us = 0;
        memset(&s_last_peer_addr, 0, sizeof(s_last_peer_addr));
        xSemaphoreGive(s_sock_lock);
    } else {
        close_socket_();
    }
    atomic_store_explicit(&s_task, NULL, memory_order_release);
    vTaskDelete(NULL);
}

esp_err_t udp_nmea_server_start(void)
{
#if !WIFI_AP_ENABLED || !WIFI_UDP_NMEA_ENABLED
    return ESP_OK;
#else
    TaskHandle_t created_task = NULL;

    if (atomic_load_explicit(&s_task, memory_order_acquire)) {
        return stop_requested_() ? ESP_ERR_INVALID_STATE : ESP_OK;
    }

    atomic_store_explicit(&s_stop, false, memory_order_release);
    atomic_store_explicit(&s_start_result, UDP_NMEA_START_PENDING, memory_order_release);
    if (!s_sock_lock) {
        s_sock_lock = xSemaphoreCreateMutex();
        if (!s_sock_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreatePinnedToCore(udp_nmea_server_task_, "udp_nmea", 4096,
                                NULL, 4, &created_task, tskNO_AFFINITY) != pdPASS) {
        atomic_store_explicit(&s_start_result, ESP_ERR_NO_MEM, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_task, created_task, memory_order_release);
    xTaskNotifyGive(created_task);

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(UDP_NMEA_START_WAIT_MS);
    int start_result;
    do {
        start_result = atomic_load_explicit(&s_start_result, memory_order_acquire);
        if (start_result != UDP_NMEA_START_PENDING) break;
        if (!atomic_load_explicit(&s_task, memory_order_acquire)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);

    start_result = atomic_load_explicit(&s_start_result, memory_order_acquire);
    if (start_result == ESP_OK) return ESP_OK;
    if (start_result == UDP_NMEA_START_PENDING) start_result = ESP_ERR_TIMEOUT;

    atomic_store_explicit(&s_stop, true, memory_order_release);
    shutdown_socket_();
    (void)udp_nmea_wait_stopped_(UDP_NMEA_STOP_WAIT_MS);
    return (esp_err_t)start_result;
#endif
}

esp_err_t udp_nmea_server_stop(void)
{
    atomic_store_explicit(&s_stop, true, memory_order_release);
    if (s_sock_lock && xSemaphoreTake(s_sock_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        shutdown_socket_();
        xSemaphoreGive(s_sock_lock);
    } else {
        shutdown_socket_();
    }
    return udp_nmea_wait_stopped_(UDP_NMEA_STOP_WAIT_MS);
}

bool udp_nmea_server_is_running(void)
{
    return atomic_load_explicit(&s_task, memory_order_acquire) != NULL;
}

void udp_nmea_server_set_rx_cb(udp_nmea_server_rx_cb_t cb, void *user)
{
    portENTER_CRITICAL(&s_rx_lock);
    s_rx_cb = NULL;
    s_rx_user = NULL;
    portEXIT_CRITICAL(&s_rx_lock);

    udp_nmea_rx_wait_idle_();

    portENTER_CRITICAL(&s_rx_lock);
    s_rx_cb = cb;
    s_rx_user = user;
    portEXIT_CRITICAL(&s_rx_lock);
}

bool udp_nmea_server_get_last_peer(char *buf, size_t buf_sz, uint16_t *port)
{
    if (!buf || buf_sz == 0) {
        return false;
    }

    buf[0] = 0;
    if (port) *port = 0;
    if (!s_sock_lock || xSemaphoreTake(s_sock_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    if (s_last_peer_ip[0] == 0) {
        xSemaphoreGive(s_sock_lock);
        return false;
    }

    strlcpy(buf, s_last_peer_ip, buf_sz);
    if (port) {
        *port = s_last_peer_port;
    }
    xSemaphoreGive(s_sock_lock);
    return true;
}

uint32_t udp_nmea_server_get_last_peer_age_ms(void)
{
    int64_t now_us;
    int64_t last_peer_us;

    if (!s_sock_lock || xSemaphoreTake(s_sock_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return UINT32_MAX;
    }
    last_peer_us = s_last_peer_us;
    xSemaphoreGive(s_sock_lock);

    if (last_peer_us == 0) {
        return UINT32_MAX;
    }

    now_us = esp_timer_get_time();
    if (now_us <= last_peer_us) {
        return 0;
    }

    return (uint32_t)((now_us - last_peer_us) / 1000);
}

esp_err_t udp_nmea_server_send_to_last_peer(const uint8_t *data, size_t len)
{
    int n;
    int sock_fd;

    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_sock_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_sock_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    sock_fd = socket_fd_();
    if (sock_fd < 0 || s_last_peer_us == 0) {
        xSemaphoreGive(s_sock_lock);
        return ESP_ERR_INVALID_STATE;
    }

    n = sendto(sock_fd, data, len, 0,
               (const struct sockaddr *)&s_last_peer_addr, sizeof(s_last_peer_addr));
    xSemaphoreGive(s_sock_lock);

    if (n < 0) {
        return ESP_FAIL;
    }
    if ((size_t)n != len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
