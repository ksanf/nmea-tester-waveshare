#include "web/web_server.h"
#include "app/app_controller.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <limits.h>

extern const unsigned char web_index_start[] asm("_binary_index_html_gz_start");
extern const unsigned char web_index_end[] asm("_binary_index_html_gz_end");

#define WEB_PEERS 5
#define WEB_INPUT_LIMIT 4096
#define WEB_OUTPUT_LIMIT 16384
#define WEB_PENDING_LIMIT 8

typedef struct { int id, fd; bool closing, close_queued; } peer_t;
typedef struct { int id; size_t len; char payload[]; } outgoing_t;
static httpd_handle_t s_server;
static peer_t s_peers[WEB_PEERS];
static int s_next_id = 1;
static portMUX_TYPE s_peer_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_uint s_pending;
static QueueHandle_t s_outgoing;
static _Atomic(TaskHandle_t) s_dispatch_task;
static atomic_bool s_dispatch_stop;
static atomic_bool s_ready;
static atomic_bool s_work_pending;

/* HTTPD queue_work may wait for its control mailbox. Only this worker is
 * permitted to call it; the controller and the LCD return path never wait. */
static void dispatch_task_(void *arg);
static void close_work_(void *arg);

static int peer_fd_(int id) {
    int fd = -1;
    portENTER_CRITICAL(&s_peer_lock);
    for (int i = 0; i < WEB_PEERS; ++i) if (s_peers[i].id == id) { fd = s_peers[i].fd; break; }
    portEXIT_CRITICAL(&s_peer_lock);
    return fd;
}
bool web_server_peer_alive(int id) {
    bool alive = false;
    portENTER_CRITICAL(&s_peer_lock);
    for (int i = 0; i < WEB_PEERS; ++i)
        if (s_peers[i].id == id && id > 0) { alive = !s_peers[i].closing; break; }
    portEXIT_CRITICAL(&s_peer_lock);
    return alive;
}
static void free_peer_(void *ctx) {
    peer_t *p = ctx;
    if (!p) return;
    portENTER_CRITICAL(&s_peer_lock);
    for (int i = 0; i < WEB_PEERS; ++i) if (s_peers[i].id == p->id) s_peers[i] = (peer_t){0};
    portEXIT_CRITICAL(&s_peer_lock);
    app_controller_web_disconnected(p->id);
    free(p);
}
static esp_err_t index_(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    const size_t length = web_index_end - web_index_start;
    for (size_t offset = 0; offset < length; offset += 2048) {
        size_t count = length - offset; if (count > 2048) count = 2048;
        esp_err_t err = httpd_resp_send_chunk(req, (const char *)web_index_start + offset, count);
        if (err != ESP_OK) return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}
static esp_err_t websocket_(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        char host[96], origin[112], expected[112];
        if(httpd_req_get_hdr_value_str(req,"Host",host,sizeof(host))!=ESP_OK ||
           httpd_req_get_hdr_value_str(req,"Origin",origin,sizeof(origin))!=ESP_OK)return ESP_FAIL;
        snprintf(expected,sizeof(expected),"http://%s",host);
        if(strcmp(origin,expected))return ESP_FAIL;
        peer_t *peer = calloc(1, sizeof(*peer));
        if (!peer) return ESP_ERR_NO_MEM;
        peer->fd = httpd_req_to_sockfd(req);
        bool inserted = false;
        portENTER_CRITICAL(&s_peer_lock);
        for (int i = 0; i < WEB_PEERS; ++i) if (!s_peers[i].id) {
            peer->id = s_next_id;
            s_next_id = s_next_id == INT_MAX ? 1 : s_next_id + 1;
            s_peers[i] = *peer; inserted = true; break;
        }
        portEXIT_CRITICAL(&s_peer_lock);
        if (!inserted) { free(peer); return ESP_FAIL; }
        req->sess_ctx = peer; req->free_ctx = free_peer_;
        return ESP_OK;
    }
    peer_t *peer = req->sess_ctx;
    if (!peer) return ESP_FAIL;
    httpd_ws_frame_t packet = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t err = httpd_ws_recv_frame(req, &packet, 0);
    if (err != ESP_OK) return err;
    if (packet.type == HTTPD_WS_TYPE_CLOSE) return ESP_FAIL;
    if (packet.type != HTTPD_WS_TYPE_TEXT || !packet.final ||
        !packet.len || packet.len > WEB_INPUT_LIMIT) return ESP_ERR_INVALID_SIZE;
    char *buffer = heap_caps_malloc(packet.len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) return ESP_ERR_NO_MEM;
    packet.payload = (uint8_t *)buffer;
    err = httpd_ws_recv_frame(req, &packet, packet.len);
    if (err == ESP_OK) {
        buffer[packet.len] = 0;
        if (memchr(buffer, 0, packet.len) ||
            !app_controller_web_submit(peer->id, buffer, packet.len)) err = ESP_FAIL;
    }
    free(buffer);
    return err;
}
static void send_work_(void *arg) {
    outgoing_t *out = arg;
    int fd = peer_fd_(out->id);
    if (fd >= 0 && httpd_ws_get_fd_info(s_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        httpd_ws_frame_t frame = {
            .final = true, .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)out->payload, .len = out->len
        };
        if (httpd_ws_send_frame_async(s_server, fd, &frame) != ESP_OK)
            web_server_close(out->id);
    }
    free(out);
    atomic_fetch_sub(&s_pending, 1);
    atomic_store_explicit(&s_work_pending, false, memory_order_release);
}
bool web_server_send(int id, const char *text, size_t len) {
    if (!atomic_load_explicit(&s_ready, memory_order_acquire) ||
        !text || !len || len > WEB_OUTPUT_LIMIT || peer_fd_(id) < 0) return false;
    if (atomic_fetch_add(&s_pending, 1) >= WEB_PENDING_LIMIT) {
        atomic_fetch_sub(&s_pending, 1); return false;
    }
    outgoing_t *out = heap_caps_malloc(sizeof(*out) + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) { atomic_fetch_sub(&s_pending, 1); return false; }
    out->id = id; out->len = len; memcpy(out->payload, text, len);
    if (xQueueSend(s_outgoing, &out, 0) != pdTRUE) {
        free(out); atomic_fetch_sub(&s_pending, 1); return false;
    }
    return true;
}
static void close_work_(void *arg) {
    int id = (int)(intptr_t)arg;
    int fd = peer_fd_(id);
    /* Already on the HTTPD task: never call trigger_close here, since it
     * recursively queue_work()s and can exhaust the blocking control mailbox.
     * shutdown preserves HTTPD ownership of close() and session cleanup. */
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
    atomic_store_explicit(&s_work_pending, false, memory_order_release);
}
void web_server_close(int id) {
    if (id <= 0) return;
    /* A close request is retained even when the outgoing queue is full. */
    portENTER_CRITICAL(&s_peer_lock);
    for (int i = 0; i < WEB_PEERS; ++i)
        if (s_peers[i].id == id) { s_peers[i].closing = true; break; }
    portEXIT_CRITICAL(&s_peer_lock);
}

static int next_close_(void)
{
    int id = -1;
    portENTER_CRITICAL(&s_peer_lock);
    for (int i = 0; i < WEB_PEERS; ++i) {
        if (s_peers[i].id && s_peers[i].closing && !s_peers[i].close_queued) {
            id = s_peers[i].id;
            s_peers[i].close_queued = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_peer_lock);
    return id;
}

static void discard_output_(outgoing_t *out)
{
    free(out);
    atomic_fetch_sub(&s_pending, 1);
}

static void retry_close_(int id)
{
    portENTER_CRITICAL(&s_peer_lock);
    for (int i = 0; i < WEB_PEERS; ++i)
        if (s_peers[i].id == id) { s_peers[i].close_queued = false; break; }
    portEXIT_CRITICAL(&s_peer_lock);
}

static void dispatch_task_(void *arg)
{
    (void)arg;
    while (!atomic_load_explicit(&s_dispatch_stop, memory_order_acquire)) {
        /* Keep only one application callback in the HTTPD mailbox. IDF also
         * queues work internally while processing WebSocket close frames. */
        if (atomic_load_explicit(&s_work_pending, memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int id = next_close_();
        if (id > 0) {
            atomic_store_explicit(&s_work_pending, true, memory_order_release);
            if (httpd_queue_work(s_server, close_work_, (void *)(intptr_t)id) != ESP_OK) {
                atomic_store_explicit(&s_work_pending, false, memory_order_release);
                retry_close_(id);
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            continue;
        }
        outgoing_t *out = NULL;
        if (xQueueReceive(s_outgoing, &out, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (peer_fd_(out->id) < 0) { discard_output_(out); continue; }
            atomic_store_explicit(&s_work_pending, true, memory_order_release);
            if (httpd_queue_work(s_server, send_work_, out) != ESP_OK) {
                atomic_store_explicit(&s_work_pending, false, memory_order_release);
                discard_output_(out);
            }
        }
    }
    atomic_store_explicit(&s_dispatch_task, NULL, memory_order_release);
    vTaskDelete(NULL);
}

/* Startup failure only: handlers are not registered yet, so no peer can own
 * outstanding work. Join the polling dispatcher before freeing its queue. */
static void cleanup_startup_(void)
{
    atomic_store_explicit(&s_ready, false, memory_order_release);
    atomic_store_explicit(&s_dispatch_stop, true, memory_order_release);
    while (atomic_load_explicit(&s_dispatch_task, memory_order_acquire))
        vTaskDelay(pdMS_TO_TICKS(10));
    if (s_outgoing) {
        outgoing_t *out;
        while (xQueueReceive(s_outgoing, &out, 0) == pdTRUE) discard_output_(out);
        vQueueDelete(s_outgoing); s_outgoing = NULL;
    }
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
}

esp_err_t web_server_start(void) {
    if (atomic_load_explicit(&s_ready, memory_order_acquire)) return ESP_OK;
    if (s_server) return ESP_ERR_INVALID_STATE;
    s_outgoing = xQueueCreate(WEB_PENDING_LIMIT, sizeof(outgoing_t *));
    if (!s_outgoing) return ESP_ERR_NO_MEM;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144; cfg.max_open_sockets = WEB_PEERS;
    cfg.max_uri_handlers = 4; cfg.lru_purge_enable = false;
    cfg.recv_wait_timeout = 2; cfg.send_wait_timeout = 1;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) { cleanup_startup_(); return err; }
    atomic_store_explicit(&s_dispatch_stop, false, memory_order_release);
    TaskHandle_t dispatcher = NULL;
    if (xTaskCreate(dispatch_task_, "web_output", 4096, NULL, 3, &dispatcher) != pdPASS) {
        cleanup_startup_(); return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_dispatch_task, dispatcher, memory_order_release);
    const httpd_uri_t index = { .uri = "/", .method = HTTP_GET, .handler = index_ };
    const httpd_uri_t ws = { .uri = "/ws", .method = HTTP_GET, .handler = websocket_, .is_websocket = true };
    err = httpd_register_uri_handler(s_server, &index);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_server, &ws);
    if (err != ESP_OK) { cleanup_startup_(); return err; }
    atomic_store_explicit(&s_ready, true, memory_order_release);
    return ESP_OK;
}
