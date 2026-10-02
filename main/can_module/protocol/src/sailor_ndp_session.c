/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#include "sailor_ndp_session.h"
#include <string.h>

static bool legal_sa(uint8_t sa) { return sa < 0xfeu; }
static bool elapsed(uint32_t now, uint32_t then, uint32_t ms) {
    return (uint32_t)(now - then) >= ms;
}
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static bool legal_name(const uint8_t *name) {
    if (!name) return false;
    uint8_t any = 0, all = 0xff;
    for (unsigned i = 0; i < 8; ++i) { any |= name[i]; all &= name[i]; }
    return any != 0 && all != 0xff;
}

bool sailor_ndp_session_parse_discovery(uint8_t sa, const uint8_t *p,
                                       size_t n, sailor_ndp_peer_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!p || !legal_sa(sa) || n < 50 || p[0] != 2 || p[1] > 16 ||
        n != 50u + 3u * p[1] || le16(p + 10) != 2 || !legal_name(p + 2))
        return false;
    sailor_ndp_peer_t peer = {0};
    memcpy(peer.name, p + 2, 8);
    peer.sa = sa;
    peer.device_type = le16(p + 10);
    peer.device_subtype = le16(p + 12);
    peer.version_major = p[14]; peer.version_minor = p[15];
    peer.build = le16(p + 16); peer.service_count = p[1];
    peer.binary_port = peer.terminal_port = SAILOR_NDP_NO_PORT;
    for (unsigned i = 0; i < peer.service_count; ++i) {
        const uint8_t *entry = p + 50u + 3u * i;
        uint16_t type = le16(entry);
        uint8_t port = entry[2];
        if (port > 15) return false;
        if (type == 1) {
            if (peer.binary_service) return false;
            peer.binary_service = true; peer.binary_port = port;
        } else if (type == 2) {
            if (peer.terminal_service) return false;
            peer.terminal_service = true; peer.terminal_port = port;
        }
    }
    if (!peer.binary_service && !peer.terminal_service) return false;
    size_t digits = 0;
    while (digits < 32 && p[18 + digits] >= '0' && p[18 + digits] <= '9') ++digits;
    if (digits > 0 && digits <= 16 && p[18 + digits] == 0) {
        memcpy(peer.serial, p + 18, digits);
        peer.serial[digits] = '\0'; peer.serial_valid = true;
    }
    *out = peer;
    return true;
}

static bool bound(const sailor_ndp_session_slot_t *q) {
    return q->state != SAILOR_NDP_SESSION_UNBOUND;
}
static uint8_t tx_channel(const sailor_ndp_session_slot_t *q) {
    return (uint8_t)((q->remote_port << 4) | q->local_port);
}
static bool transmit(sailor_ndp_session_t *s, uint8_t sa, uint8_t series,
                     uint8_t channel, const uint8_t *p, uint16_t n, bool ack) {
    return legal_sa(s->local_sa) && legal_sa(sa) && sa != s->local_sa &&
           s->callbacks.transmit &&
           s->callbacks.transmit(s->user, sa, series, channel, p, n, ack);
}
static bool control_to(sailor_ndp_session_t *s, uint8_t sa, uint8_t series,
                       uint8_t channel, const uint8_t peer_name[8]) {
    uint8_t names[16];
    memcpy(names, s->local_name, 8); memcpy(names + 8, peer_name, 8);
    return transmit(s, sa, series, channel, names, sizeof(names), false);
}
static bool control(sailor_ndp_session_t *s, uint8_t slot, uint8_t series) {
    sailor_ndp_session_slot_t *q = &s->slots[slot];
    return control_to(s, q->peer_sa, series, tx_channel(q), q->peer_name);
}
static void notify(sailor_ndp_session_t *s, uint8_t slot,
                   sailor_ndp_session_reason_t reason, uint32_t now) {
    if (s->callbacks.state)
        s->callbacks.state(s->user, slot, s->slots[slot].state, reason, now);
}
/* State changes before completions, so a callback cannot refill a dead queue.
 * Complete each previously accepted cookie once, including unsent packets. */
static void reset(sailor_ndp_session_t *s, uint8_t slot,
                  sailor_ndp_session_state_t state,
                  sailor_ndp_session_reason_t reason, uint32_t now) {
    sailor_ndp_session_slot_t *q = &s->slots[slot];
    q->state = state;
    q->tx_seq = q->rx_seq = 0; q->last_rx_seq = 0xff;
    q->have_rx = false; q->open_sent = false; q->open_attempted = false;
    q->last_rx_len = 0; q->at = now;
    while (q->count) {
        uint32_t cookie = q->queue[q->head].cookie;
        q->head = (uint8_t)((q->head + 1u) % SAILOR_NDP_SESSION_QUEUE);
        --q->count;
        if (s->callbacks.complete)
            s->callbacks.complete(s->user, slot, cookie, false, now);
    }
    q->head = 0;
    notify(s, slot, reason, now);
}
static void abort_and_retry(sailor_ndp_session_t *s, uint8_t slot,
                            sailor_ndp_session_reason_t reason, uint32_t now) {
    sailor_ndp_session_slot_t *q = &s->slots[slot];
    if (q->state == SAILOR_NDP_SESSION_UP || q->open_sent)
        (void)control(s, slot, 0x12);
    reset(s, slot, SAILOR_NDP_SESSION_RETRY_WAIT, reason, now);
}

void sailor_ndp_session_init(sailor_ndp_session_t *s, const uint8_t name[8],
                            uint8_t sa,
                            const sailor_ndp_session_callbacks_t *callbacks,
                            void *user) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    if (name) memcpy(s->local_name, name, 8);
    s->local_sa = sa; s->user = user;
    if (callbacks) s->callbacks = *callbacks;
    for (unsigned i = 0; i < SAILOR_NDP_SESSION_SLOTS; ++i) {
        s->slots[i].local_port = i == SAILOR_NDP_BINARY_SLOT ? 1 : 3;
        s->slots[i].remote_port = SAILOR_NDP_NO_PORT;
        s->slots[i].peer_sa = 0xfe;
    }
}
bool sailor_ndp_session_bind(sailor_ndp_session_t *s, uint8_t slot,
                            const sailor_ndp_peer_t *peer,
                            uint8_t remote_port, uint32_t now) {
    if (!s || !peer || slot >= SAILOR_NDP_SESSION_SLOTS || remote_port > 15 ||
        !legal_sa(peer->sa) || peer->sa == s->local_sa ||
        !legal_name(peer->name) || !legal_name(s->local_name) ||
        !memcmp(s->local_name, peer->name, 8) || peer->device_type != 2)
        return false;
    if (slot == SAILOR_NDP_BINARY_SLOT) {
        if (!peer->binary_service || peer->binary_port != remote_port) return false;
    } else if (!peer->terminal_service || peer->terminal_port != remote_port)
        return false;
    sailor_ndp_session_slot_t *q = &s->slots[slot];
    if (bound(q))
        return q->peer_sa == peer->sa && q->remote_port == remote_port &&
               !memcmp(q->peer_name, peer->name, 8);
    /* A CAN source address identifies one NAME at a time, even across slots. */
    for (unsigned i = 0; i < SAILOR_NDP_SESSION_SLOTS; ++i) {
        const sailor_ndp_session_slot_t *other = &s->slots[i];
        if (i == slot || !bound(other) || !legal_sa(other->peer_sa)) continue;
        bool same_name = !memcmp(other->peer_name, peer->name, 8);
        if ((other->peer_sa == peer->sa) != same_name) return false;
    }
    memcpy(q->peer_name, peer->name, 8);
    q->peer_sa = peer->sa; q->remote_port = remote_port;
    reset(s, slot, SAILOR_NDP_SESSION_OPENING, SAILOR_NDP_REASON_NONE, now);
    return true;
}
void sailor_ndp_session_unbind(sailor_ndp_session_t *s, uint8_t slot,
                               uint32_t now) {
    if (!s || slot >= SAILOR_NDP_SESSION_SLOTS) return;
    sailor_ndp_session_slot_t *q = &s->slots[slot];
    if (!bound(q)) return;
    if (q->state == SAILOR_NDP_SESSION_UP || q->open_sent)
        (void)control(s, slot, 0x12);
    reset(s, slot, SAILOR_NDP_SESSION_UNBOUND, SAILOR_NDP_REASON_LOCAL_CLOSE, now);
    memset(q->peer_name, 0, 8);
    q->peer_sa = 0xfe; q->remote_port = SAILOR_NDP_NO_PORT;
}
bool sailor_ndp_session_peer_claim(sailor_ndp_session_t *s,
                                   const uint8_t name[8], uint8_t sa,
                                   uint32_t now) {
    if (!s || !legal_name(name) || !legal_sa(sa) || sa == s->local_sa ||
        !memcmp(name, s->local_name, 8)) return false;
    bool known = false;
    for (unsigned i = 0; i < SAILOR_NDP_SESSION_SLOTS; ++i) {
        sailor_ndp_session_slot_t *q = &s->slots[i];
        if (!bound(q)) continue;
        if (!memcmp(name, q->peer_name, 8)) {
            known = true;
            if (q->peer_sa != sa) {
                q->peer_sa = sa;
                reset(s, (uint8_t)i, SAILOR_NDP_SESSION_OPENING,
                      SAILOR_NDP_REASON_ADDRESS_CHANGED, now);
            }
        } else if (q->peer_sa == sa) {
            /* The old SA no longer belongs to the saved NAME. Do not send any
             * ABORT/DATA there, and do not adopt this unrelated claimant. */
            q->peer_sa = 0xfe;
            reset(s, (uint8_t)i, SAILOR_NDP_SESSION_OPENING,
                  SAILOR_NDP_REASON_ADDRESS_CHANGED, now);
        }
    }
    return known;
}
void sailor_ndp_session_set_local_sa(sailor_ndp_session_t *s, uint8_t sa,
                                    uint32_t now) {
    if (!s || s->local_sa == sa) return;
    s->local_sa = sa;
    /* The previous address may already belong to another node after a lost
     * address claim. Never send an abort under that previous identity. */
    for (unsigned i = 0; i < SAILOR_NDP_SESSION_SLOTS; ++i) {
        if (bound(&s->slots[i]))
            reset(s, (uint8_t)i, SAILOR_NDP_SESSION_OPENING,
                  SAILOR_NDP_REASON_ADDRESS_CHANGED, now);
    }
}
bool sailor_ndp_session_is_up(const sailor_ndp_session_t *s, uint8_t slot) {
    return s && slot < SAILOR_NDP_SESSION_SLOTS &&
           s->slots[slot].state == SAILOR_NDP_SESSION_UP;
}
bool sailor_ndp_session_send(sailor_ndp_session_t *s, uint8_t slot,
                            const uint8_t *p, size_t n, uint32_t cookie,
                            uint32_t now) {
    if (!sailor_ndp_session_is_up(s, slot) || n > SAILOR_NDP_SESSION_MAX_DATA ||
        (n && !p)) return false;
    sailor_ndp_session_slot_t *q = &s->slots[slot];
    if (q->count == SAILOR_NDP_SESSION_QUEUE) return false;
    unsigned tail = (q->head + q->count) % SAILOR_NDP_SESSION_QUEUE;
    sailor_ndp_session_packet_t *packet = &q->queue[tail];
    if (n) memcpy(packet->data, p, n);
    packet->len = (uint16_t)n; packet->cookie = cookie;
    packet->sent_at = now; packet->attempts = 0; packet->sent = false;
    ++q->count;
    return true;
}

void sailor_ndp_session_rx(sailor_ndp_session_t *s, uint8_t sa, uint8_t da,
                          uint8_t series, uint8_t channel,
                          const uint8_t *p, size_t n, uint32_t now) {
    if (!s || !legal_sa(sa) || !legal_sa(s->local_sa) || da != s->local_sa ||
        sa == s->local_sa || (n && !p) || n > SAILOR_NDP_SESSION_MAX_DATA)
        return;
    int matched = -1;
    for (unsigned i = 0; i < SAILOR_NDP_SESSION_SLOTS; ++i) {
        sailor_ndp_session_slot_t *q = &s->slots[i];
        if (bound(q) && q->peer_sa == sa && q->local_port == (channel >> 4) &&
            q->remote_port == (channel & 15)) { matched = (int)i; break; }
    }
    if (series >= 0x10 && series <= 0x12) {
        if (n != 16 || !legal_name(p) || memcmp(p + 8, s->local_name, 8)) return;
        if (matched < 0 || memcmp(p, s->slots[matched].peer_name, 8)) {
            if (series == 0x10)
                (void)control_to(s, sa, 0x12,
                    (uint8_t)((channel << 4) | (channel >> 4)), p);
            return;
        }
        uint8_t slot = (uint8_t)matched;
        sailor_ndp_session_slot_t *q = &s->slots[slot];
        if (series == 0x12) {
            reset(s, slot, SAILOR_NDP_SESSION_RETRY_WAIT,
                  SAILOR_NDP_REASON_PEER_ABORT, now);
        } else if (series == 0x11) {
            if (q->state == SAILOR_NDP_SESSION_OPENING && q->open_sent) {
                q->state = SAILOR_NDP_SESSION_UP; q->at = now;
                q->open_sent = false;
                notify(s, slot, SAILOR_NDP_REASON_NONE, now);
            }
        } else {
            /* A NAME-matched peer OPEN establishes a fresh transport epoch.
             * Even at the same SA a reboot must clear old queued bytes/seqs. */
            reset(s, slot, SAILOR_NDP_SESSION_OPENING,
                  SAILOR_NDP_REASON_PEER_RESTART, now);
            if (control(s, slot, 0x11)) {
                q->state = SAILOR_NDP_SESSION_UP;
                notify(s, slot, SAILOR_NDP_REASON_NONE, now);
            } else {
                reset(s, slot, SAILOR_NDP_SESSION_RETRY_WAIT,
                      SAILOR_NDP_REASON_SEND_FAILED, now);
            }
        }
        return;
    }
    if (matched < 0) return;
    uint8_t slot = (uint8_t)matched;
    sailor_ndp_session_slot_t *q = &s->slots[slot];
    if (q->state != SAILOR_NDP_SESSION_UP) return;
    if (series <= 7) {
        if (n || !q->count) return;
        sailor_ndp_session_packet_t *packet = &q->queue[q->head];
        /* A failed retry must not invalidate the earlier successful send. */
        if (!packet->sent) return;
        if (series != q->tx_seq) {
            abort_and_retry(s, slot, SAILOR_NDP_REASON_BAD_SEQUENCE, now);
            return;
        }
        uint32_t cookie = packet->cookie;
        q->head = (uint8_t)((q->head + 1u) % SAILOR_NDP_SESSION_QUEUE);
        --q->count; q->tx_seq = (uint8_t)((q->tx_seq + 1u) & 7u);
        if (s->callbacks.complete)
            s->callbacks.complete(s->user, slot, cookie, true, now);
    } else if (series <= 15) {
        uint8_t seq = (uint8_t)(series & 7u);
        if (seq == q->rx_seq) {
            if (!s->callbacks.receive ||
                !s->callbacks.receive(s->user, slot, p, (uint16_t)n, now)) return;
            if (n) memcpy(q->last_rx, p, n);
            q->last_rx_len = (uint16_t)n;
            q->last_rx_seq = seq; q->have_rx = true;
            q->rx_seq = (uint8_t)((seq + 1u) & 7u);
        } else if (!q->have_rx || seq != q->last_rx_seq ||
                   n != q->last_rx_len || (n && memcmp(q->last_rx, p, n))) {
            abort_and_retry(s, slot, SAILOR_NDP_REASON_BAD_SEQUENCE, now);
            return;
        }
        /* Also re-ACK a consumed duplicate if the preceding ACK was lost. */
        (void)transmit(s, sa, seq, tx_channel(q), NULL, 0, true);
    }
}
void sailor_ndp_session_tick(sailor_ndp_session_t *s, uint32_t now) {
    if (!s || !legal_sa(s->local_sa) || !legal_name(s->local_name)) return;
    for (unsigned i = 0; i < SAILOR_NDP_SESSION_SLOTS; ++i) {
        sailor_ndp_session_slot_t *q = &s->slots[i];
        if (!bound(q) || !legal_sa(q->peer_sa) || q->peer_sa == s->local_sa)
            continue;
        if (q->state == SAILOR_NDP_SESSION_RETRY_WAIT) {
            if (!elapsed(now, q->at, SAILOR_NDP_SESSION_REOPEN_MS)) continue;
            reset(s, (uint8_t)i, SAILOR_NDP_SESSION_OPENING,
                  SAILOR_NDP_REASON_NONE, now);
        }
        if (q->state == SAILOR_NDP_SESSION_OPENING) {
            if (!q->open_attempted) {
                q->open_sent = control(s, (uint8_t)i, 0x10);
                q->open_attempted = true; q->at = now;
            } else if (elapsed(now, q->at, SAILOR_NDP_SESSION_RETRY_MS)) {
                abort_and_retry(s, (uint8_t)i,
                    q->open_sent ? SAILOR_NDP_REASON_TIMEOUT : SAILOR_NDP_REASON_SEND_FAILED,
                    now);
            }
            continue;
        }
        if (q->state != SAILOR_NDP_SESSION_UP || !q->count) continue;
        sailor_ndp_session_packet_t *packet = &q->queue[q->head];
        if (packet->attempts &&
            !elapsed(now, packet->sent_at, SAILOR_NDP_SESSION_RETRY_MS)) continue;
        if (packet->attempts == SAILOR_NDP_SESSION_ATTEMPTS) {
            abort_and_retry(s, (uint8_t)i,
                packet->sent ? SAILOR_NDP_REASON_TIMEOUT : SAILOR_NDP_REASON_SEND_FAILED,
                now);
            continue;
        }
        ++packet->attempts; packet->sent_at = now;
        bool sent = transmit(s, q->peer_sa, (uint8_t)(8u | q->tx_seq),
                             tx_channel(q), packet->data, packet->len, false);
        packet->sent = packet->sent || sent;
    }
}
