/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   L5 transparent service exchange over the shared L3+L4 transport.
 */

#include "l5_service.h"
#include "sailor_proto_common.h"
#include "l3l4_transport.h"
#include "strategy_map.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#define CFG_LOG_MODULE LOG_CFG_L5_SERVICE
#include "config_logs.h"

#define TAG "L5.SVC"

#define SP_SERVICE_SERIES_FIRST 0x08u
#define SP_SERVICE_SERIES_LAST  0x0Fu
#define SP_SERVICE_SERIES_AUTO  0xFFu

typedef struct {
    sp_service_rx_cb_t cb;
    void              *user;
    bool               inited;

    SemaphoreHandle_t  mtx;
    uint8_t            fp_sid;
    uint8_t            app_series;
} svc_ctx_t;

static svc_ctx_t g_svc = {0};

static inline uint8_t service_series_next_(uint8_t s)
{
    if (s < SP_SERVICE_SERIES_FIRST || s >= SP_SERVICE_SERIES_LAST) return SP_SERVICE_SERIES_FIRST;
    return (uint8_t)(s + 1u);
}

sp_err_t sp_service_init(sp_service_rx_cb_t on_rx, void *user)
{
    if (g_svc.inited) return SP_E_STATE;

    memset(&g_svc, 0, sizeof(g_svc));
    g_svc.cb   = on_rx;
    g_svc.user = user;
    g_svc.mtx  = xSemaphoreCreateMutex();
    if (!g_svc.mtx) {
        memset(&g_svc, 0, sizeof(g_svc));
        return SP_E_NOMEM;
    }
    g_svc.fp_sid = 0xFF;
    g_svc.app_series = SP_SERVICE_SERIES_FIRST;
    g_svc.inited = true;

    ESP_LOGI(TAG, "init: L5 service ready");
    return SP_OK;
}

void sp_service_deinit(void)
{
    if (!g_svc.inited) return;

    if (g_svc.mtx) vSemaphoreDelete(g_svc.mtx);
    memset(&g_svc, 0, sizeof(g_svc));
    ESP_LOGI(TAG, "deinit: L5 service stopped");
}

sp_err_t sp_service_send(const uint8_t *data, uint16_t len, uint8_t seq,
                         const sp_id_fields_t *id_opt)
{
    if (!g_svc.inited)     return SP_E_STATE;
    if (!data || len == 0) return SP_E_INVAL;

    sp_app_block_t blk;
    memset(&blk, 0, sizeof(blk));

    sp_strat_fill_app_header(SP_FLOW_PPP_DATA, &blk);

    blk.series   = 0;
    blk.app_data = data;
    blk.app_len  = len;

    sp_id_fields_t id_tmp;
    const sp_id_fields_t *id = id_opt;
    if (!id) {
        sp_strat_build_id(SP_FLOW_PPP_DATA, &id_tmp);
        id = &id_tmp;
    }

    if (!g_svc.mtx) return SP_E_STATE;

    if (xSemaphoreTake(g_svc.mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        return SP_E_STATE;
    }
    if (seq == SP_SERVICE_SERIES_AUTO) {
        blk.series = g_svc.app_series;
        g_svc.app_series = service_series_next_(g_svc.app_series);
    } else {
        blk.series = seq;
    }

    uint8_t sid = g_svc.fp_sid;
    sp_err_t out = sp_tr_send_app(id, &blk, &sid);
    if (out == SP_OK) g_svc.fp_sid = sid;
    xSemaphoreGive(g_svc.mtx);

    return out;
}

void sp_service_on_l4_app(uint8_t /*seq*/, const uint8_t *app_data, uint16_t app_len)
{
    if (!g_svc.inited) return;
    if (!app_data || app_len == 0) return;

    if (g_svc.cb) g_svc.cb(app_data, app_len, g_svc.user);
}
