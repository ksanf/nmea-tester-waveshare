/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#pragma once
/* Binary antenna application client above a caller-owned NDP session.
 * No CAN channels, NDP registration/sequence state, tasks, allocations or I/O.
 * The caller serializes calls and supplies a monotonic millisecond clock. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool online, identity_valid, signal_valid, position_valid, position_fresh;
    char serial[17];
    uint8_t cn0_dbhz, signal_bars;
    double latitude, longitude;
    uint32_t position_utc, signal_age_ms, position_age_ms;
    /* Cached values survive a same-peer reconnect. Fresh additionally requires
     * a new response for this group in the current session, age <= NETWORK_MS,
     * and an online binary service. Unknown enum values remain raw. */
    bool registration_valid, registration_fresh;
    uint8_t registration_state; /* 0 logged in, 1 not logged in; others unknown */
    uint16_t registered_ncs, registered_channel;
    bool ocean_valid, ocean_fresh;
    uint8_t ocean_region; /* current registered region: 0 AOR-W, 1 AOR-E, 2 POR, 3 IOR */
    bool protocol_valid, protocol_fresh;
    uint8_t current_protocol; /* TT6006 GUI enum 0..20; other values unknown */
    bool channel_valid, channel_fresh;
    uint16_t channel_number;
    bool channel_state_valid, channel_state_fresh;
    uint8_t channel_state; /* 4043 raw state, no unverified enum interpretation */
    uint8_t tdm_state;
    uint16_t tdm_origin, tdm_frame;
    uint32_t registration_age_ms, protocol_age_ms, channel_age_ms, channel_state_age_ms;
} protocol_antenna_status_t;

/* true means NDP retained a COPY. Completion must be reported separately;
 * callbacks must not synchronously reenter this client. Cookie 0 is control
 * traffic (upper ACK/NAK/ENQ); nonzero cookies identify individual data sends. */
typedef bool (*sailor_telem_send_fn)(void *user, const uint8_t *data,
                                    uint16_t len, uint32_t cookie);

#define SAILOR_TELEM_UPPER_WINDOW 2u
#define SAILOR_TELEM_CONTROL_QUEUE 8u
#define SAILOR_TELEM_POLLS 7u
#define SAILOR_TELEM_NETWORK_MS 15000u
#define SAILOR_TELEM_ACK_MS 10000u
#define SAILOR_TELEM_RETRIES 2u

typedef enum {
    SAILOR_TELEM_TX_FREE,
    SAILOR_TELEM_TX_QUEUED,
    SAILOR_TELEM_TX_NDP,
    SAILOR_TELEM_TX_ACK,
    SAILOR_TELEM_TX_RETRY
} sailor_telem_tx_state_t;
typedef struct {
    uint8_t data[16], len, retries;
    uint16_t sequence, opcode;
    uint32_t cookie, due, order;
    bool sent;
    sailor_telem_tx_state_t state;
} sailor_telem_tx_t;
typedef struct { uint8_t data[13], len; } sailor_telem_control_t;
typedef struct {
    uint16_t opcode;
    uint32_t due, requested;
    bool pending;
} sailor_telem_poll_t;
typedef struct {
    sailor_telem_send_fn send;
    void *user;
    uint8_t peer_name[8], peer_sa;
    bool discovered, accepted, have_rx, have_signal, have_position;
    bool have_clock, position_report_valid;
    uint16_t binary_seq;
    uint32_t cookie_seq, tx_order, last_rx, signal_at, position_at, clock_at;
    uint32_t clock_utc, rx_keys[3];
    uint8_t rx_count, rx_next, poll_next;
    uint8_t network_seen; /* response groups received in this session */
    uint32_t network_at[4];
    sailor_telem_tx_t upper[SAILOR_TELEM_UPPER_WINDOW];
    sailor_telem_control_t controls[SAILOR_TELEM_CONTROL_QUEUE];
    uint8_t control_head, control_count;
    sailor_telem_poll_t polls[SAILOR_TELEM_POLLS];
    protocol_antenna_status_t status;
} sailor_telem_t;

uint16_t sailor_telem_crc(const uint8_t *data, size_t len);
void sailor_telem_init(sailor_telem_t *s, sailor_telem_send_fn send, void *user);
/* Call only for the peer selected by the shared NDP discovery/session manager.
 * Validates the announcement and extracts identity; never opens a connection.
 * A changed NAME clears cached measurements. Address/session lifecycle remains
 * exclusively in the NDP manager's on_session notifications. */
bool sailor_telem_discover(sailor_telem_t *s, uint8_t sa,
                          const uint8_t *announce, size_t len, uint32_t now);
void sailor_telem_disconnect(sailor_telem_t *s, uint32_t now);
void sailor_telem_on_session(sailor_telem_t *s, bool up, uint32_t now);
/* success must mean lower NDP ACK, not merely UART/CAN submission. */
void sailor_telem_tx_complete(sailor_telem_t *s, uint32_t cookie, bool success,
                              uint32_t now);
void sailor_telem_tick(sailor_telem_t *s, uint32_t now);
/* One complete binary packet delivered by the selected NDP session.
 * false ONLY for upper reply backpressure: no RX state/measurement is changed.
 * Malformed/unsupported envelopes return true (NDP bytes were delivered), then
 * are discarded or NAKed at this layer. Valid multipart packets are ACKed but
 * not reassembled or decoded. Only single-fragment ETX telemetry is published. */
bool sailor_telem_rx(sailor_telem_t *s, const uint8_t *data, size_t len,
                     uint32_t now);
void sailor_telem_snapshot(const sailor_telem_t *s, uint32_t now,
                           protocol_antenna_status_t *out);
