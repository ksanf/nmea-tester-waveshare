/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   L2 implementation over can_driver: 29-bit ID to/from PGN/PDU, send/receive.
 */

#include "n2k_iso11783.h"
#include "esp_log.h"
#define CFG_LOG_MODULE LOG_CFG_N2K_ISO11783
#include "config_logs.h"

static const char *TAG = "n2k_iso11783";

/* Build a 29-bit identifier from priority, PGN, SA, and DA/GE.
 *  - For PDU1 (PF < 240), ID.PS contains DA and the PGN must have PS=0x00.
 *  - For PDU2 (PF >= 240), ID.PS contains GE (the low PGN byte); DA is ignored and set to 0xFF above.
 */
uint32_t n2k_pack_id(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t da_or_ge)
{
    uint32_t can_id = 0;
    uint8_t pf = n2k_pgn_pf(pgn);
    uint8_t dp = n2k_pgn_dp(pgn);
    uint8_t ps;

    if (pf < 240U) {
        /* For PDU1, the low PGN byte must be zero. */
        pgn = n2k_pgn_canonical(pgn);
        ps  = da_or_ge; /* Destination Address */
    } else {
        /* PDU2: PS=Group Extension = low byte PGN */
        ps  = (uint8_t)(pgn & 0xFFU);
    }

    /* Build the ID. EDP is 0 for NMEA2000; DP comes from the PGN. */
    can_id |= ((uint32_t)(priority & N2K_ID_PRIORITY_MASK)) << N2K_ID_PRIORITY_SHIFT;
    can_id |= ((uint32_t)0U & N2K_ID_EDP_MASK)            << N2K_ID_EDP_SHIFT; /* EDP=0 */
    can_id |= ((uint32_t)(dp & N2K_ID_DP_MASK))           << N2K_ID_DP_SHIFT;
    can_id |= ((uint32_t)pf)                              << N2K_ID_PF_SHIFT;
    can_id |= ((uint32_t)ps)                              << N2K_ID_PS_SHIFT;
    can_id |= ((uint32_t)sa)                              << N2K_ID_SA_SHIFT;

    return can_id;
}

/* Parse a 29-bit identifier into n2k_ll_frame_t (ID fields, PGN, and addressing).
 * Leave data/len/timestamp untouched; the caller or n2k_ll_receive() fills them.
 */
void n2k_parse_id(uint32_t id, n2k_ll_frame_t *out)
{
    uint8_t priority = (uint8_t)((id >> N2K_ID_PRIORITY_SHIFT) & N2K_ID_PRIORITY_MASK);
    uint8_t edp      = (uint8_t)((id >> N2K_ID_EDP_SHIFT)      & N2K_ID_EDP_MASK);
    uint8_t dp       = (uint8_t)((id >> N2K_ID_DP_SHIFT)       & N2K_ID_DP_MASK);
    uint8_t pf       = (uint8_t)((id >> N2K_ID_PF_SHIFT)       & N2K_ID_PF_MASK);
    uint8_t ps       = (uint8_t)((id >> N2K_ID_PS_SHIFT)       & N2K_ID_PS_MASK);
    uint8_t sa       = (uint8_t)((id >> N2K_ID_SA_SHIFT)       & N2K_ID_SA_MASK);

    out->priority = priority & 0x7U;
    out->edp      = edp;
    out->dp       = dp;
    out->pf       = pf;
    out->ps       = ps;
    out->sa       = sa;

    if (pf < 240U) {
        /* PDU1: DA=PS, PGN low byte = 0x00 */
        out->is_pdu2 = false;
        out->dst     = ps;
        out->pgn     = ((uint32_t)dp << 16) | ((uint32_t)pf << 8);
    } else {
        /* PDU2: low PGN byte is GE (PS), and dst is GLOBAL (broadcast). */
        out->is_pdu2 = true;
        out->dst     = N2K_ADDR_GLOBAL;
        out->pgn     = ((uint32_t)dp << 16) | ((uint32_t)pf << 8) | ps;
    }
}

/* Send a single-frame N2K/J1939 frame (DLC <= 8) through can_driver.
 * Upper transport layers must segment and reassemble longer messages (DLC > 8).
 */
esp_err_t n2k_ll_send(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t da,
                      const uint8_t *payload, uint8_t len, uint32_t timeout_ms)
{
    if (!n2k_ll_len_valid(len)) {
        ESP_LOGE(TAG, "len=%u > 8: transport layer required (Fast-Packet/TP)", len);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Build the 29-bit ID. */
    uint8_t pf = n2k_pgn_pf(pgn);
    if (pf < 240U) {
        /* For a PDU1 broadcast, set DA=0xFF (global). */
        if (da == N2K_ADDR_GLOBAL) {
            /* Valid: this produces a global PDU1 broadcast. */
        }
    } else {
        /* For PDU2, DA is ignored and ID.PS receives GE from the PGN. */
        da = N2K_ADDR_GLOBAL;
    }

    uint32_t can_id = n2k_pack_id(priority, pgn, sa, (pf < 240U) ? da : (uint8_t)(pgn & 0xFFU));

    /* Build the TWAI frame. */
    twai_message_t msg = {0};
    msg.identifier = can_id;
    msg.data_length_code = len;
    msg.flags = TWAI_MSG_FLAG_EXTD;  /* N2K/J1939 always uses an extended ID. */
    if (len == 0) {
        /* Zero-length payloads are unusual but valid. */
    } else {
        memcpy(msg.data, payload, len);
    }

    esp_err_t err = can_driver_send(&msg, timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAN TX failed (PGN %u, pri %u, sa %u, da %u): %s",
                 (unsigned)pgn, (unsigned)priority, (unsigned)sa, (unsigned)da, esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

/* Receive a twai_message_t from the lower driver, reject non-extended and RTR frames,
 * parse the ID, and copy the payload.
 */
esp_err_t n2k_ll_receive(n2k_ll_frame_t *out, uint32_t timeout_ms)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    twai_message_t msg;
    esp_err_t err = can_driver_receive(&msg, timeout_ms);
    if (err != ESP_OK) {
        return err; /* ESP_ERR_TIMEOUT or another valid error code */
    }

    /* Only extended frames are relevant to N2K/J1939. */
    if ((msg.flags & TWAI_MSG_FLAG_EXTD) == 0) {
#if BRIDGE_CAN_LOG_N2K_TXRX
        ESP_LOGI(TAG, "L2 DROP std id=%08X dlc=%u flags=%08X",
                 msg.identifier, msg.data_length_code, (unsigned)msg.flags);
#endif
        /* Skip it and return a format error so the caller can retry. */
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* N2K does not use RTR frames, so discard them. */
    if (msg.flags & TWAI_MSG_FLAG_RTR) {
#if BRIDGE_CAN_LOG_N2K_TXRX
        ESP_LOGI(TAG, "L2 DROP RTR id=%08X dlc=%u flags=%08X",
                 msg.identifier, msg.data_length_code, (unsigned)msg.flags);
#endif
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Parse the ID into L2 fields. */
    n2k_parse_id(msg.identifier, out);

    /* Payload; clamp DLC to 8. */
    uint8_t dlc = msg.data_length_code;
    if (dlc > 8U) dlc = 8U; /* TWAI can return >8 only for FD, which is not used here. */
    out->len = dlc;
    if (dlc) memcpy(out->data, msg.data, dlc);

    out->timestamp_us = (uint64_t)esp_timer_get_time();

#if BRIDGE_CAN_LOG_N2K_TXRX
    if (out->pgn == 127505u) {
        ESP_LOGI(TAG,
                 "L2 PGN=127505 id=%08X pri=%u dp=%u pf=%02X ps=%02X sa=%02X dlc=%u b0=%02X b1=%02X",
                 msg.identifier,
                 out->priority,
                 out->dp,
                 out->pf,
                 out->ps,
                 out->sa,
                 out->len,
                 out->len > 0 ? out->data[0] : 0u,
                 out->len > 1 ? out->data[1] : 0u);
    }
#endif
    return ESP_OK;
}
