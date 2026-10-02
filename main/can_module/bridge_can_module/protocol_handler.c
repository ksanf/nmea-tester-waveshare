/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   UART<->CAN infrastructure handler (bring-up/tasks/timers/queues), without protocol logic.
 */

#include "protocol_handler.h"
#include "protocol_poll_worker.h"
#include "bridge_can_config.h"
#include "can_driver.h"

/* Stack and upper layers */
#include "sailor_proto_common.h"
#include "fsm.h"                /* TT6006 profile owns L2, FP and NDP clients */

/* Types used by bind_layers configuration; no protocol logic lives here. */
#include "l2_link.h"            /* sp_l2_config_t */
#include "l3l4_transport.h"     /* sp_timing_t */

#include "serial_comm.h"
#include "buffer_manager.h"
#include "can_byte_ring.h"
#include "config_nmea_tester.h"
#include "system/telnet_router.h"
#include "system/telnet_server.h"
#include "system/wifi_ap.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdatomic.h>

#define TAG "PROTO"

/* ===== Periods and timers ===== */
#define TERM_RX_TICK_MS       20            /* 50 Hz */
#define TERM_RX_MAX_PER_TICK  64
#define TERM_RX_PENDING_MAX   192
#define TERM_RX_IDLE_FLUSH_MS 250
#define TERM_RX_BURST_FLUSH_MS 80
#define TERM_EVT_ENQ_WAIT_MS  0  /* never block CAN RX callback on PIPE queue backpressure */
#define PIPE_TICK_MS          20
#define PIPE_MAX_EVT_PER_TICK 4
#define PIPE_MAX_BYTES_PER_TICK 512
#define PIPE_UART_CHUNK_MAX    64
#define HANDLER_TASK_STOP_WAIT_MS 2500u
#define HANDLER_TASK_STOP_POLL_MS   10u

static bool term_rx_has_line_end_(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == '\r' || buf[i] == '\n') return true;
    }
    return false;
}

/* ===== UI and state ===== */
static _Atomic(protocol_log_cb_t) g_log_cb = NULL;
static _Atomic(protocol_state_cb_t) g_state_cb = NULL;
static void               *g_ui_user  = NULL;
static _Atomic(protocol_link_state_t) g_state = PROTO_STATE_BOOT;
static portMUX_TYPE g_ui_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_uint g_ui_inflight = ATOMIC_VAR_INIT(0);

/* ===== Configuration and resources ===== */
static protocol_handler_cfg_t g_cfg;
static atomic_uint g_rs_baudrate;
static _Atomic(TaskHandle_t)  g_task_term_rx = NULL;
static _Atomic(TaskHandle_t)  g_task_pipe    = NULL;
static atomic_bool            g_tasks_stop = ATOMIC_VAR_INIT(false);
static atomic_bool g_term_ready_seen = ATOMIC_VAR_INIT(false);
static _Atomic(protocol_term_io_t) g_term_owner = PROTOCOL_TERM_IO_UART;
static atomic_bool g_handler_ready = ATOMIC_VAR_INIT(false);
static atomic_bool g_can_owned = ATOMIC_VAR_INIT(false);
static atomic_bool g_serial_owned = ATOMIC_VAR_INIT(false);
static atomic_uint g_io_generation = ATOMIC_VAR_INIT(0);
/* Kept for process lifetime: browser writes/drains never race a freed mutex/ring. */
static _Atomic(SemaphoreHandle_t) g_io_mutex;
static portMUX_TYPE g_web_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t g_web_input[PROTOCOL_WEB_INPUT_CAPACITY];
static can_byte_ring_t g_web_input_ring = CAN_BYTE_RING_INIT(g_web_input);
static uint8_t g_web_output[PROTOCOL_WEB_OUTPUT_CAPACITY];
static can_byte_ring_t g_web_output_ring = CAN_BYTE_RING_INIT(g_web_output);

static bool io_lock_(void)
{
    return g_io_mutex && xSemaphoreTake(g_io_mutex, portMAX_DELAY) == pdTRUE;
}

static void web_rings_clear_(void)
{
    portENTER_CRITICAL(&g_web_lock);
    can_byte_ring_clear(&g_web_input_ring);
    can_byte_ring_clear(&g_web_output_ring);
    portEXIT_CRITICAL(&g_web_lock);
}

static void web_output_push_(const uint8_t *data, size_t len)
{
    portENTER_CRITICAL(&g_web_lock);
    can_byte_ring_append(&g_web_output_ring, data, len);
    portEXIT_CRITICAL(&g_web_lock);
}

static size_t web_input_take_(uint8_t *out, size_t cap)
{
    portENTER_CRITICAL(&g_web_lock);
    size_t n = can_byte_ring_read(&g_web_input_ring, out, cap);
    portEXIT_CRITICAL(&g_web_lock);
    return n;
}
static uint8_t                g_wifi_recent_tx[TERM_RX_PENDING_MAX];
static size_t                 g_wifi_recent_tx_len = 0;
static uint32_t               g_wifi_recent_tx_ms = 0;

/* ===== Terminal event queue from CAN to UART ===== */
typedef struct {
    uint8_t *p;
    uint16_t len;
    bool     is_echo;
} term_evt_t;

static QueueHandle_t g_q_term_evt = NULL;
static QueueHandle_t g_q_wifi_rx = NULL;
static inline void ui_log(bool to_can, const char *s);

static bool tasks_stop_requested_(void)
{
    return atomic_load_explicit(&g_tasks_stop, memory_order_acquire);
}

static bool handler_wait_tasks_stopped_(uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (atomic_load_explicit(&g_task_term_rx, memory_order_acquire) ||
           atomic_load_explicit(&g_task_pipe, memory_order_acquire)) {
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(HANDLER_TASK_STOP_POLL_MS));
    }
    return true;
}
static inline UBaseType_t term_evt_q_depth_(void)
{
    return g_q_term_evt ? uxQueueMessagesWaiting(g_q_term_evt) : 0;
}

static void term_evt_queue_drain_(void)
{
    term_evt_t evt;

    if (!g_q_term_evt) return;
    while (xQueueReceive(g_q_term_evt, &evt, 0) == pdTRUE) {
        if (evt.p) {
            vPortFree(evt.p);
        }
    }
}

static void wifi_rx_queue_drain_(void)
{
    term_evt_t evt;
    if (!g_q_wifi_rx) return;
    while (xQueueReceive(g_q_wifi_rx, &evt, 0) == pdTRUE) {
        if (evt.p) vPortFree(evt.p);
    }
}

static void wifi_recent_tx_record_(const uint8_t *data, size_t len)
{
    portENTER_CRITICAL(&g_web_lock);
    if (!data || !len) {
        g_wifi_recent_tx_len = 0;
        g_wifi_recent_tx_ms = 0;
    } else {
        if (len > sizeof(g_wifi_recent_tx)) len = sizeof(g_wifi_recent_tx);
        memcpy(g_wifi_recent_tx, data, len);
        g_wifi_recent_tx_len = len;
        g_wifi_recent_tx_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    }
    portEXIT_CRITICAL(&g_web_lock);
}

static bool wifi_recent_tx_matches_echo_(const uint8_t *data, uint16_t len)
{
    if (!data || !len) return false;
    for (uint16_t i = 0; i < len; ++i) {
        uint8_t c = data[i];
        if (!(c == '\r' || c == '\n' || c == '\t' || (c >= 32 && c < 127))) return false;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    portENTER_CRITICAL(&g_web_lock);
    bool match = g_wifi_recent_tx_len >= len && (now - g_wifi_recent_tx_ms) <= 2000u &&
        (memcmp(data, g_wifi_recent_tx + (g_wifi_recent_tx_len - len), len) == 0 ||
         memcmp(data, g_wifi_recent_tx, len) == 0);
    portEXIT_CRITICAL(&g_web_lock);
    return match;
}

/* ===== Log helpers (for L7/PIPE previews) ===== */
static inline void hex_dump_line(const uint8_t *p, size_t n, char *out, size_t out_sz) {
    size_t pos = 0;
    for (size_t i = 0; i < n && pos + 3 < out_sz; ++i) pos += snprintf(out + pos, out_sz - pos, "%02X ", p[i]);
    if (pos && pos < out_sz) out[pos - 1] = 0;
}
static inline void ascii_preview(const uint8_t *p, size_t n, char *out, size_t out_sz) {
    size_t k = n > 80 ? 80 : n;
    size_t pos = 0;
    for (size_t i = 0; i < k && pos + 1 < out_sz; ++i) {
        uint8_t c = p[i];
        char ch = (c >= 32 && c < 127) ? (char)c : '.';
        if (c == '\r') ch = 'r';
        else if (c == '\n') ch = 'n';
        out[pos++] = ch;
    }
    if (pos < out_sz) out[pos] = 0;
}

static inline void ui_log_pc_chunk_(const char *dir, const uint8_t *data, uint16_t len)
{
    char ascii[96] = {0};
    char line[128] = {0};

    if (!data || !len) return;
    ascii_preview(data, len, ascii, sizeof(ascii));
    snprintf(line, sizeof(line), "%s[%u] %s%s",
             dir ? dir : "?",
             (unsigned)len,
             ascii,
             (len > 80 ? "..." : ""));
    ui_log(false, line);
}

static inline void ui_log_can_chunk_(const char *dir, const uint8_t *data, uint16_t len)
{
    char hex[3 * 24 + 1] = {0};
    char line[128] = {0};
    size_t shown = len > 24 ? 24 : len;

    if (!data || !len) return;
    hex_dump_line(data, shown, hex, sizeof(hex));
    snprintf(line, sizeof(line), "%s[%u] %s%s",
             dir ? dir : "?",
             (unsigned)len,
             hex,
             (len > shown ? " ..." : ""));
    ui_log(true, line);
}

static void pipe_emit_locked_(const uint8_t *data, uint16_t len)
{
    if (!data || !len) return;

    if (g_term_owner == PROTOCOL_TERM_IO_WEB) {
        web_output_push_(data, len);
        return;
    }
    if (g_term_owner == PROTOCOL_TERM_IO_WIFI) {
        esp_err_t err = telnet_server_send(data, len);
        if (err != ESP_OK) {
            HLOGW(TAG, "PIPE write to Wi-Fi failed: %s", esp_err_to_name(err));
            return;
        }
#if BRIDGE_CAN_LOG_PIPE
        ESP_LOGI("PIPE", "PC<- %.*s", len, (const char*)data);
#endif
        if (g_log_cb) {
            ui_log_pc_chunk_("RX", data, len);
        }
        return;
    }

    for (uint16_t off = 0; off < len && !tasks_stop_requested_();) {
        const uint16_t chunk = (uint16_t)(((len - off) > PIPE_UART_CHUNK_MAX)
                                              ? PIPE_UART_CHUNK_MAX
                                              : (len - off));
        esp_err_t wr = ESP_FAIL;
        for (int attempt = 0; attempt < 3 && !tasks_stop_requested_(); ++attempt) {
            wr = serial_comm_write((const char *)data + off, chunk);
            if (wr == ESP_OK) break;
            /* RS-485 bus can be temporarily busy (read/write lock contention). */
            vTaskDelay(pdMS_TO_TICKS((attempt + 1) * 4));
        }
        if (wr != ESP_OK) {
            HLOGW(TAG, "PIPE write len=%u off=%u drop after retries: %s",
                  (unsigned)len, (unsigned)off, esp_err_to_name(wr));
            PCMTLOGW(TAG, "PCMT PIPE->PC write FAIL len=%u q=%u",
                     (unsigned)len, (unsigned)term_evt_q_depth_());
            return;
        }
        off = (uint16_t)(off + chunk);
    }
    PCMTLOGI(TAG, "PCMT PIPE->PC write OK len=%u q=%u",
             (unsigned)len, (unsigned)term_evt_q_depth_());

#if BRIDGE_CAN_LOG_PIPE
    ESP_LOGI("PIPE", "PC<- %.*s", len, (const char*)data);
#endif

    if (g_log_cb) {
        ui_log_pc_chunk_("RX", data, len);
    }
}

static void pipe_emit_to_uart_(const uint8_t *data, uint16_t len)
{
    if (!io_lock_()) return;
    if (g_handler_ready) pipe_emit_locked_(data, len);
    xSemaphoreGive(g_io_mutex);
}

#if BRIDGE_CAN_LOG_L3
static const char *tr_evt_str_(sp_event_t ev)
{
    switch (ev) {
    case SP_EVT_L2_RX_DROPPED: return "L2_RX_DROPPED";
    case SP_EVT_L2_TX_DROPPED: return "L2_TX_DROPPED";
    case SP_EVT_FP_TIMEOUT:    return "FP_TIMEOUT";
    case SP_EVT_FP_LOST_SEQ:   return "FP_LOST_SEQ";
    case SP_EVT_FP_OVERLEN:    return "FP_OVERLEN";
    default:                   return "EVT_UNKNOWN";
    }
}

static void tr_evt_log_cb_(sp_event_t evt, const sp_id_fields_t *id, uint8_t sid, void *user)
{
    (void)user;
    if (id) {
        L3LOGW(TAG, "TR EVT: %s sid=%u id=%02X%01X%02X%02X",
               tr_evt_str_(evt), (unsigned)sid,
               (unsigned)id->pri, (unsigned)id->dp,
               (unsigned)id->pf, (unsigned)id->ps);
    } else {
        L3LOGW(TAG, "TR EVT: %s sid=%u", tr_evt_str_(evt), (unsigned)sid);
    }
}
#else
static void tr_evt_log_cb_(sp_event_t evt, const sp_id_fields_t *id, uint8_t sid, void *user)
{
    (void)evt;
    (void)id;
    (void)sid;
    (void)user;
}
#endif

/* ===== UI ===== */
#if BRIDGE_CAN_LOG_HANDLER
static const char* proto_state_str(protocol_link_state_t s) {
    switch (s) {
        case PROTO_STATE_BOOT:   return "BOOT";
        case PROTO_STATE_ERROR:  return "ERROR";
        case PROTO_STATE_ONLINE: return "ONLINE";
        default:                 return "UNKNOWN";
    }
}
#endif
static inline void ui_log(bool to_can, const char *s) {
    if (!s) return;
    portENTER_CRITICAL(&g_ui_lock);
    protocol_log_cb_t cb = g_log_cb;
    void *user = g_ui_user;
    if (cb) atomic_fetch_add(&g_ui_inflight, 1u);
    portEXIT_CRITICAL(&g_ui_lock);
    if (cb) { cb(to_can, s, user); atomic_fetch_sub(&g_ui_inflight, 1u); }
}
static inline void ui_state(protocol_link_state_t st) {
    portENTER_CRITICAL(&g_ui_lock);
    protocol_state_cb_t cb = g_state_cb;
    void *user = g_ui_user;
    if (cb) atomic_fetch_add(&g_ui_inflight, 1u);
    portEXIT_CRITICAL(&g_ui_lock);
    if (cb) { cb(st, user); atomic_fetch_sub(&g_ui_inflight, 1u); }
}
static inline void set_state(protocol_link_state_t st) {
    if (g_state != st) {
        HLOGI(TAG, "STATE: %s -> %s", proto_state_str(g_state), proto_state_str(st));
        g_state = st;
        g_term_ready_seen = (st == PROTO_STATE_ONLINE);
        if (st != PROTO_STATE_ONLINE) {
            atomic_fetch_add(&g_io_generation, 1u);
            portENTER_CRITICAL(&g_web_lock);
            can_byte_ring_clear(&g_web_input_ring);
            portEXIT_CRITICAL(&g_web_lock);
            wifi_rx_queue_drain_();
        }
        ui_state(st);
    }
}

/* ===== CAN bring-up ===== */
static bool bringup_can_(uint32_t bitrate) {
    can_config_t cc = {
        .nom_speed   = bitrate ? bitrate : 250000,
        .listen_only = false
    };
    esp_err_t err = can_driver_init(&cc);
    if (err != ESP_OK) {
        HLOGE(TAG, "CAN init failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* ======================================================================================
 * NDP TERMINAL: RX callback (CAN -> queue -> PIPE -> UART), with no protocol logic.
 * ====================================================================================*/
static bool term_on_ndp_rx_(const uint8_t *data, uint16_t len, void *user)
{
    (void)user;
    if (!data || !len || !g_handler_ready) return false;
    const bool is_echo = wifi_recent_tx_matches_echo_(data, len);
    if (!is_echo) g_term_ready_seen = true;
    if (g_term_owner != PROTOCOL_TERM_IO_UART &&
        is_echo) {
        return true;
    }

#if BRIDGE_CAN_LOG_L7
    char ascii[96]={0}, hex[3*64+4]={0};
    ascii_preview(data, len, ascii, sizeof(ascii));
    hex_dump_line(data, len > 64 ? 64 : len, hex, sizeof(hex));
    L7LOGI(TAG, "L7 CAN->TERM len=%u %s | \"%s\"%s | %s%s",
           len, (is_echo?"(echo)":"(block)"), ascii, (len>80?"...":""), hex, (len>64?" ...":""));
#endif

    if (g_log_cb) ui_log_can_chunk_(is_echo ? "RXE" : "RX", data, len);

    uint8_t *cp = (uint8_t*)pvPortMalloc(len);
    if (!cp) {
        HLOGE(TAG, "No mem for term evt (%u)", (unsigned)len);
        return false;
    }
    memcpy(cp, data, len);
    term_evt_t evt = { .p = cp, .len = len, .is_echo = is_echo };
    if (!g_q_term_evt || xQueueSend(g_q_term_evt, &evt, pdMS_TO_TICKS(TERM_EVT_ENQ_WAIT_MS)) != pdTRUE) {
        vPortFree(cp);
        HLOGW(TAG, "term evt queue full (retry %u)", (unsigned)len);
        PCMTLOGW(TAG, "PCMT CAN->PIPE backpressure len=%u echo=%u q=%u",
                 (unsigned)len, (unsigned)is_echo, (unsigned)term_evt_q_depth_());
        return false;
    } else {
        PCMTLOGI(TAG, "PCMT CAN->PIPE enqueue len=%u echo=%u q=%u",
                 (unsigned)len, (unsigned)is_echo, (unsigned)term_evt_q_depth_());
    }
    return true;
}

/* ======================================================================================
 * PIPE task: CAN to UART (drains the queue into UART)
 * ====================================================================================*/
static void pipe_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    HLOGI(TAG, "PIPE task start");
    while (!tasks_stop_requested_()) {
        size_t bytes_sent = 0;
        uint32_t evt_sent = 0;

        term_evt_t evt;
        while (g_q_term_evt &&
               evt_sent < PIPE_MAX_EVT_PER_TICK &&
               bytes_sent < PIPE_MAX_BYTES_PER_TICK &&
               xQueueReceive(g_q_term_evt, &evt, 0) == pdTRUE) {
            if (evt.p && evt.len) {
                PCMTLOGI(TAG, "PCMT PIPE dequeue len=%u echo=%u q=%u",
                         (unsigned)evt.len, (unsigned)evt.is_echo, (unsigned)term_evt_q_depth_());
#if BRIDGE_CAN_LOG_L7
                char ascii[96]={0}, hex[3*64+4]={0};
                ascii_preview(evt.p, evt.len, ascii, sizeof(ascii));
                hex_dump_line(evt.p, evt.len > 64 ? 64 : evt.len, hex, sizeof(hex));
                L7LOGI(TAG, "PIPE: push len=%u | \"%s\"%s | %s%s",
                       evt.len, ascii, (evt.len>80?"...":""), hex, (evt.len>64?" ...":""));
#endif
                pipe_emit_to_uart_(evt.p, evt.len);
                bytes_sent += evt.len;
                evt_sent++;
            }
            if (evt.p) vPortFree(evt.p);
        }

        char line[OUTPUT_BUFFER_SIZE];
        uint16_t len = 0;
        while (bytes_sent < PIPE_MAX_BYTES_PER_TICK &&
               buffer_manager_get_output(line, sizeof(line), &len)) {
            if (len) {
                PCMTLOGI(TAG, "PCMT BM->PC len=%u q=%u", (unsigned)len, (unsigned)term_evt_q_depth_());
                pipe_emit_to_uart_((const uint8_t *)line, len);
                bytes_sent += len;
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PIPE_TICK_MS));
    }
    atomic_store_explicit(&g_task_pipe, NULL, memory_order_release);
    vTaskDelete(NULL);
}

static void term_wifi_rx_cb_(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    if (!data || !len || len > TERM_RX_PENDING_MAX || !io_lock_()) return;
    if (!g_handler_ready || g_state != PROTO_STATE_ONLINE || g_term_owner != PROTOCOL_TERM_IO_WIFI || !g_q_wifi_rx) {
        xSemaphoreGive(g_io_mutex);
        return;
    }
    uint8_t *cp = (uint8_t *)pvPortMalloc(len);
    if (cp) {
        memcpy(cp, data, len);
        term_evt_t evt = { .p = cp, .len = (uint16_t)len, .is_echo = false };
        if (xQueueSend(g_q_wifi_rx, &evt, 0) != pdTRUE) vPortFree(cp);
    }
    xSemaphoreGive(g_io_mutex);
}

/* ======================================================================================
 * UART to CAN: read UART at 50 Hz and send blocks through the NDP terminal session (FSM handles addressing)
 * ====================================================================================*/
static void term_rx_task(void *arg)
{
    (void)arg;
    HLOGI(TAG, "TERM RX task start (50 Hz)");

    TickType_t last_wake = xTaskGetTickCount();
    uint8_t pending[TERM_RX_PENDING_MAX];
    size_t  pending_len = 0;
    uint32_t pending_last_rx_ms = 0;
    uint32_t wait_credit_log_ms = 0;
    uint32_t io_generation = atomic_load(&g_io_generation);

    while (!tasks_stop_requested_()) {
        uint32_t current_generation = atomic_load(&g_io_generation);
        if (current_generation != io_generation) {
            pending_len = 0;
            pending_last_rx_ms = 0;
            io_generation = current_generation;
        }
        if (sp_fsm_get_state() != SP_ST_ONLINE || !g_term_ready_seen) {
            /* A closed transport epoch cannot retain unsubmitted input. */
            pending_len = 0;
            pending_last_rx_ms = 0;
            (void)serial_comm_flush_input();
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TERM_RX_TICK_MS));
            continue;
        }

        uint8_t buf[TERM_RX_MAX_PER_TICK];
        if (g_term_owner == PROTOCOL_TERM_IO_WEB) {
            size_t n = 0;
            if (io_lock_()) {
                if (io_generation == atomic_load(&g_io_generation)) {
                    n = web_input_take_(&pending[pending_len], sizeof(pending) - pending_len);
                }
                xSemaphoreGive(g_io_mutex);
            }
            if (n) {
                pending_len += n;
                pending_last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            }
        } else if (g_term_owner == PROTOCOL_TERM_IO_WIFI) {
            term_evt_t evt;
            while (pending_len < sizeof(pending) &&
                   g_q_wifi_rx &&
                   xQueueReceive(g_q_wifi_rx, &evt, 0) == pdTRUE) {
                size_t room = sizeof(pending) - pending_len;
                size_t take = (evt.len <= room) ? evt.len : room;
                if (take > 0) {
                    memcpy(&pending[pending_len], evt.p, take);
                    pending_len += take;
                    pending_last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                }
                if (evt.p) vPortFree(evt.p);
                if (take == 0) break;
            }
        } else {
            size_t avail_now = serial_comm_available();
            if (avail_now > 0) {
                size_t n = sizeof(buf);
                if (n > avail_now) n = avail_now;
                if (n == 0) n = 1;

                esp_err_t er = serial_comm_read(buf, &n);
                if (er != ESP_OK || n == 0) {
                    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TERM_RX_TICK_MS));
                    continue;
                }

                size_t room = sizeof(pending) - pending_len;
                size_t take = (n <= room) ? n : room;
                if (take > 0) {
                    memcpy(&pending[pending_len], buf, take);
                    pending_len += take;
                    pending_last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                    PCMTLOGI(TAG, "PCMT PC->CAN rx take=%u pending=%u avail=%u",
                             (unsigned)take, (unsigned)pending_len, (unsigned)avail_now);
                }
                if (take < n) {
                    HLOGW(TAG, "TERM RX overflow: dropped %u bytes", (unsigned)(n - take));
                    PCMTLOGW(TAG, "PCMT PC->CAN overflow drop=%u pending=%u",
                             (unsigned)(n - take), (unsigned)pending_len);
                }

                while (pending_len < sizeof(pending)) {
                    size_t avail = serial_comm_available();
                    if (avail == 0) break;

                    size_t more = sizeof(buf);
                    size_t left = sizeof(pending) - pending_len;
                    if (more > left) more = left;
                    if (more > avail) more = avail;
                    if (more == 0) break;

                    esp_err_t er_more = serial_comm_read(buf, &more);
                    if (er_more != ESP_OK || more == 0) break;

                    memcpy(&pending[pending_len], buf, more);
                    pending_len += more;
                    pending_last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                }
            }
        }

        if (pending_len > 0 && sp_fsm_term_input_can_send()) {
            bool flush_now = true;

            if (g_term_owner != PROTOCOL_TERM_IO_UART) {
                const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                const bool have_eol = term_rx_has_line_end_(pending, pending_len);
                const bool idle_flush = pending_last_rx_ms != 0 &&
                                        (now_ms - pending_last_rx_ms) >= TERM_RX_IDLE_FLUSH_MS;
                const bool full_flush = pending_len >= TERM_RX_MAX_PER_TICK;
                flush_now = have_eol || idle_flush || full_flush;
            }

            if (!flush_now) {
                vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TERM_RX_TICK_MS));
                continue;
            }

            size_t tx_len = (pending_len > TERM_RX_MAX_PER_TICK) ? TERM_RX_MAX_PER_TICK : pending_len;
            if (g_term_owner == PROTOCOL_TERM_IO_UART) {
                tx_len = 1;
            }
#if BRIDGE_CAN_LOG_L7
            char ascii[96] = {0}, hex[3 * 64 + 4] = {0};
            ascii_preview(pending, tx_len, ascii, sizeof(ascii));
            hex_dump_line(pending, tx_len > 64 ? 64 : tx_len, hex, sizeof(hex));
            L7LOGI(TAG, "L7 TERM RX block: len=%u | \"%s\"%s | %s%s",
                   (unsigned)tx_len, ascii, (tx_len > 80 ? "..." : ""),
                   hex, (tx_len > 64 ? " ..." : ""));
#endif
            PCMTLOGI(TAG, "PCMT PC->CAN enqueue len=%u pending=%u",
                     (unsigned)tx_len, (unsigned)pending_len);
            /* Revoke waits for an already started write, while unstarted bytes
             * from the previous browser/Telnet owner are discarded. */
            if (!io_lock_()) continue;
            if (!g_handler_ready || io_generation != atomic_load(&g_io_generation)) {
                pending_len = 0;
                pending_last_rx_ms = 0;
                xSemaphoreGive(g_io_mutex);
                continue;
            }
            sp_err_t se = sp_fsm_term_send(pending, (uint16_t)tx_len);
            if (se != SP_OK) {
                HLOGW(TAG, "term_send(len=%u) failed: %d", (unsigned)tx_len, (int)se);
                PCMTLOGW(TAG, "PCMT PC->CAN enqueue FAIL len=%u err=%d pending=%u",
                         (unsigned)tx_len, (int)se, (unsigned)pending_len);
            } else {
                if (g_term_owner != PROTOCOL_TERM_IO_UART) {
                    wifi_recent_tx_record_(pending, tx_len);
                }
                if (g_log_cb) {
                    ui_log_can_chunk_("TX", pending, (uint16_t)tx_len);
                    ui_log_pc_chunk_("TX", pending, (uint16_t)tx_len);
                }
                pending_len -= tx_len;
                if (pending_len > 0) {
                    memmove(pending, pending + tx_len, pending_len);
                } else {
                    pending_last_rx_ms = 0;
                }
                PCMTLOGI(TAG, "PCMT PC->CAN queued pending=%u",
                         (unsigned)pending_len);
            }
            xSemaphoreGive(g_io_mutex);
        } else if (pending_len > 0) {
            const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            if ((now_ms - wait_credit_log_ms) >= 500u) {
                wait_credit_log_ms = now_ms;
                PCMTLOGW(TAG, "PCMT wait-credit pending=%u buffered=%u",
                         (unsigned)pending_len, (unsigned)serial_comm_available());
            }
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TERM_RX_TICK_MS));
    }
    atomic_store_explicit(&g_task_term_rx, NULL, memory_order_release);
    vTaskDelete(NULL);
}

/* ======================================================================================
 * FSM worker callback (blocking protocol work runs outside ESP_TIMER_TASK)
 * ====================================================================================*/
static void fsm_poll(void)
{
    sp_fsm_tick();

    sp_state_t st = sp_fsm_get_state();
    protocol_link_state_t ps =
        (st == SP_ST_ONLINE)   ? PROTO_STATE_ONLINE :
        (st == SP_ST_OFF)      ? PROTO_STATE_ERROR  : PROTO_STATE_BOOT;

#if BRIDGE_CAN_LOG_HANDLER
    static sp_state_t prev_st = (sp_state_t)-1;
    if (st != prev_st) {
        HLOGI(TAG, "FSM: %d -> proto=%d", (int)st, (int)ps);
        prev_st = st;
    }
#endif

    if (ps != g_state) {
        set_state(ps);
    }
}

/* ======================================================================================
 * PUBLIC API
 * ====================================================================================*/
void protocol_handler_set_ui(protocol_log_cb_t log_cb, protocol_state_cb_t state_cb, void *user)
{
    portENTER_CRITICAL(&g_ui_lock);
    g_log_cb = NULL; g_state_cb = NULL;
    portEXIT_CRITICAL(&g_ui_lock);
    /* Join callbacks that already copied the old user pointer before replacing it. */
    while (atomic_load(&g_ui_inflight)) vTaskDelay(pdMS_TO_TICKS(1));
    portENTER_CRITICAL(&g_ui_lock);
    g_ui_user = user; g_log_cb = log_cb; g_state_cb = state_cb;
    portEXIT_CRITICAL(&g_ui_lock);
}

bool protocol_handler_init(const protocol_handler_cfg_t *cfg)
{
    if (!cfg || g_can_owned || g_serial_owned) return false;
    if (!g_io_mutex) {
        g_io_mutex = xSemaphoreCreateMutex();
        if (!g_io_mutex) return false;
    }
    g_cfg = *cfg;
    g_rs_baudrate = cfg->rs_baudrate;
    web_rings_clear_();
    atomic_fetch_add(&g_io_generation, 1u);

    if (!buffer_manager_init()) {
        HLOGE(TAG, "Buffer manager init failed");
        return false;
    }

    HLOGI(TAG, "SERIAL init: baud=%u", g_cfg.rs_baudrate);
    if (serial_comm_init(g_cfg.rs_baudrate) != ESP_OK) {
        HLOGE(TAG, "Serial init failed"); buffer_manager_deinit(); return false;
    }

    g_serial_owned = true;

    /* === CAN up === */
    if (!bringup_can_(g_cfg.can_bitrate)) {
        if (serial_comm_deinit() == ESP_OK) g_serial_owned = false;
        buffer_manager_deinit();
        return false;
    }
    g_can_owned = true;

    /* TT6006 service profile owns discovery and both NDP client sessions. */
    {
        /* Address claim and service discovery are owned by this profile. */
        sp_fsm_config_t fcfg = { .local_sa = 0x00 };
        if (sp_fsm_init(&fcfg) != SP_OK) {
            HLOGE(TAG, "TT6006 profile init failed"); goto fail_can;
        }
        sp_fsm_set_term_rx(term_on_ndp_rx_, NULL);

        /* Unfiltered CAN RX, bounded FP assembly and serialized NDP delivery. */
        sp_l2_config_t  l2cfg = { .use_hw_filters = false };
        sp_timing_t     trtim = { .fp_block_timeout_ms = 850 };
        const uint16_t  tr_slots = BRIDGE_CAN_TR_MAX_SLOTS;

        if (sp_fsm_bind_layers(&l2cfg, &trtim, tr_slots,
                               /*on_l2_evt*/NULL, /*on_tr_evt*/tr_evt_log_cb_, /*user*/NULL) != SP_OK) {
            HLOGE(TAG, "FSM bind layers failed"); goto fail_fsm;
        }
    }

    /* Terminal event queue */
    g_q_term_evt = xQueueCreate(BRIDGE_CAN_Q_TERM_EVT_LEN, sizeof(term_evt_t));
    if (!g_q_term_evt) { HLOGE(TAG, "term evt queue create failed"); goto fail_bind; }
    g_q_wifi_rx = xQueueCreate(32, sizeof(term_evt_t));
    if (!g_q_wifi_rx) { HLOGE(TAG, "wifi rx queue create failed"); goto fail_q_term; }
    telnet_router_register(TELNET_ROUTE_BRIDGE, term_wifi_rx_cb_, NULL);

    set_state(PROTO_STATE_BOOT);
    wifi_recent_tx_record_(NULL, 0);
    g_term_owner = (wifi_ap_is_enabled() && telnet_server_is_running())
        ? PROTOCOL_TERM_IO_WIFI
        : PROTOCOL_TERM_IO_UART;
    telnet_router_set_active(g_term_owner == PROTOCOL_TERM_IO_WIFI
                                 ? TELNET_ROUTE_BRIDGE
                                 : TELNET_ROUTE_NONE);
    g_handler_ready = true;
    HLOGI(TAG, "TERM I/O owner at init: %s",
          g_term_owner == PROTOCOL_TERM_IO_WIFI ? "WIFI" : "UART");
    HLOGI(TAG, "Init done: CAN=%u, RS=%u", g_cfg.can_bitrate, g_cfg.rs_baudrate);
    return true;

/* ----- error path: the regular stop joins producers before releasing memory ----- */
fail_q_term:
fail_bind:
fail_fsm:
fail_can:
    (void)protocol_handler_deinit();
    return false;
}

bool protocol_handler_start(void)
{
    if (!g_handler_ready) return false;
    BaseType_t rc;
    TaskHandle_t created_task = NULL;

    atomic_store_explicit(&g_tasks_stop, false, memory_order_release);

    if (!atomic_load_explicit(&g_task_pipe, memory_order_acquire)) {
        rc = xTaskCreatePinnedToCore(
            pipe_task, "pipe",
            BRIDGE_CAN_STACK_PUMP,
            NULL, BRIDGE_CAN_PRIO_PUMP,
            &created_task, BRIDGE_CAN_CORE_BG
        );
        if (rc != pdPASS) {
            HLOGE(TAG, "Failed to create pipe task (stack=%u, prio=%u, core=%d)",
                  (unsigned)BRIDGE_CAN_STACK_PUMP, (unsigned)BRIDGE_CAN_PRIO_PUMP, BRIDGE_CAN_CORE_BG);
            goto fail_started;
        }
        atomic_store_explicit(&g_task_pipe, created_task, memory_order_release);
    }

    if (!atomic_load_explicit(&g_task_term_rx, memory_order_acquire)) {
        created_task = NULL;
        rc = xTaskCreatePinnedToCore(
            term_rx_task, "term_rx",
            BRIDGE_CAN_STACK_TERMINAL,
            NULL, BRIDGE_CAN_PRIO_TERMINAL,
            &created_task, BRIDGE_CAN_CORE_TERM
        );
        if (rc != pdPASS) {
            HLOGE(TAG, "Failed to create term_rx task (stack=%u, prio=%u, core=%d)",
                  (unsigned)BRIDGE_CAN_STACK_TERMINAL, (unsigned)BRIDGE_CAN_PRIO_TERMINAL, BRIDGE_CAN_CORE_TERM);
            goto fail_started;
        }
        atomic_store_explicit(&g_task_term_rx, created_task, memory_order_release);
    }

    if (!protocol_poll_worker_start(fsm_poll)) {
        HLOGE(TAG, "Failed to create CAN FSM worker");
        goto fail_started;
    }

    HLOGI(TAG, "Started");
    return true;

fail_started:
    /* Also revoke readiness if a worker cannot be joined; a later start must
     * never clear the stop flag and revive producers from a failed attempt. */
    (void)protocol_handler_stop();
    return false;
}

bool protocol_handler_stop(void)
{
    g_handler_ready = false;
    atomic_fetch_add(&g_io_generation, 1u);
    /* Drain a copied Wi-Fi callback before deleting its destination queue. */
    telnet_router_unregister(TELNET_ROUTE_BRIDGE);
    wifi_recent_tx_record_(NULL, 0);
    atomic_store_explicit(&g_tasks_stop, true, memory_order_release);
    /* Request every producer to stop before waiting on any one of them. */
    (void)protocol_poll_worker_stop(0);
    if (!handler_wait_tasks_stopped_(HANDLER_TASK_STOP_WAIT_MS)) {
        HLOGE(TAG, "Worker task stop timed out; runtime kept alive");
        return false;
    }

    if (!protocol_poll_worker_stop(HANDLER_TASK_STOP_WAIT_MS)) {
        HLOGE(TAG, "CAN FSM worker stop timed out; runtime kept alive");
        return false;
    }
    sp_fsm_deinit(); /* Close NDP while CAN is still available, then stop its RX producer. */
    if (!sp_fsm_unbind_layers()) return false; /* join CAN RX before freeing its output queue */

    if (g_q_term_evt) {
        term_evt_queue_drain_();
        vQueueDelete(g_q_term_evt);
        g_q_term_evt = NULL;
    }
    if (g_q_wifi_rx) {
        wifi_rx_queue_drain_();
        vQueueDelete(g_q_wifi_rx);
        g_q_wifi_rx = NULL;
    }

    esp_err_t can_err = g_can_owned ? can_driver_deinit() : ESP_OK;
    if (can_err != ESP_OK) {
        HLOGE(TAG, "CAN stop failed: %s", esp_err_to_name(can_err));
        return false;
    }

    g_can_owned = false;
    web_rings_clear_();
    set_state(PROTO_STATE_BOOT);
    atomic_store_explicit(&g_tasks_stop, false, memory_order_release);
    HLOGI(TAG, "Stopped");
    return true;
}

bool protocol_handler_deinit(void)
{
    if (!protocol_handler_stop()) return false;

    if (g_serial_owned) {
        if (serial_comm_deinit() != ESP_OK) return false;
        g_serial_owned = false;
    }
    buffer_manager_deinit();
    HLOGI(TAG, "Deinit");
    return true;
}

esp_err_t protocol_handler_set_rs_baudrate(uint32_t baud)
{
    esp_err_t err;

    if (baud == 0) return ESP_ERR_INVALID_ARG;
    if (!io_lock_()) return ESP_ERR_INVALID_STATE;
    if (!g_handler_ready || !g_serial_owned) { xSemaphoreGive(g_io_mutex); return ESP_ERR_INVALID_STATE; }
    err = serial_comm_set_baudrate(baud);
    if (err == ESP_OK) {
        g_cfg.rs_baudrate = baud;
        g_rs_baudrate = baud;
        HLOGI(TAG, "SERIAL baud changed on-the-fly: %u", (unsigned)baud);
    } else {
        HLOGW(TAG, "SERIAL baud change failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(g_io_mutex);
    return err;
}

void protocol_handler_set_term_io_owner(protocol_term_io_t owner)
{
    if (owner != PROTOCOL_TERM_IO_UART && owner != PROTOCOL_TERM_IO_WIFI && owner != PROTOCOL_TERM_IO_WEB) return;
    bool locked = io_lock_();
    if (g_term_owner != owner) {
        g_term_owner = owner;
        atomic_fetch_add(&g_io_generation, 1u);
        sp_fsm_term_cancel_pending();
        web_rings_clear_();
        wifi_recent_tx_record_(NULL, 0);
        if (g_handler_ready) {
            (void)serial_comm_flush_input();
            wifi_rx_queue_drain_();
        }
    }
    if (g_handler_ready) {
        telnet_router_set_active(owner == PROTOCOL_TERM_IO_WIFI ? TELNET_ROUTE_BRIDGE : TELNET_ROUTE_NONE);
    }
    if (locked) xSemaphoreGive(g_io_mutex);
}

protocol_term_io_t protocol_handler_get_term_io_owner(void)
{
    return g_term_owner;
}

bool protocol_handler_is_term_ready(void)
{
    return g_handler_ready && (g_state == PROTO_STATE_ONLINE) && g_term_ready_seen;
}

bool protocol_handler_is_running(void) { return g_can_owned || g_serial_owned; }
bool protocol_handler_is_started(void)
{
    return g_handler_ready && atomic_load(&g_task_pipe) && atomic_load(&g_task_term_rx) &&
        protocol_poll_worker_is_running();
}
protocol_link_state_t protocol_handler_get_state(void) { return g_state; }
uint32_t protocol_handler_get_rs_baudrate(void)
{
    return g_rs_baudrate;
}

esp_err_t protocol_handler_web_write(const uint8_t *data, size_t len)
{
    if (!data || !len || len > sizeof(g_web_input)) return ESP_ERR_INVALID_ARG;
    if (!io_lock_()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&g_web_lock);
    esp_err_t err = ESP_OK;
    if (!g_handler_ready || g_term_owner != PROTOCOL_TERM_IO_WEB || !g_term_ready_seen || g_state != PROTO_STATE_ONLINE) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!can_byte_ring_write(&g_web_input_ring, data, len)) {
        err = ESP_ERR_NO_MEM;
    }
    portEXIT_CRITICAL(&g_web_lock);
    xSemaphoreGive(g_io_mutex);
    return err;
}

size_t protocol_handler_web_drain(uint8_t *out, size_t cap)
{
    if (!out || !cap) return 0;
    portENTER_CRITICAL(&g_web_lock);
    size_t n = can_byte_ring_read(&g_web_output_ring, out, cap);
    portEXIT_CRITICAL(&g_web_lock);
    return n;
}

void protocol_handler_web_stats(size_t *pending, uint32_t *dropped)
{
    portENTER_CRITICAL(&g_web_lock);
    if (pending) *pending = g_web_output_ring.count;
    if (dropped) *dropped = g_web_output_ring.dropped;
    portEXIT_CRITICAL(&g_web_lock);
}

/* External terminal input uses the same reliable NDP queue as UART/web. */
bool protocol_handler_send_ascii(const uint8_t *data, size_t len)
{
    if (!data || !len) { HLOGW(TAG, "Invalid data or len"); return false; }


    size_t off = 0;
    while (off < len) {
        const size_t chunk = (len - off > TERM_RX_MAX_PER_TICK) ? TERM_RX_MAX_PER_TICK : (len - off);
        sp_err_t se = sp_fsm_term_send(data + off, (uint16_t)chunk);
        if (se != SP_OK) {
            HLOGW(TAG, "Send chunk off=%u len=%u failed: %d",
                  (unsigned)off, (unsigned)chunk, (int)se);
            return false;
        }

        if (g_log_cb) {
            ui_log_can_chunk_("TX", data + off, (uint16_t)chunk);
            ui_log_pc_chunk_("TX", data + off, (uint16_t)chunk);
        }
        buffer_manager_put_output((const char *)(data + off), (uint16_t)chunk);
        off += chunk;
    }

#if BRIDGE_CAN_L7_APPEND_CRLF
    buffer_manager_put_output("\r\n", 2);
#endif
    return true;
}


void protocol_handler_get_antenna_status(protocol_antenna_status_t *out)
{
    if(!out) return;
    sp_fsm_get_antenna_status(out);
    if(!g_handler_ready) {
        out->online=false; out->signal_valid=false; out->position_fresh=false;
    }
}
