/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   J1939/NMEA2000 L2 link: 29-bit ID packing/parsing, routing, and TX/RX up to 8 bytes.
 */

#include "l2_link.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bridge_can_config.h"   /* HLOG */ /* BRIDGE_CAN_LOG_N2K_TXRX controls low-level logs. */
#include "esp_log.h"
#include <stdatomic.h>

/* ===== Default constants ===== */
#ifndef SP_L2_RX_TASK_NAME
#define SP_L2_RX_TASK_NAME     "sp_l2_rx"
#endif
#ifndef SP_L2_RX_TASK_STACK
#define SP_L2_RX_TASK_STACK    (8192)   /* PPP RX/TX decode+logs run in this task; 4 KB no longer fits safely */
#endif
#ifndef SP_L2_RX_TASK_PRIO
#define SP_L2_RX_TASK_PRIO     (configMAX_PRIORITIES - 3)
#endif
#ifndef SP_L2_RX_TIMEOUT_MS
#define SP_L2_RX_TIMEOUT_MS    (20)   /* Driver polling period */
#endif
#ifndef SP_L2_TX_TIMEOUT_MS
#define SP_L2_TX_TIMEOUT_MS    (10)   /* Driver transmit timeout */
#endif

#define SP_L2_STOP_WAIT_MS     1000u
#define SP_L2_STOP_POLL_MS       10u

/* ===== Internal state ===== */
typedef struct {
    sp_l2_rx_cb_t  on_rx;
    sp_l2_evt_cb_t on_evt;
    void          *user;

    sp_l2_config_t cfg;

    SemaphoreHandle_t tx_mutex;
    bool           inited;
} sp_l2_ctx_t;

static sp_l2_ctx_t g_l2;
static _Atomic(TaskHandle_t) s_l2_rx_task = NULL;
static atomic_bool s_l2_stop = ATOMIC_VAR_INIT(false);

/* ===== Helpers ===== */

static inline uint32_t sp_pack_id(const sp_id_fields_t *f) {
    return ((uint32_t)(f->pri & 0x7) << 26) |
           ((uint32_t)(f->dp  & 0x1) << 24) |
           ((uint32_t) f->pf         << 16) |
           ((uint32_t) f->ps         <<  8) |
           ((uint32_t) f->sa);
}

static inline void sp_unpack_id(uint32_t id, sp_id_fields_t *f) {
    f->pri = (id >> 26) & 0x7;
    f->dp  = (id >> 24) & 0x1;
    f->pf  = (id >> 16) & 0xFF;
    f->ps  = (id >>  8) & 0xFF;
    f->sa  =  id        & 0xFF;
}

/* Configure filters to accept extended frames; reject the rest above. */
static void sp_l2_configure_filters_(bool use_hw_filters) {
    /* can_driver_init() already installs ACCEPT_ALL. Do not tear down and
     * reinstall a live TWAI driver when the caller explicitly wants no HW filter. */
    if (!use_hw_filters) return;
#ifdef TWAI_FILTER_CONFIG_ACCEPT_ALL
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    (void)can_driver_set_filters(&f);
#endif
}

/* ===== RX Task ===== */
static void sp_l2_rx_task_(void *arg) {
    (void)arg;

    twai_message_t msg;
    sp_l2_frame_t  out;

#if BRIDGE_CAN_LOG_HANDLER
    TickType_t t_last = xTaskGetTickCount();
#endif

    while (!atomic_load_explicit(&s_l2_stop, memory_order_acquire)) {
        esp_err_t er = can_driver_receive(&msg, SP_L2_RX_TIMEOUT_MS);
        if (atomic_load_explicit(&s_l2_stop, memory_order_acquire)) break;
        if (er == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (er != ESP_OK) {
            if (g_l2.on_evt) g_l2.on_evt(SP_EVT_L2_RX_DROPPED, NULL);
            continue;
        }

        if ((msg.flags & TWAI_MSG_FLAG_EXTD) == 0) {
            continue; /* Accept extended frames only. */
        }

        sp_unpack_id(msg.identifier, &out.id);

        uint8_t dlc = (msg.data_length_code <= 8) ? msg.data_length_code : 8;
        out.dlc = dlc;
        if (dlc) memcpy(out.data, msg.data, dlc);

#if BRIDGE_CAN_LOG_N2K_TXRX
        ESP_LOGI("N2K",
                 "RX id=%08X pri=%u dp=%u pf=%02X ps=%02X sa=%02X dlc=%u  "
                 "data=%02X %02X %02X %02X %02X %02X %02X %02X",
                 (unsigned)msg.identifier,
                 out.id.pri, out.id.dp, out.id.pf, out.id.ps, out.id.sa, out.dlc,
                 out.dlc>0?out.data[0]:0, out.dlc>1?out.data[1]:0, out.dlc>2?out.data[2]:0, out.dlc>3?out.data[3]:0,
                 out.dlc>4?out.data[4]:0, out.dlc>5?out.data[5]:0, out.dlc>6?out.data[6]:0, out.dlc>7?out.data[7]:0);
#endif

        if (g_l2.on_rx) {
            g_l2.on_rx(&out, g_l2.user);
        }

#if BRIDGE_CAN_LOG_HANDLER
        if ((xTaskGetTickCount() - t_last) > pdMS_TO_TICKS(1000)) {
            UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
            HLOGI("L2", "RX hwmark: %u words (~%u bytes)", (unsigned)hw, (unsigned)(hw * sizeof(StackType_t)));
            t_last = xTaskGetTickCount();
        }
#endif
    }

    atomic_store_explicit(&s_l2_rx_task, NULL, memory_order_release);
    vTaskDelete(NULL);
}

/* ===== Public API ===== */

sp_err_t sp_l2_init(const sp_l2_config_t *cfg,
                    sp_l2_rx_cb_t on_rx,
                    sp_l2_evt_cb_t on_evt,
                    void *user)
{
    if (g_l2.inited) return SP_E_STATE;
    if (!cfg || !on_rx) return SP_E_INVAL;

    memset(&g_l2, 0, sizeof(g_l2));
    g_l2.cfg    = *cfg;
    g_l2.on_rx  = on_rx;
    g_l2.on_evt = on_evt;
    g_l2.user   = user;

    sp_l2_configure_filters_(cfg->use_hw_filters);

    g_l2.tx_mutex = xSemaphoreCreateMutex();
    if (!g_l2.tx_mutex) return SP_E_NOMEM;

    TaskHandle_t created_task = NULL;
    atomic_store_explicit(&s_l2_stop, false, memory_order_release);
    BaseType_t ok = xTaskCreate(
        sp_l2_rx_task_, SP_L2_RX_TASK_NAME,
        SP_L2_RX_TASK_STACK, NULL,
        SP_L2_RX_TASK_PRIO, &created_task
    );
    if (ok != pdPASS) {
        vSemaphoreDelete(g_l2.tx_mutex);
        g_l2.tx_mutex = NULL;
        return SP_E_NOMEM;
    }
    atomic_store_explicit(&s_l2_rx_task, created_task, memory_order_release);

    g_l2.inited = true;
    return SP_OK;
}

void sp_l2_deinit(void) {
    if (!g_l2.inited) return;

    if (atomic_load_explicit(&s_l2_rx_task, memory_order_acquire)) {
        atomic_store_explicit(&s_l2_stop, true, memory_order_release);
        for (uint32_t waited_ms = 0;
             waited_ms < SP_L2_STOP_WAIT_MS &&
                 atomic_load_explicit(&s_l2_rx_task, memory_order_acquire);
             waited_ms += SP_L2_STOP_POLL_MS) {
            vTaskDelay(pdMS_TO_TICKS(SP_L2_STOP_POLL_MS));
        }
        if (atomic_load_explicit(&s_l2_rx_task, memory_order_acquire)) {
            ESP_LOGE("L2", "RX task stop timed out; resources kept alive");
            return;
        }
    }
    if (g_l2.tx_mutex) {
        vSemaphoreDelete(g_l2.tx_mutex);
        g_l2.tx_mutex = NULL;
    }
    memset(&g_l2, 0, sizeof(g_l2));
    atomic_store_explicit(&s_l2_stop, false, memory_order_release);
}

sp_err_t sp_l2_send(const sp_id_fields_t *id, const uint8_t *data, uint8_t dlc) {
    if (!g_l2.inited) return SP_E_STATE;
    if (!id || (dlc > 8)) return SP_E_INVAL;

    twai_message_t msg = {0};
    msg.identifier         = sp_pack_id(id);
    msg.extd               = 1;
    msg.flags              = TWAI_MSG_FLAG_EXTD;
    msg.data_length_code   = dlc;
    if (dlc && data) memcpy(msg.data, data, dlc);

#if BRIDGE_CAN_LOG_N2K_TXRX
    ESP_LOGI("N2K",
             "TX id=%08X pri=%u dp=%u pf=%02X ps=%02X sa=%02X dlc=%u  "
             "data=%02X %02X %02X %02X %02X %02X %02X %02X",
             (unsigned)msg.identifier,
             id->pri, id->dp, id->pf, id->ps, id->sa, dlc,
             dlc>0?(data?data[0]:0):0, dlc>1?(data?data[1]:0):0,
             dlc>2?(data?data[2]:0):0, dlc>3?(data?data[3]:0):0,
             dlc>4?(data?data[4]:0):0, dlc>5?(data?data[5]:0):0,
             dlc>6?(data?data[6]:0):0, dlc>7?(data?data[7]:0):0);
#endif

    sp_err_t ret = SP_OK;

    if (g_l2.tx_mutex) xSemaphoreTake(g_l2.tx_mutex, portMAX_DELAY);
    esp_err_t er = can_driver_send(&msg, SP_L2_TX_TIMEOUT_MS);
    if (g_l2.tx_mutex) xSemaphoreGive(g_l2.tx_mutex);

    if (er != ESP_OK) {
    if (g_l2.on_evt) g_l2.on_evt(SP_EVT_L2_TX_DROPPED, (void*)id);  // Context points to id.
        ret = SP_E_PROTO;
#if BRIDGE_CAN_LOG_N2K_TXRX
        ESP_LOGW("N2K", "TX failed: %s", esp_err_to_name(er));
#endif
    }
    return ret;
}
