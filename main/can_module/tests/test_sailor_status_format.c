/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#include "sailor_status_format.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    sailor_network_text_t t;
    memset(&t, 0xff, sizeof(t));
    sailor_status_format_network(NULL, &t);
    assert(!t.ocean.known && !t.registration.known && !t.protocol.known && !t.channel.known);
    protocol_antenna_status_t s = {
        .online=true, .ocean_valid=true, .ocean_fresh=true, .ocean_region=2,
        .registration_valid=true, .registration_fresh=true, .registration_state=0,
        .protocol_valid=true, .protocol_fresh=true, .current_protocol=0,
        .channel_valid=true, .channel_fresh=true, .channel_number=12580
    };
    sailor_status_format_network(&s, &t);
    assert(!strcmp(t.ocean.text,"Pacific") && !strcmp(t.registration.text,"Logged in"));
    assert(!strcmp(t.protocol.text,"Free") && !strcmp(t.channel.text,"12580"));
    assert(t.ocean.fresh && t.registration.fresh && t.protocol.fresh && t.channel.fresh);
    /* One stale group must not suppress fresh data or regain freshness from it. */
    s.protocol_fresh=false; sailor_status_format_network(&s,&t);
    assert(t.protocol.known && !t.protocol.fresh && t.registration.fresh && t.channel.fresh);
    /* Disconnect keeps the last values visible but never presents them as live. */
    s.online=false; sailor_status_format_network(&s,&t);
    assert(t.ocean.known && t.registration.known && t.protocol.known && t.channel.known);
    assert(!t.ocean.fresh && !t.registration.fresh && !t.protocol.fresh && !t.channel.fresh);
    s.online=true; s.registration_state=1; s.ocean_valid=false;
    sailor_status_format_network(&s,&t);
    assert(!t.ocean.known && !t.ocean.text[0] && !strcmp(t.registration.text,"Not logged in"));
    /* Unknown enum codes must never become Free/Logged in by default. */
    s.registration_state=255; s.current_protocol=21; s.protocol_fresh=true;
    sailor_status_format_network(&s,&t);
    assert(!strcmp(t.registration.text,"Unknown (255)") && !strcmp(t.protocol.text,"Unknown (21)"));
    s.current_protocol=20; sailor_status_format_network(&s,&t);
    assert(!strcmp(t.protocol.text,"Receiving assignment control"));
    /* Keep known long vendor labels intact in both UIs' shared snapshot. */
    s.current_protocol=10; sailor_status_format_network(&s,&t);
    assert(!strcmp(t.protocol.text,"Sending Distress Alert Test"));
    s.channel_number=0; s.ocean_valid=true; s.ocean_region=4;
    sailor_status_format_network(&s,&t); assert(!t.channel.known && !t.ocean.known);
    s.channel_number=UINT16_MAX;
    sailor_status_format_network(&s,&t); assert(!t.channel.known);
    memset(&s,0,sizeof(s)); sailor_status_format_network(&s,&t);
    assert(!t.registration.known && !t.protocol.known && !t.channel.known);
    puts("SAILOR status formatting: known, unknown, stale and offline passed");
    return 0;
}
