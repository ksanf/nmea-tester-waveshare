/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#include "sailor_status_format.h"
#include <stdio.h>
#include <string.h>

/* TT6006 GUI current-protocol table (4073 DATA[1]); deliberately separate
 * from registration and channel state, which are independent responses. */
static const char *const protocols[] = {
    "Free", "Sending message", "Receiving message", "Sending data report",
    "Sending data report", "Sending Distress Alert", "Requesting confirmation",
    "Logging in", "Logging out", "Performing link test",
    "Sending Distress Alert Test", "Tuning", "Scanning", "Pending",
    "Calibrating hardware", "Free", "Sending data report",
    "Receiving network info", "Receiving EGC", "Receiving assignment",
    "Receiving assignment control"
};
static const char *const oceans[] = {
    "Atlantic West", "Atlantic East", "Pacific", "Indian"
};

static void value(sailor_status_value_t *out, bool fresh, const char *text) {
    out->known = true;
    out->fresh = fresh;
    snprintf(out->text, sizeof(out->text), "%s", text);
}

void sailor_status_format_network(const protocol_antenna_status_t *s,
                                   sailor_network_text_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s) return;
    if (s->ocean_valid && s->ocean_region < sizeof(oceans)/sizeof(*oceans))
        value(&out->ocean, s->online && s->ocean_fresh, oceans[s->ocean_region]);
    if (s->registration_valid) {
        if (s->registration_state <= 1)
            value(&out->registration, s->online && s->registration_fresh,
                  s->registration_state == 0 ? "Logged in" : "Not logged in");
        else {
            out->registration.known = true;
            out->registration.fresh = s->online && s->registration_fresh;
            snprintf(out->registration.text, sizeof(out->registration.text),
                     "Unknown (%u)", (unsigned)s->registration_state);
        }
    }
    if (s->protocol_valid) {
        if (s->current_protocol < sizeof(protocols)/sizeof(*protocols))
            value(&out->protocol, s->online && s->protocol_fresh,
                  protocols[s->current_protocol]);
        else {
            out->protocol.known = true;
            out->protocol.fresh = s->online && s->protocol_fresh;
            snprintf(out->protocol.text, sizeof(out->protocol.text),
                     "Unknown (%u)", (unsigned)s->current_protocol);
        }
    }
    if (s->channel_valid && s->channel_number != 0 && s->channel_number != UINT16_MAX) {
        out->channel.known = true;
        out->channel.fresh = s->online && s->channel_fresh;
        snprintf(out->channel.text, sizeof(out->channel.text), "%u",
                 (unsigned)s->channel_number);
    }
}
