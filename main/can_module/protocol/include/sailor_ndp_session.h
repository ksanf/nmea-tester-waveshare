/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#pragma once
/* Portable, bounded NDP connection layer. No allocation, tasks or I/O.
 * All APIs use wire-order NAME bytes and a wrapping monotonic millisecond clock.
 * Caller serializes calls. Callbacks may only call session_send; TX callback
 * must not re-enter this module. Wire RX is a COMPLETE reassembled NDP message.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAILOR_NDP_SESSION_SLOTS 2u
#define SAILOR_NDP_BINARY_SLOT 0u
#define SAILOR_NDP_TERMINAL_SLOT 1u
#define SAILOR_NDP_SESSION_QUEUE 4u
#define SAILOR_NDP_SESSION_MAX_DATA 512u
#define SAILOR_NDP_SESSION_RETRY_MS 1350u
#define SAILOR_NDP_SESSION_REOPEN_MS 1000u
#define SAILOR_NDP_SESSION_ATTEMPTS 3u
#define SAILOR_NDP_NO_PORT 0xffu

typedef struct {
    uint8_t name[8], sa;
    uint16_t device_type, device_subtype, build;
    uint8_t version_major, version_minor, service_count;
    bool binary_service, terminal_service, serial_valid;
    uint8_t binary_port, terminal_port;
    char serial[17];
} sailor_ndp_peer_t;

typedef enum {
    SAILOR_NDP_SESSION_UNBOUND = 0,
    SAILOR_NDP_SESSION_OPENING,
    SAILOR_NDP_SESSION_UP,
    SAILOR_NDP_SESSION_RETRY_WAIT
} sailor_ndp_session_state_t;

typedef enum {
    SAILOR_NDP_REASON_NONE = 0,
    SAILOR_NDP_REASON_LOCAL_CLOSE,
    SAILOR_NDP_REASON_PEER_ABORT,
    SAILOR_NDP_REASON_TIMEOUT,
    SAILOR_NDP_REASON_BAD_SEQUENCE,
    SAILOR_NDP_REASON_PEER_RESTART,
    SAILOR_NDP_REASON_ADDRESS_CHANGED,
    SAILOR_NDP_REASON_SEND_FAILED
} sailor_ndp_session_reason_t;

/* Lower transport callback. series is the complete wire control
 * byte: 0x10/11/12, 0x08|seq or seq. short_ack is true only for the last case.
 * Returning true means the complete message was accepted for lower transport.
 * The data pointer is borrowed only for the duration of this callback. */
typedef bool (*sailor_ndp_session_tx_fn)(void *user, uint8_t peer,
    uint8_t series, uint8_t channel, const uint8_t *data, uint16_t len,
    bool short_ack);
typedef bool (*sailor_ndp_session_receive_fn)(void *user, uint8_t slot,
    const uint8_t *data, uint16_t len, uint32_t now);
typedef void (*sailor_ndp_session_state_fn)(void *user, uint8_t slot,
    sailor_ndp_session_state_t state, sailor_ndp_session_reason_t reason,
    uint32_t now);
typedef void (*sailor_ndp_session_complete_fn)(void *user, uint8_t slot,
    uint32_t cookie, bool success, uint32_t now);
typedef struct {
    sailor_ndp_session_tx_fn transmit;
    sailor_ndp_session_receive_fn receive;
    sailor_ndp_session_state_fn state;
    sailor_ndp_session_complete_fn complete;
} sailor_ndp_session_callbacks_t;

typedef struct {
    uint8_t data[SAILOR_NDP_SESSION_MAX_DATA];
    uint16_t len;
    uint32_t cookie, sent_at;
    uint8_t attempts;
    bool sent;
} sailor_ndp_session_packet_t;
typedef struct {
    sailor_ndp_session_state_t state;
    uint8_t local_port, remote_port, peer_sa, peer_name[8];
    uint8_t tx_seq, rx_seq, last_rx_seq, head, count;
    bool have_rx, open_sent, open_attempted;
    uint32_t at;
    uint16_t last_rx_len;
    uint8_t last_rx[SAILOR_NDP_SESSION_MAX_DATA];
    sailor_ndp_session_packet_t queue[SAILOR_NDP_SESSION_QUEUE];
} sailor_ndp_session_slot_t;
typedef struct {
    uint8_t local_name[8], local_sa;
    sailor_ndp_session_callbacks_t callbacks;
    void *user;
    sailor_ndp_session_slot_t slots[SAILOR_NDP_SESSION_SLOTS];
} sailor_ndp_session_t;

/* Pure parser, application bytes starting with02, not the 5F99 NDP header.
 * Requires exact50+3*count bytes, count<=16, type2, nonzero/non-FF NAME,
 * ports0..15 and at least one of service1(binary)/2(terminal).
 * Conflicting or duplicate service1/2 entries are rejected. Unknown service
 * types are allowed. A missing/nondecimal/long serial does not invalidate an
 * otherwise usable announcement; serial_valid is false. Output zero on error.
 */
bool sailor_ndp_session_parse_discovery(uint8_t sa, const uint8_t *data,
    size_t len, sailor_ndp_peer_t *out);
void sailor_ndp_session_init(sailor_ndp_session_t *s,
    const uint8_t local_name[8], uint8_t local_sa,
    const sailor_ndp_session_callbacks_t *callbacks, void *user);
/* Slot0 always uses local1 and requires the advertised binary port; slot1 uses
 * local3 and requires the advertised terminal port. Pins peer NAME/SA/port.
 * Same binding is idempotent. A different NAME, SA or port requires unbind or
 * peer_claim as appropriate; announcements cannot silently steal a session.
 * Bind enters OPENING; tick sends OPEN. Does not retain the peer pointer. */
bool sailor_ndp_session_bind(sailor_ndp_session_t *s, uint8_t slot,
    const sailor_ndp_peer_t *peer, uint8_t remote_port, uint32_t now);
void sailor_ndp_session_unbind(sailor_ndp_session_t *s, uint8_t slot,
    uint32_t now);
/* Only call with a VALIDATED address-claim (full NAME+legal source address).
 * A known NAME moving to a new SA resets both its slots and queued messages.
 * Returns false for an unknown NAME, illegal SA, or own NAME/SA collision.
 * A different NAME claiming a bound SA invalidates that binding until a valid
 * claim of the saved NAME resolves it again. No unknown NAME is adopted. */
bool sailor_ndp_session_peer_claim(sailor_ndp_session_t *s,
    const uint8_t name[8], uint8_t sa, uint32_t now);
void sailor_ndp_session_set_local_sa(sailor_ndp_session_t *s, uint8_t sa,
    uint32_t now);
/* Copies0..512 bytes, succeeds only in UP with queue capacity. Cookie0 is valid.
 * Exactly one completion for each accepted send: success after NDP ACK, false
 * when discarded by timeout/abort/unbind/rebinding. Failure may follow a
 * successful lower transmission; the peer may already have consumed bytes. */
bool sailor_ndp_session_send(sailor_ndp_session_t *s, uint8_t slot,
    const uint8_t *data, size_t len, uint32_t cookie, uint32_t now);
/* DA must equal current legal local SA. Nonmatching SA/channel/NAME is ignored.
 * receive=false means backpressure: no ACK or RX-sequence advance, so the peer
 * can retry. Upper-protocol malformed data that should be discarded must be
 * consumed by returning true; this layer does not interpret application CRC. */
void sailor_ndp_session_rx(sailor_ndp_session_t *s, uint8_t sa, uint8_t da,
    uint8_t series, uint8_t channel, const uint8_t *data, size_t len,
    uint32_t now);
void sailor_ndp_session_tick(sailor_ndp_session_t *s, uint32_t now);
bool sailor_ndp_session_is_up(const sailor_ndp_session_t *s, uint8_t slot);
