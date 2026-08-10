/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "AISdecoder/aisdecoder_internal.h"

#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_AISDEC_PARSE
#include "config_logs.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

typedef struct {
    bool active;
    uint8_t total;
    uint8_t next;
    char seq[8];
    char channel;
    uint8_t fill_bits;
    uint32_t started_ms;
    char payload[512];
    size_t payload_len;
} ais_frag_state_t;

static ais_frag_state_t s_frag;
static const char *TAG = "aisdec_parse";

static int hex_value_(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool checksum_valid_(const char *line)
{
    const char *p;
    const char *star;
    uint8_t sum = 0;
    int hi;
    int lo;

    if (!line) return false;
    p = (*line == '!') ? line + 1 : line;
    star = strchr(p, '*');
    if (!star || star == p) return false;
    if (star[1] == '\0' || star[2] == '\0') return false;
    hi = hex_value_(star[1]);
    lo = hex_value_(star[2]);
    if (hi < 0 || lo < 0) return false;
    for (const char *q = p; q < star; ++q) sum ^= (uint8_t)*q;
    return sum == (uint8_t)((hi << 4) | lo);
}

static bool parse_int_(const char *s, int min_value, int max_value, int *out)
{
    char *end = NULL;
    long value;

    if (!s || !*s || !out) return false;
    errno = 0;
    value = strtol(s, &end, 10);
    if (errno != 0 || !end || *end != '\0' ||
        value < min_value || value > max_value) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool payload_valid_(const char *payload)
{
    if (!payload || !*payload) return false;
    for (const unsigned char *p = (const unsigned char *)payload; *p; ++p) {
        if (*p < 48u || *p > 119u || (*p >= 88u && *p <= 95u)) return false;
    }
    return true;
}

static int split_csv_preserve_empty_(char *buf, char **fields, int max_fields)
{
    int n = 0;
    char *p = buf;

    if (!buf || !fields || max_fields <= 0) return 0;

    fields[n++] = p;
    while (*p && n < max_fields) {
        if (*p == ',') {
            *p = '\0';
            fields[n++] = p + 1;
        }
        p++;
    }
    return n;
}

static bool split_vdm_(const char *line, int *frag_cnt, int *frag_num, char *seq, size_t seq_sz,
                       char *channel, char *payload, size_t payload_sz, int *fill_bits)
{
    char buf[AISDEC_MAX_LINE];
    char *fields[8] = {0};
    int n = 0;
    char *star;
    const char *src = line;

    if (!line || !checksum_valid_(line)) return false;
    if (*src == '!') src++;
    if (strlcpy(buf, src, sizeof(buf)) >= sizeof(buf)) return false;
    n = split_csv_preserve_empty_(buf, fields, 8);
    if (n < 7) return false;
    if (strcmp(fields[0], "AIVDM") != 0 && strcmp(fields[0], "AIVDO") != 0) return false;

    if (!parse_int_(fields[1], 1, 9, frag_cnt) ||
        !parse_int_(fields[2], 1, *frag_cnt, frag_num)) {
        return false;
    }
    strlcpy(seq, fields[3] ? fields[3] : "", seq_sz);
    *channel = (fields[4] && fields[4][0]) ? fields[4][0] : '-';
    if (strlcpy(payload, fields[5] ? fields[5] : "", payload_sz) >= payload_sz) {
        return false;
    }
    star = strchr(fields[6], '*');
    if (!star) return false;
    *star = '\0';
    if (!parse_int_(fields[6], 0, 5, fill_bits) || !payload_valid_(payload)) {
        return false;
    }
    if (*frag_num < *frag_cnt && *fill_bits != 0) return false;
    return true;
}

bool aisdecoder_parse_is_sentence(const char *line)
{
    if (!line) return false;
    if (strncmp(line, "!AIVDM", 6) == 0 || strncmp(line, "!AIVDO", 6) == 0) return true;
    if (strncmp(line, "AIVDM", 5) == 0 || strncmp(line, "AIVDO", 5) == 0) return true;
    return false;
}

void aisdecoder_parse_feed_line(const char *line)
{
    int frag_cnt = 0;
    int frag_num = 0;
    int fill_bits = 0;
    char seq[8];
    char channel;
    char payload[128];
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    if (!split_vdm_(line, &frag_cnt, &frag_num, seq, sizeof(seq), &channel,
                    payload, sizeof(payload), &fill_bits)) {
        ESP_LOGW(TAG, "split failed: %s", line ? line : "(null)");
        return;
    }

    ESP_LOGI(TAG, "line ok: frags=%d/%d seq=%s ch=%c fill=%d payload_len=%u",
             frag_num, frag_cnt, seq[0] ? seq : "-", channel, fill_bits, (unsigned)strlen(payload));

    if (frag_cnt <= 1) {
        aisdecoder_decode_feed_payload(payload);
        return;
    }

    if (!s_frag.active || (now_ms - s_frag.started_ms) > AISDEC_FRAG_TIMEOUT_MS ||
        frag_num == 1 || s_frag.total != (uint8_t)frag_cnt ||
        s_frag.channel != channel || strcmp(s_frag.seq, seq) != 0) {
        memset(&s_frag, 0, sizeof(s_frag));
        s_frag.active = true;
        s_frag.total = (uint8_t)frag_cnt;
        s_frag.next = 1;
        s_frag.channel = channel;
        s_frag.started_ms = now_ms;
        strlcpy(s_frag.seq, seq, sizeof(s_frag.seq));
    }

    if (frag_num != s_frag.next) {
        ESP_LOGW(TAG, "frag order mismatch: got=%d expect=%u total=%u", frag_num, s_frag.next, s_frag.total);
        return;
    }
    if ((s_frag.payload_len + strlen(payload)) >= sizeof(s_frag.payload)) {
        ESP_LOGW(TAG, "frag payload overflow");
        memset(&s_frag, 0, sizeof(s_frag));
        return;
    }

    memcpy(&s_frag.payload[s_frag.payload_len], payload, strlen(payload));
    s_frag.payload_len += strlen(payload);
    s_frag.payload[s_frag.payload_len] = '\0';
    s_frag.fill_bits = (uint8_t)fill_bits;
    s_frag.next++;

    if ((uint8_t)frag_num == s_frag.total) {
        aisdecoder_decode_feed_payload(s_frag.payload);
        memset(&s_frag, 0, sizeof(s_frag));
    }
}
