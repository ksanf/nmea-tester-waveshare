/* Copyright (c) 2026 S.Zhurba. SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AISDEC_MAX_TARGETS 256
#define AISDEC_MAX_LINE 192
#define AISDEC_STALE_MS 600000U
#define AISDEC_FRAG_TIMEOUT_MS 5000U

typedef struct {
    bool used;
    uint32_t last_seen_ms;
    uint32_t mmsi;
    uint8_t msg_type;
    uint8_t nav_status;
    float lat, lon, sog, cog;
    uint16_t heading;
    char name[21];
    char call[8];
} aisdecoder_target_t;

typedef struct {
    bool active;
    uint32_t lines_seen;
    uint32_t decoded_updates;
    uint32_t last_rx_ms;
    char last_line[AISDEC_MAX_LINE];
} aisdecoder_runtime_status_t;

/* Thread-safe task APIs; none calls LVGL. The RX mode owns start/stop;
 * opening/closing a local or web viewer never clears the target table. */
bool aisdecoder_runtime_start(void);
void aisdecoder_runtime_stop(void);
void aisdecoder_runtime_feed(const char *line);
size_t aisdecoder_runtime_snapshot(aisdecoder_target_t *out, size_t max_targets,
                                  uint32_t *decoded_updates);
void aisdecoder_runtime_status(aisdecoder_runtime_status_t *out);

/* Compatibility aliases, also safe from an RX worker without a screen. */
bool aisdecoder_is_ais_sentence(const char *line);
void aisdecoder_feed_nmea_line(const char *line);

#ifdef __cplusplus
}
#endif
