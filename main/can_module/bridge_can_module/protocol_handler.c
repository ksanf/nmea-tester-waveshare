/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   UART<->CAN infrastructure handler (bring-up/tasks/timers/queues), without protocol logic.
 */

#include "protocol_handler.h"
#include "bridge_can_config.h"
#include "can_driver.h"

/* Stack and upper layers */
#include "sailor_proto_common.h"
#include "l5_terminal.h"        /* Terminal tunnel (L7 over L3L4) */
#include "l5_service.h"
#include "fsm.h"                /* FSM owns the L2, L3L4, and TLV layers */
#include "fsm_actions.h"        /* TX primitives for the FSM */

/* Types used by bind_layers configuration; no protocol logic lives here. */
#include "l2_link.h"            /* sp_l2_config_t */
#include "l3l4_transport.h"     /* sp_timing_t */
#include "tlv_codec.h"          /* sp_tlv_config_t */

#include "serial_comm.h"
#include "buffer_manager.h"
#include "config_nmea_tester.h"
#include "system/telnet_router.h"
#include "system/telnet_server.h"
#include "system/wifi_ap.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdatomic.h>

#define TAG "PROTO"

/* ===== Periods and timers ===== */
#define FSM_POLL_PERIOD_US    (20 * 1000)   /* 20 ms */
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

static inline uint8_t term_input_series_next_(uint8_t cur)
{
    if (cur < 0x08u || cur > 0x0Fu) return 0x08u;
    return (cur == 0x0Fu) ? 0x08u : (uint8_t)(cur + 1u);
}

static bool term_rx_has_line_end_(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == '\r' || buf[i] == '\n') return true;
    }
    return false;
}

/* ===== UI and state ===== */
static protocol_log_cb_t   g_log_cb   = NULL;
static protocol_state_cb_t g_state_cb = NULL;
static void               *g_ui_user  = NULL;
static volatile protocol_link_state_t g_state = PROTO_STATE_BOOT;

/* ===== Configuration and resources ===== */
static protocol_handler_cfg_t g_cfg;
static _Atomic(TaskHandle_t)  g_task_term_rx = NULL;
static _Atomic(TaskHandle_t)  g_task_pipe    = NULL;
static atomic_bool            g_tasks_stop = ATOMIC_VAR_INIT(false);
static esp_timer_handle_t     g_fsm_timer    = NULL;
static volatile bool          g_term_ready_seen = false;
static volatile protocol_term_io_t g_term_owner = PROTOCOL_TERM_IO_UART;
static volatile bool          g_handler_ready = false;
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
    if (!data || len == 0) {
        g_wifi_recent_tx_len = 0;
        g_wifi_recent_tx_ms = 0;
        return;
    }
    if (len > sizeof(g_wifi_recent_tx)) len = sizeof(g_wifi_recent_tx);
    memcpy(g_wifi_recent_tx, data, len);
    g_wifi_recent_tx_len = len;
    g_wifi_recent_tx_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool wifi_recent_tx_matches_echo_(const uint8_t *data, uint16_t len)
{
    uint32_t now_ms;

    if (!data || len == 0 || g_wifi_recent_tx_len == 0) return false;
    if (len > g_wifi_recent_tx_len) return false;

    now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if ((now_ms - g_wifi_recent_tx_ms) > 2000u) return false;

    for (uint16_t i = 0; i < len; ++i) {
        uint8_t c = data[i];
        if (!(c == '\r' || c == '\n' || c == '\t' || (c >= 32 && c < 127))) {
            return false;
        }
    }

    if (memcmp(data, g_wifi_recent_tx + (g_wifi_recent_tx_len - len), len) == 0) {
        return true;
    }
    if (memcmp(data, g_wifi_recent_tx, len) == 0) {
        return true;
    }
    return false;
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

static void pipe_emit_to_uart_(const uint8_t *data, uint16_t len)
{
    if (!data || !len) return;

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
    if (g_log_cb && s) g_log_cb(to_can, s, g_ui_user);
}
static inline void ui_state(protocol_link_state_t st) {
    if (g_state_cb) g_state_cb(st, g_ui_user);
}
static inline void set_state(protocol_link_state_t st) {
    if (g_state != st) {
        HLOGI(TAG, "STATE: %s -> %s", proto_state_str(g_state), proto_state_str(st));
        g_state = st;
        if (st != PROTO_STATE_ONLINE) g_term_ready_seen = false;
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
 * L5 TERMINAL: RX callback (CAN -> queue -> PIPE -> UART). No protocol logic.
 * ====================================================================================*/
static void term_on_l5_rx_(const uint8_t *data, uint16_t len, bool is_echo, void *user)
{
    (void)user;
    if (!data || !len) return;
    if (!is_echo) g_term_ready_seen = true;
    if (g_term_owner == PROTOCOL_TERM_IO_WIFI &&
        (is_echo || wifi_recent_tx_matches_echo_(data, len))) {
        return;
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
    if (!cp) { HLOGE(TAG, "No mem for term evt (%u)", (unsigned)len); return; }
    memcpy(cp, data, len);
    term_evt_t evt = { .p = cp, .len = len, .is_echo = is_echo };
    if (!g_q_term_evt || xQueueSend(g_q_term_evt, &evt, pdMS_TO_TICKS(TERM_EVT_ENQ_WAIT_MS)) != pdTRUE) {
        vPortFree(cp);
        HLOGW(TAG, "term evt queue full (drop %u)", (unsigned)len);
        PCMTLOGW(TAG, "PCMT CAN->PIPE enqueue DROP len=%u echo=%u q=%u",
                 (unsigned)len, (unsigned)is_echo, (unsigned)term_evt_q_depth_());
    } else {
        PCMTLOGI(TAG, "PCMT CAN->PIPE enqueue len=%u echo=%u q=%u",
                 (unsigned)len, (unsigned)is_echo, (unsigned)term_evt_q_depth_());
    }
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
    if (!data || len == 0 || !g_q_wifi_rx) return;

    uint8_t *cp = (uint8_t *)pvPortMalloc(len);
    if (!cp) return;
    memcpy(cp, data, len);

    term_evt_t evt = {
        .p = cp,
        .len = (uint16_t)len,
        .is_echo = false,
    };
    if (xQueueSend(g_q_wifi_rx, &evt, 0) != pdTRUE) {
        vPortFree(cp);
    }
}

/* ======================================================================================
 * UART to CAN: read UART at 50 Hz and send blocks through L5.Terminal (FSM handles addressing)
 * ====================================================================================*/
static void term_rx_task(void *arg)
{
    (void)arg;
    HLOGI(TAG, "TERM RX task start (50 Hz)");

    TickType_t last_wake = xTaskGetTickCount();
    static uint8_t seq = 0x08u;
    uint8_t pending[TERM_RX_PENDING_MAX];
    size_t  pending_len = 0;
    uint32_t pending_last_rx_ms = 0;
    uint32_t wait_credit_log_ms = 0;

    while (!tasks_stop_requested_()) {
        if (sp_fsm_get_state() != SP_ST_ONLINE || !g_term_ready_seen) {
            /* A new or renewed tunnel session starts with the compatible series value. */
            seq = 0x08u;
            pending_len = 0;
            pending_last_rx_ms = 0;
            (void)serial_comm_flush_input();
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TERM_RX_TICK_MS));
            continue;
        }

        uint8_t buf[TERM_RX_MAX_PER_TICK];
        if (g_term_owner == PROTOCOL_TERM_IO_WIFI) {
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

            if (g_term_owner == PROTOCOL_TERM_IO_WIFI) {
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
            L7LOGI(TAG, "L7 TERM RX block: series=0x%02X len=%u | \"%s\"%s | %s%s",
                   (unsigned)seq, (unsigned)tx_len, ascii, (tx_len > 80 ? "..." : ""),
                   hex, (tx_len > 64 ? " ..." : ""));
#endif
            PCMTLOGI(TAG, "PCMT PC->CAN send series=0x%02X len=%u pending=%u",
                     (unsigned)seq, (unsigned)tx_len, (unsigned)pending_len);
            sp_err_t se = sp_term_send(pending, (uint16_t)tx_len, seq);
            if (se != SP_OK) {
                HLOGW(TAG, "term_send(len=%u) failed: %d", (unsigned)tx_len, (int)se);
                PCMTLOGW(TAG, "PCMT PC->CAN send FAIL series=0x%02X len=%u err=%d pending=%u",
                         (unsigned)seq, (unsigned)tx_len, (int)se, (unsigned)pending_len);
            } else {
                if (g_term_owner == PROTOCOL_TERM_IO_WIFI) {
                    wifi_recent_tx_record_(pending, tx_len);
                }
                seq = term_input_series_next_(seq);
                sp_fsm_term_input_mark_sent();
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
                PCMTLOGI(TAG, "PCMT PC->CAN send OK next_series=0x%02X pending=%u",
                         (unsigned)seq, (unsigned)pending_len);
            }
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
 * FSM TIMER (polls the FSM; state logic is implemented in fsm.c)
 * ====================================================================================*/
static void fsm_poll_timer(void *arg)
{
    (void)arg;

    sp_fsm_tick((uint32_t)(esp_timer_get_time() / 1000)); /* now_ms */

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
    g_log_cb = log_cb; g_state_cb = state_cb; g_ui_user = user;
}

bool protocol_handler_init(const protocol_handler_cfg_t *cfg)
{
    if (!cfg) { HLOGE(TAG, "Invalid config"); return false; }
    g_cfg = *cfg;

    if (!buffer_manager_init()) {
        HLOGE(TAG, "Buffer manager init failed");
        return false;
    }

    HLOGI(TAG, "SERIAL init: baud=%u", g_cfg.rs_baudrate);
    if (serial_comm_init(g_cfg.rs_baudrate) != ESP_OK) {
        HLOGE(TAG, "Serial init failed"); return false;
    }

    /* === CAN up === */
    if (!bringup_can_(g_cfg.can_bitrate)) {
        serial_comm_deinit();
        return false;
    }

    /* === L5.Terminal (RX callback into the pipe) === */
    {
        sp_term_config_t tcfg = (sp_term_config_t){0};
        if (sp_term_init(&tcfg, term_on_l5_rx_, NULL) != SP_OK) {
            HLOGE(TAG, "L5.Terminal init failed"); goto fail_can;
        }
        (void)sp_service_init(/*on_rx*/NULL, NULL);
    }

    /* === FSM and actions (all protocol logic is encapsulated by the FSM) === */
    {
        if (sp_actions_init() != SP_OK) {
            HLOGE(TAG, "sp_actions_init failed");
            goto fail_l5;
        }

        /* The FSM initializes and owns L2, L3L4, and TLV. */
        sp_fsm_config_t fcfg = {
            .local_sa = 0x00,
            .peer_sa  = 0xFF,
            .timing = {
                .addr_claim_interval_ms = 1000,
                .announce_period_ms     = 3000,
                .poll_timeout_ms        = 3000
            }
        };
        sp_fsm_actions_t fact = {
            .send_bcast_ef_small   = sp_action_send_bcast_ef_small,
            .send_bcast_ea_pointer = sp_action_send_bcast_ea_pointer,
            .send_bcast_ee_status  = sp_action_send_bcast_ee_status,
            .send_dialog_req       = sp_action_send_dialog_req
        };
        if (sp_fsm_init(&fcfg, &fact) != SP_OK) {
            HLOGE(TAG, "FSM init failed"); goto fail_l5;
        }

        /* The FSM binds the layers through the new sp_fsm_bind_layers() prototype:
           - L2 configuration: no hardware filters
           - L3L4 block timeout: from bridge_can_config.h
           - slots: 8
           - TLV: 50 ms deduplication, cycle detector enabled
           - this handler does not need L2/L3L4 events, so callbacks are NULL */
        sp_l2_config_t  l2cfg = { .use_hw_filters = false };
        sp_timing_t     trtim = { .fp_block_timeout_ms = BRIDGE_CAN_PKT_TIMEOUT_MS };
        const uint16_t  tr_slots = BRIDGE_CAN_TR_MAX_SLOTS;
        sp_tlv_config_t tlvc  = { .dedup_ms = 50, .detect_cycle = true };

        if (sp_fsm_bind_layers(&l2cfg, &trtim, tr_slots, &tlvc,
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

    /* FSM polling timer */
    {
        esp_timer_create_args_t targs = {
            .callback = fsm_poll_timer,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "fsm_poll"
        };
        if (esp_timer_create(&targs, &g_fsm_timer) != ESP_OK ||
            esp_timer_start_periodic(g_fsm_timer, FSM_POLL_PERIOD_US) != ESP_OK) {
            HLOGE(TAG, "FSM timer init failed"); goto fail_q;
        }
    }

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

/* ----- error path ----- */
fail_q:
    telnet_router_unregister(TELNET_ROUTE_BRIDGE);
    if (g_q_wifi_rx) { vQueueDelete(g_q_wifi_rx); g_q_wifi_rx = NULL; }
fail_q_term:
    vQueueDelete(g_q_term_evt); g_q_term_evt = NULL;
fail_bind:
    sp_fsm_unbind_layers();
fail_fsm:
    sp_fsm_deinit();
fail_l5:
    sp_term_deinit();
    sp_service_deinit();
fail_can:
    g_handler_ready = false;
    {
        esp_err_t can_err = can_driver_deinit();
        if (can_err != ESP_OK) {
            HLOGE(TAG, "CAN cleanup failed: %s", esp_err_to_name(can_err));
        }
    }
    serial_comm_deinit();
    buffer_manager_deinit();
    return false;
}

bool protocol_handler_start(void)
{
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
            return false;
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
            atomic_store_explicit(&g_tasks_stop, true, memory_order_release);
            (void)handler_wait_tasks_stopped_(HANDLER_TASK_STOP_WAIT_MS);
            return false;
        }
        atomic_store_explicit(&g_task_term_rx, created_task, memory_order_release);
    }

    HLOGI(TAG, "Started");
    return true;
}

bool protocol_handler_stop(void)
{
    g_handler_ready = false;
    /* Drain a copied Wi-Fi callback before deleting its destination queue. */
    telnet_router_unregister(TELNET_ROUTE_BRIDGE);
    wifi_recent_tx_record_(NULL, 0);
    atomic_store_explicit(&g_tasks_stop, true, memory_order_release);
    if (!handler_wait_tasks_stopped_(HANDLER_TASK_STOP_WAIT_MS)) {
        HLOGE(TAG, "Worker task stop timed out; runtime kept alive");
        return false;
    }

    if (g_fsm_timer) { esp_timer_stop(g_fsm_timer); esp_timer_delete(g_fsm_timer); g_fsm_timer = NULL; }

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
    /* The FSM owns the layers and strategy and disconnects them during unbind/deinit. */
    sp_fsm_unbind_layers();
    sp_fsm_deinit();

    sp_term_deinit();
    sp_service_deinit();

    esp_err_t can_err = can_driver_deinit();
    if (can_err != ESP_OK) {
        HLOGE(TAG, "CAN stop failed: %s", esp_err_to_name(can_err));
        return false;
    }

    atomic_store_explicit(&g_tasks_stop, false, memory_order_release);
    HLOGI(TAG, "Stopped");
    return true;
}

bool protocol_handler_deinit(void)
{
    if (!protocol_handler_stop()) return false;

    serial_comm_deinit();
    buffer_manager_deinit();
    HLOGI(TAG, "Deinit");
    return true;
}

esp_err_t protocol_handler_set_rs_baudrate(uint32_t baud)
{
    esp_err_t err;

    if (baud == 0) return ESP_ERR_INVALID_ARG;
    err = serial_comm_set_baudrate(baud);
    if (err == ESP_OK) {
        g_cfg.rs_baudrate = baud;
        HLOGI(TAG, "SERIAL baud changed on-the-fly: %u", (unsigned)baud);
    } else {
        HLOGW(TAG, "SERIAL baud change failed: %s", esp_err_to_name(err));
    }
    return err;
}

void protocol_handler_set_term_io_owner(protocol_term_io_t owner)
{
    g_term_owner = owner;
    if (!g_handler_ready) {
        HLOGI(TAG, "TERM I/O owner staged: %s",
              owner == PROTOCOL_TERM_IO_WIFI ? "WIFI" : "UART");
        return;
    }
    if (owner == PROTOCOL_TERM_IO_WIFI) {
        (void)serial_comm_flush_input();
        wifi_recent_tx_record_(NULL, 0);
        telnet_router_set_active(TELNET_ROUTE_BRIDGE);
    } else if (g_q_wifi_rx) {
        wifi_rx_queue_drain_();
        telnet_router_set_active(TELNET_ROUTE_NONE);
    }
    HLOGI(TAG, "TERM I/O owner: %s", owner == PROTOCOL_TERM_IO_WIFI ? "WIFI" : "UART");
}

protocol_term_io_t protocol_handler_get_term_io_owner(void)
{
    return g_term_owner;
}

bool protocol_handler_is_term_ready(void)
{
    return (g_state == PROTO_STATE_ONLINE) && g_term_ready_seen;
}

/* External byte-stream transmission uses L5.Terminal; the FSM handles addressing. */
bool protocol_handler_send_ascii(const uint8_t *data, size_t len)
{
    if (!data || !len) { HLOGW(TAG, "Invalid data or len"); return false; }

    static uint8_t seq = 0x08u;

    size_t off = 0;
    while (off < len) {
        const size_t chunk = (len - off > TERM_RX_MAX_PER_TICK) ? TERM_RX_MAX_PER_TICK : (len - off);
        sp_err_t se = sp_term_send(data + off, (uint16_t)chunk, seq);
        if (se != SP_OK) {
            HLOGW(TAG, "Send chunk off=%u len=%u failed: %d",
                  (unsigned)off, (unsigned)chunk, (int)se);
            return false;
        }
        seq = term_input_series_next_(seq);

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
