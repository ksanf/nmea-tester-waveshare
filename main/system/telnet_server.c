/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "system/telnet_server.h"
#include "system/telnet_rx.h"

#include "config/config_nmea_tester.h"
#include "config/memory_config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_TELNET_SERVER
#include "config_logs.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

static const char *TAG = "telnet_srv";

static _Atomic(TaskHandle_t) s_task = NULL;
static atomic_bool s_stop = ATOMIC_VAR_INIT(false);
static atomic_int s_listen_fd = ATOMIC_VAR_INIT(-1);
static atomic_int s_client_fd = ATOMIC_VAR_INIT(-1);
static QueueHandle_t s_tx_q = NULL;
static telnet_server_rx_cb_t s_rx_cb = NULL;
static void *s_rx_user = NULL;
static atomic_uint s_rx_inflight = ATOMIC_VAR_INIT(0);
static portMUX_TYPE s_rx_lock = portMUX_INITIALIZER_UNLOCKED;
static _Atomic uint32_t s_client_ipv4 = 0;
static atomic_int s_start_result = ATOMIC_VAR_INIT(INT_MIN);

#define TELNET_TX_Q_LEN 32
#define TELNET_STOP_WAIT_MS 3000U
#define TELNET_RX_DRAIN_POLL_MS 10U
#define TELNET_IO_POLL_MS 20
#define TELNET_START_WAIT_MS 1500U
#define TELNET_START_PENDING INT_MIN

typedef struct {
    uint8_t *p;
    uint16_t len;
} telnet_tx_evt_t;

static telnet_server_rx_cb_t telnet_rx_acquire_(void **user)
{
    telnet_server_rx_cb_t cb;

    portENTER_CRITICAL(&s_rx_lock);
    cb = s_rx_cb;
    *user = s_rx_user;
    if (cb) {
        atomic_fetch_add_explicit(&s_rx_inflight, 1u, memory_order_acq_rel);
    }
    portEXIT_CRITICAL(&s_rx_lock);
    return cb;
}

static void telnet_rx_release_(void)
{
    atomic_fetch_sub_explicit(&s_rx_inflight, 1u, memory_order_release);
}

static void telnet_rx_wait_idle_(void)
{
    while (atomic_load_explicit(&s_rx_inflight, memory_order_acquire) != 0u) {
        vTaskDelay(pdMS_TO_TICKS(TELNET_RX_DRAIN_POLL_MS));
    }
}

static bool stop_requested_(void)
{
    return atomic_load_explicit(&s_stop, memory_order_acquire);
}

static int socket_fd_(const atomic_int *fd)
{
    return atomic_load_explicit(fd, memory_order_acquire);
}

static void shutdown_socket_(const atomic_int *fd)
{
    const int socket_fd = socket_fd_(fd);
    if (socket_fd >= 0) {
        (void)shutdown(socket_fd, SHUT_RDWR);
    }
}

static void close_socket_(atomic_int *fd)
{
    const int socket_fd = atomic_exchange_explicit(fd, -1, memory_order_acq_rel);
    if (socket_fd >= 0) {
        (void)shutdown(socket_fd, SHUT_RDWR);
        close(socket_fd);
    }
}

static void close_client_socket_(void)
{
    close_socket_(&s_client_fd);
    atomic_store_explicit(&s_client_ipv4, 0u, memory_order_release);
}

static void set_recv_timeout_(int fd, int timeout_ms)
{
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static void set_send_timeout_(int fd, int timeout_ms)
{
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void telnet_tx_queue_drain_(void)
{
    telnet_tx_evt_t evt;

    if (!s_tx_q) return;
    while (xQueueReceive(s_tx_q, &evt, 0) == pdTRUE) {
        free(evt.p);
    }
}

static void telnet_flush_tx_queue_(telnet_tx_evt_t *pending, size_t *pending_off)
{
    if (!s_tx_q || !pending || !pending_off) return;
    for (;;) {
        if (!pending->p && xQueueReceive(s_tx_q, pending, 0) != pdTRUE) return;

        int client_fd = socket_fd_(&s_client_fd);
        if (pending->p && pending->len && client_fd >= 0) {
            while (*pending_off < pending->len && !stop_requested_()) {
                int sent = send(client_fd, pending->p + *pending_off,
                                pending->len - *pending_off, 0);
                if (sent > 0) {
                    *pending_off += (size_t)sent;
                    continue;
                }
                if (errno == EINTR) continue;
                if (errno == EWOULDBLOCK || errno == EAGAIN) return;
                close_client_socket_();
                break;
            }
        }
        free(pending->p);
        pending->p = NULL;
        pending->len = 0;
        *pending_off = 0;
        if (client_fd < 0 || stop_requested_()) return;
    }
}

static esp_err_t telnet_wait_stopped_(uint32_t timeout_ms)
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

static void telnet_publish_start_(esp_err_t result)
{
    int expected = TELNET_START_PENDING;
    (void)atomic_compare_exchange_strong_explicit(
        &s_start_result, &expected, (int)result,
        memory_order_release, memory_order_relaxed);
}

static void telnet_server_task_(void *arg)
{
    (void)arg;

    struct sockaddr_in addr = { 0 };
    int yes = 1;
    int listen_fd;

    addr.sin_family = AF_INET;
    addr.sin_port = htons(WIFI_TELNET_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* start() publishes our handle before releasing this task. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        goto done;
    }
    atomic_store_explicit(&s_listen_fd, listen_fd, memory_order_release);
    if (stop_requested_()) goto done;

    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    set_recv_timeout_(listen_fd, 1000);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed: errno=%d", errno);
        goto done;
    }

    if (listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
        goto done;
    }

    telnet_publish_start_(ESP_OK);
    ESP_LOGI(TAG, "Telnet server listening on port %u", (unsigned)WIFI_TELNET_PORT);

    while (!stop_requested_()) {
        struct sockaddr_in client_addr = { 0 };
        socklen_t client_len = sizeof(client_addr);
        int client_fd;

        client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (stop_requested_()) break;
            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
                continue;
            }

            ESP_LOGW(TAG, "accept() failed: errno=%d", errno);
            continue;
        }
        if (stop_requested_()) {
            (void)shutdown(client_fd, SHUT_RDWR);
            close(client_fd);
            break;
        }

        /* The same task drains the asynchronous TX queue and receives client
         * input.  Keep recv() bounded tightly so antenna output cannot sit in
         * the queue until a long socket timeout expires. */
        set_recv_timeout_(client_fd, TELNET_IO_POLL_MS);
        set_send_timeout_(client_fd, 50);
        const int nodelay = 1;
        if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                       &nodelay, sizeof(nodelay)) != 0) {
            ESP_LOGW(TAG, "TCP_NODELAY failed: errno=%d", errno);
        }
        uint32_t ip = ntohl(client_addr.sin_addr.s_addr);
        atomic_store_explicit(&s_client_ipv4, ip, memory_order_release);
        atomic_store_explicit(&s_client_fd, client_fd, memory_order_release);
        ESP_LOGI(TAG, "Client connected");
        telnet_rx_t rx_state = {0};
        telnet_tx_evt_t pending_tx = {0};
        size_t pending_tx_off = 0;

        static const char banner[] =
            "NMEA Tester telnet\r\n"
            "Wi-Fi terminal channel is active.\r\n";

        (void)send(client_fd, banner, sizeof(banner) - 1, 0);

        while (!stop_requested_()) {
            uint8_t buf[128];

            telnet_flush_tx_queue_(&pending_tx, &pending_tx_off);
            client_fd = socket_fd_(&s_client_fd);
            if (client_fd < 0) break;
            int n = recv(client_fd, buf, sizeof(buf), 0);

            if (n > 0) {
                void *rx_user = NULL;
                telnet_server_rx_cb_t rx_cb = telnet_rx_acquire_(&rx_user);
                if (rx_cb) {
                    size_t filtered = telnet_rx_filter(&rx_state, buf, (size_t)n);
                    if (filtered > 0) {
                        rx_cb(buf, filtered, rx_user);
                    }
                    telnet_rx_release_();
                } else {
                    (void)send(client_fd, buf, (size_t)n, 0);
                }
                continue;
            }

            if (n == 0) {
                ESP_LOGI(TAG, "Client disconnected");
                break;
            }

            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
                continue;
            }
            if (stop_requested_()) break;

            ESP_LOGW(TAG, "recv() failed: errno=%d", errno);
            break;
        }

        telnet_flush_tx_queue_(&pending_tx, &pending_tx_off);
        free(pending_tx.p);
        telnet_tx_queue_drain_();
        close_client_socket_();
    }

done:
    telnet_publish_start_(ESP_FAIL);
    telnet_tx_queue_drain_();
    close_client_socket_();
    close_socket_(&s_listen_fd);
    atomic_store_explicit(&s_task, NULL, memory_order_release);
    vTaskDelete(NULL);
}

esp_err_t telnet_server_start(void)
{
#if !WIFI_AP_ENABLED || !WIFI_TELNET_ENABLED
    return ESP_OK;
#else
    TaskHandle_t created_task = NULL;

    if (atomic_load_explicit(&s_task, memory_order_acquire)) {
        return stop_requested_() ? ESP_ERR_INVALID_STATE : ESP_OK;
    }

    atomic_store_explicit(&s_stop, false, memory_order_release);
    atomic_store_explicit(&s_start_result, TELNET_START_PENDING, memory_order_release);
    if (!s_tx_q) {
        s_tx_q = xQueueCreate(TELNET_TX_Q_LEN, sizeof(telnet_tx_evt_t));
        if (!s_tx_q) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreatePinnedToCore(telnet_server_task_, "telnet_srv", 4096, NULL, 4,
                                &created_task, tskNO_AFFINITY) != pdPASS) {
        atomic_store_explicit(&s_start_result, ESP_ERR_NO_MEM, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_task, created_task, memory_order_release);
    xTaskNotifyGive(created_task);

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TELNET_START_WAIT_MS);
    int start_result;
    do {
        start_result = atomic_load_explicit(&s_start_result, memory_order_acquire);
        if (start_result != TELNET_START_PENDING) break;
        if (!atomic_load_explicit(&s_task, memory_order_acquire)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);

    start_result = atomic_load_explicit(&s_start_result, memory_order_acquire);
    if (start_result == ESP_OK) return ESP_OK;
    if (start_result == TELNET_START_PENDING) start_result = ESP_ERR_TIMEOUT;

    atomic_store_explicit(&s_stop, true, memory_order_release);
    shutdown_socket_(&s_client_fd);
    shutdown_socket_(&s_listen_fd);
    (void)telnet_wait_stopped_(TELNET_STOP_WAIT_MS);
    return (esp_err_t)start_result;
#endif
}

esp_err_t telnet_server_stop(void)
{
    atomic_store_explicit(&s_stop, true, memory_order_release);
    shutdown_socket_(&s_client_fd);
    shutdown_socket_(&s_listen_fd);
    telnet_tx_queue_drain_();
    return telnet_wait_stopped_(TELNET_STOP_WAIT_MS);
}

bool telnet_server_is_running(void)
{
    return atomic_load_explicit(&s_task, memory_order_acquire) != NULL;
}

bool telnet_server_client_connected(void)
{
    return socket_fd_(&s_client_fd) >= 0;
}

bool telnet_server_get_client_ip(char *buf, size_t buf_sz)
{
    uint32_t ip;

    if (!buf || buf_sz == 0) return false;
    ip = atomic_load_explicit(&s_client_ipv4, memory_order_acquire);
    if (socket_fd_(&s_client_fd) < 0 || ip == 0u) {
        buf[0] = 0;
        return false;
    }
    snprintf(buf, buf_sz, "%lu.%lu.%lu.%lu",
             (unsigned long)((ip >> 24) & 0xFFu),
             (unsigned long)((ip >> 16) & 0xFFu),
             (unsigned long)((ip >> 8) & 0xFFu),
             (unsigned long)(ip & 0xFFu));
    return true;
}

esp_err_t telnet_server_send(const uint8_t *data, size_t len)
{
    telnet_tx_evt_t evt = {0};
    uint8_t *cp;

    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > UINT16_MAX) return ESP_ERR_INVALID_SIZE;
    if (!s_tx_q) return ESP_ERR_INVALID_STATE;
    if (socket_fd_(&s_client_fd) < 0) return ESP_ERR_INVALID_STATE;

    cp = MALLOC_WHERE(TELNET_TX_BUF_IN_PSRAM, len);
    if (!cp) return ESP_ERR_NO_MEM;
    memcpy(cp, data, len);

    evt.p = cp;
    evt.len = (uint16_t)len;
    if (xQueueSend(s_tx_q, &evt, 0) != pdTRUE) {
        free(cp);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void telnet_server_set_rx_cb(telnet_server_rx_cb_t cb, void *user)
{
    portENTER_CRITICAL(&s_rx_lock);
    s_rx_cb = NULL;
    s_rx_user = NULL;
    portEXIT_CRITICAL(&s_rx_lock);

    telnet_rx_wait_idle_();

    portENTER_CRITICAL(&s_rx_lock);
    s_rx_cb = cb;
    s_rx_user = user;
    portEXIT_CRITICAL(&s_rx_lock);
}

void telnet_server_flush_tx(void)
{
    /* TX is owned by the server task and polled every TELNET_IO_POLL_MS.
     * Sending here would race the task and could reorder TCP bytes. */
}
