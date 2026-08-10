/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "AISdecoder/aisdecoder_internal.h"
#include "config/memory_config.h"

#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_AISDEC_DECODE
#include "config_logs.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static aisdecoder_target_t *s_targets = NULL;
static uint32_t s_decoded_updates = 0;
static const char *TAG = "aisdec_decode";

static bool targets_alloc_(void)
{
    if (s_targets) return true;
    s_targets = CALLOC_WHERE(AIS_TARGETS_IN_PSRAM, AISDEC_MAX_TARGETS, sizeof(*s_targets));
    return s_targets != NULL;
}

void aisdecoder_decode_deinit(void)
{
    free(s_targets);
    s_targets = NULL;
    s_decoded_updates = 0;
}

static int ais6_to_val_(char c)
{
    int v = (int)c - 48;
    if (v > 40) v -= 8;
    return (v >= 0 && v <= 63) ? v : 0;
}

static bool payload_has_bits_(const char *payload, size_t needed_bits)
{
    size_t chars;

    if (!payload) return false;
    chars = strlen(payload);
    if (chars > (SIZE_MAX / 6u)) return false;
    return chars * 6u >= needed_bits;
}

static uint32_t ais_get_ubit_(const char *payload, int bit_off, int bit_len)
{
    uint32_t v = 0;
    for (int i = 0; i < bit_len; i++) {
        int bit = bit_off + i;
        int ch_idx = bit / 6;
        int bit_in_ch = 5 - (bit % 6);
        int six = ais6_to_val_(payload[ch_idx]);
        v = (v << 1) | ((six >> bit_in_ch) & 1U);
    }
    return v;
}

static int32_t ais_get_sbit_(const char *payload, int bit_off, int bit_len)
{
    uint32_t raw = ais_get_ubit_(payload, bit_off, bit_len);
    if (raw & (1U << (bit_len - 1))) {
        raw |= ~((1U << bit_len) - 1U);
    }
    return (int32_t)raw;
}

static char ais6_to_char_(uint8_t v)
{
    static const char lut[64] = {
        '@','A','B','C','D','E','F','G','H','I','J','K','L','M','N','O',
        'P','Q','R','S','T','U','V','W','X','Y','Z','[','\\',']','^','_',
        ' ','!','\"','#','$','%','&','\'','(',')','*','+',',','-','.','/',
        '0','1','2','3','4','5','6','7','8','9',':',';','<','=','>','?'
    };
    return lut[v & 0x3F];
}

static void ais_get_text_(const char *payload, int bit_off, int char_count, char *dst, size_t dst_sz)
{
    int out = 0;
    char tmp[32];

    if (!dst || dst_sz == 0) return;
    if (char_count > (int)(sizeof(tmp) - 1)) char_count = (int)(sizeof(tmp) - 1);
    for (int i = 0; i < char_count; i++) {
        tmp[i] = ais6_to_char_((uint8_t)ais_get_ubit_(payload, bit_off + (i * 6), 6));
    }
    tmp[char_count] = '\0';

    while (char_count > 0 && (tmp[char_count - 1] == '@' || tmp[char_count - 1] == ' ')) {
        tmp[--char_count] = '\0';
    }
    for (int i = 0; i < char_count && out < (int)(dst_sz - 1); i++) {
        dst[out++] = (tmp[i] == '@') ? ' ' : tmp[i];
    }
    dst[out] = '\0';
}

static aisdecoder_target_t *find_target_(uint32_t mmsi)
{
    aisdecoder_target_t *free_slot = NULL;
    aisdecoder_target_t *oldest = NULL;

    for (size_t i = 0; i < AISDEC_MAX_TARGETS; i++) {
        if (s_targets[i].used && s_targets[i].mmsi == mmsi) {
            return &s_targets[i];
        }
        if (!s_targets[i].used && !free_slot) {
            free_slot = &s_targets[i];
        }
        if (!oldest || s_targets[i].last_seen_ms < oldest->last_seen_ms) {
            oldest = &s_targets[i];
        }
    }
    if (free_slot) return free_slot;
    memset(oldest, 0, sizeof(*oldest));
    return oldest;
}

static void decode_position_common_(aisdecoder_target_t *t, const char *payload, int sog_bit, int lon_bit,
                                    int lat_bit, int cog_bit, int hdg_bit)
{
    int32_t raw_lon = ais_get_sbit_(payload, lon_bit, 28);
    int32_t raw_lat = ais_get_sbit_(payload, lat_bit, 27);
    uint32_t raw_sog = ais_get_ubit_(payload, sog_bit, 10);
    uint32_t raw_cog = ais_get_ubit_(payload, cog_bit, 12);
    uint32_t raw_hdg = ais_get_ubit_(payload, hdg_bit, 9);

    t->sog = (raw_sog >= 1022U) ? 0.0f : ((float)raw_sog / 10.0f);
    t->cog = (raw_cog >= 3600U) ? 0.0f : ((float)raw_cog / 10.0f);
    t->heading = (raw_hdg > 359U) ? 0U : (uint16_t)raw_hdg;
    t->lon = (raw_lon == 0x06791AC0) ? 999.0f : ((float)raw_lon / 600000.0f);
    t->lat = (raw_lat == 0x03412140) ? 999.0f : ((float)raw_lat / 600000.0f);
}

static void decode_type_123_(const char *payload, aisdecoder_target_t *t)
{
    t->nav_status = (uint8_t)ais_get_ubit_(payload, 38, 4);
    decode_position_common_(t, payload, 50, 61, 89, 116, 128);
}

static void decode_type_18_(const char *payload, aisdecoder_target_t *t)
{
    decode_position_common_(t, payload, 46, 57, 85, 112, 124);
}

static void decode_type_9_(const char *payload, aisdecoder_target_t *t)
{
    int32_t raw_lon = ais_get_sbit_(payload, 61, 28);
    int32_t raw_lat = ais_get_sbit_(payload, 89, 27);
    uint32_t raw_sog = ais_get_ubit_(payload, 50, 10);
    uint32_t raw_cog = ais_get_ubit_(payload, 116, 12);

    t->sog = (raw_sog >= 1023U) ? 0.0f : (float)raw_sog;
    t->cog = (raw_cog >= 3600U) ? 0.0f : ((float)raw_cog / 10.0f);
    t->lon = (raw_lon == 0x06791AC0) ? 999.0f : ((float)raw_lon / 600000.0f);
    t->lat = (raw_lat == 0x03412140) ? 999.0f : ((float)raw_lat / 600000.0f);
    if (!t->name[0]) strlcpy(t->name, "SAR AIR", sizeof(t->name));
}

static void decode_type_19_(const char *payload, aisdecoder_target_t *t)
{
    decode_position_common_(t, payload, 46, 57, 85, 112, 124);
    ais_get_text_(payload, 143, 20, t->name, sizeof(t->name));
}

static void decode_type_5_(const char *payload, aisdecoder_target_t *t)
{
    ais_get_text_(payload, 70, 7, t->call, sizeof(t->call));
    ais_get_text_(payload, 112, 20, t->name, sizeof(t->name));
}

static void decode_type_24_(const char *payload, aisdecoder_target_t *t)
{
    uint32_t part = ais_get_ubit_(payload, 38, 2);
    if (part == 0U) {
        ais_get_text_(payload, 40, 20, t->name, sizeof(t->name));
    } else if (part == 1U) {
        ais_get_text_(payload, 90, 7, t->call, sizeof(t->call));
    }
}

static void decode_type_21_(const char *payload, aisdecoder_target_t *t)
{
    int32_t raw_lon = ais_get_sbit_(payload, 164, 28);
    int32_t raw_lat = ais_get_sbit_(payload, 192, 27);

    ais_get_text_(payload, 43, 20, t->name, sizeof(t->name));
    if (!t->name[0]) strlcpy(t->name, "AtoN", sizeof(t->name));
    t->lon = (raw_lon == 0x06791AC0) ? 999.0f : ((float)raw_lon / 600000.0f);
    t->lat = (raw_lat == 0x03412140) ? 999.0f : ((float)raw_lat / 600000.0f);
    t->sog = 0.0f;
    t->cog = 0.0f;
    t->heading = 0U;
}

static void decode_type_27_(const char *payload, aisdecoder_target_t *t)
{
    int32_t raw_lon = ais_get_sbit_(payload, 44, 18);
    int32_t raw_lat = ais_get_sbit_(payload, 62, 17);
    uint32_t raw_sog = ais_get_ubit_(payload, 79, 6);
    uint32_t raw_cog = ais_get_ubit_(payload, 85, 9);

    t->nav_status = (uint8_t)ais_get_ubit_(payload, 40, 4);
    t->lon = (raw_lon == 108600) ? 999.0f : ((float)raw_lon / 600.0f);
    t->lat = (raw_lat == 54600) ? 999.0f : ((float)raw_lat / 600.0f);
    t->sog = (raw_sog >= 63U) ? 0.0f : (float)raw_sog;
    t->cog = (raw_cog >= 360U) ? 0.0f : (float)raw_cog;
}

static void decode_type_28_(const char *payload, aisdecoder_target_t *t)
{
    int32_t raw_lon = ais_get_sbit_(payload, 44, 28);
    int32_t raw_lat = ais_get_sbit_(payload, 72, 27);

    if (!t->name[0]) strlcpy(t->name, "AtoN", sizeof(t->name));
    t->lon = (raw_lon == 0x06791AC0) ? 999.0f : ((float)raw_lon / 600000.0f);
    t->lat = (raw_lat == 0x03412140) ? 999.0f : ((float)raw_lat / 600000.0f);
    t->sog = 0.0f;
    t->cog = 0.0f;
    t->heading = 0U;
}

void aisdecoder_decode_reset(void)
{
    if (!targets_alloc_()) {
        ESP_LOGE(TAG, "target alloc failed");
        return;
    }
    memset(s_targets, 0, sizeof(*s_targets) * AISDEC_MAX_TARGETS);
    s_decoded_updates = 0;
}

void aisdecoder_decode_feed_payload(const char *payload)
{
    uint32_t type;
    uint32_t mmsi;
    aisdecoder_target_t *t;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    if (!payload || !*payload) return;
    if (!targets_alloc_()) {
        ESP_LOGE(TAG, "target alloc failed");
        return;
    }
    if (!payload_has_bits_(payload, 38u)) {
        ESP_LOGW(TAG, "payload too short");
        return;
    }
    type = ais_get_ubit_(payload, 0, 6);
    switch (type) {
    case 1:
    case 2:
    case 3:
    case 9:
    case 18:
    case 28:
        if (!payload_has_bits_(payload, 168u)) return;
        break;
    case 5:
        if (!payload_has_bits_(payload, 424u)) return;
        break;
    case 19:
        if (!payload_has_bits_(payload, 312u)) return;
        break;
    case 21:
        if (!payload_has_bits_(payload, 272u)) return;
        break;
    case 24: {
        if (!payload_has_bits_(payload, 40u)) return;
        const uint32_t part = ais_get_ubit_(payload, 38, 2);
        if ((part == 0u && !payload_has_bits_(payload, 160u)) ||
            (part == 1u && !payload_has_bits_(payload, 168u)) || part > 1u) {
            return;
        }
        break;
    }
    case 27:
        if (!payload_has_bits_(payload, 96u)) return;
        break;
    default:
        ESP_LOGI(TAG, "payload unsupported: type=%" PRIu32, type);
        return;
    }
    mmsi = ais_get_ubit_(payload, 8, 30);
    if (mmsi == 0U) {
        ESP_LOGW(TAG, "payload ignored: type=%" PRIu32 " mmsi=0", type);
        return;
    }

    t = find_target_(mmsi);
    if (!t) return;
    if (!t->used) {
        memset(t, 0, sizeof(*t));
        t->used = true;
        t->lon = 999.0f;
        t->lat = 999.0f;
    }
    t->mmsi = mmsi;
    t->msg_type = (uint8_t)type;
    t->last_seen_ms = now_ms;

    switch (type) {
    case 1:
    case 2:
    case 3:
        decode_type_123_(payload, t);
        s_decoded_updates++;
        break;
    case 5:
        decode_type_5_(payload, t);
        s_decoded_updates++;
        break;
    case 9:
        decode_type_9_(payload, t);
        s_decoded_updates++;
        break;
    case 18:
        decode_type_18_(payload, t);
        s_decoded_updates++;
        break;
    case 19:
        decode_type_19_(payload, t);
        s_decoded_updates++;
        break;
    case 21:
        decode_type_21_(payload, t);
        s_decoded_updates++;
        break;
    case 24:
        decode_type_24_(payload, t);
        s_decoded_updates++;
        break;
    case 27:
        decode_type_27_(payload, t);
        s_decoded_updates++;
        break;
    case 28:
        decode_type_28_(payload, t);
        s_decoded_updates++;
        break;
    default:
        ESP_LOGI(TAG, "payload unsupported: type=%" PRIu32 " mmsi=%" PRIu32, type, mmsi);
        break;
    }

    ESP_LOGI(TAG, "decoded: type=%" PRIu32 " mmsi=%" PRIu32 " lat=%.5f lon=%.5f sog=%.1f cog=%.1f hdg=%u name=%s call=%s",
             type, mmsi, t->lat, t->lon, t->sog, t->cog, t->heading,
             t->name[0] ? t->name : "-", t->call[0] ? t->call : "-");
}

size_t aisdecoder_decode_collect(aisdecoder_target_t *out, size_t max_targets, uint32_t now_ms)
{
    size_t count = 0;

    if (!out || max_targets == 0) return 0;
    if (!s_targets) return 0;
    for (size_t i = 0; i < AISDEC_MAX_TARGETS && count < max_targets; i++) {
        if (!s_targets[i].used) continue;
        if ((now_ms - s_targets[i].last_seen_ms) > AISDEC_STALE_MS) continue;
        out[count++] = s_targets[i];
    }
    return count;
}

uint32_t aisdecoder_decode_updates(void)
{
    return s_decoded_updates;
}
