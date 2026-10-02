/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
/* Replay an already established binary application stream; no hardware access.
 * Build with sailor_telemetry.c and -I../protocol/include.
 * Usage: replay_sailor_telemetry input.txt [retained-transmissions.txt]
 * Input: time_ms SA A|R HEX (decimal time/SA, contiguous packet hex).
 * A supplies selected-peer announcement application bytes and marks session UP.
 * R supplies one complete selected-peer binary envelope. Times must not decrease.
 * Discovery may be bootstrapped at time0 for midstream captures; this is NOT a
 * cold-start/session replay. Captured replies are never reordered or rewritten.
 * send() retains a copy; NDP completion follows tick(), never reenters callback.
 * Outbound polling is recorded, not treated as if the capture answered it.
 */
#include "sailor_telemetry.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t data[64]; uint16_t len; uint32_t cookie; } retained_t;
typedef struct {
    retained_t pending[16]; unsigned count;
    unsigned transmitted, controls, requests;
    FILE *output;
} sink_t;
static bool retain(void *user,const uint8_t *p,uint16_t n,uint32_t cookie) {
    sink_t *sink=user;
    assert(sink->count<16 && n<=64);
    retained_t *r=&sink->pending[sink->count++];
    r->len=n; r->cookie=cookie; memcpy(r->data,p,n);
    return true;
}
static void drain(sailor_telem_t *s,sink_t *sink,uint32_t now) {
    sailor_telem_tick(s,now);
    for(unsigned i=0;i<sink->count;i++) {
        retained_t *r=&sink->pending[i];
        assert(r->len>=3);
        uint16_t crc=(uint16_t)(r->data[r->len-2]|(r->data[r->len-1]<<8));
        assert(crc==sailor_telem_crc(r->data,r->len-2));
        sink->transmitted++; if(r->cookie) sink->requests++; else sink->controls++;
        if(sink->output) {
            fprintf(sink->output,"%"PRIu32" %"PRIu32" ",now,r->cookie);
            for(unsigned j=0;j<r->len;j++) fprintf(sink->output,"%02x",r->data[j]);
            fputc('\n',sink->output);
        }
        sailor_telem_tx_complete(s,r->cookie,true,now);
    }
    sink->count=0;
}
static int nibble(char c) {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
}
static size_t unhex(const char *text,uint8_t out[512]) {
    size_t n=strlen(text); assert(n%2==0 && n<=1024);
    for(size_t i=0;i<n;i+=2) {
        int a=nibble(text[i]),b=nibble(text[i+1]); assert(a>=0&&b>=0);
        out[i/2]=(uint8_t)((a<<4)|b);
    }
    return n/2;
}
static void unchanged_measurements(const sailor_telem_t *a,const sailor_telem_t *b) {
    assert(a->signal_at==b->signal_at && a->position_at==b->position_at);
    assert(a->clock_at==b->clock_at && a->clock_utc==b->clock_utc);
    assert(!memcmp(&a->status,&b->status,sizeof(a->status)));
}
int main(int argc,char **argv) {
    if(argc<2 || argc>3) { fprintf(stderr,"usage: %s input.txt [tx.txt]\n",argv[0]); return 2; }
    FILE *in=fopen(argv[1],"r"); if(!in) { perror(argv[1]); return 2; }
    sink_t sink={0};
    if(argc==3) { sink.output=fopen(argv[2],"w"); if(!sink.output) { perror(argv[2]); fclose(in); return 2; } }
    sailor_telem_t s; sailor_telem_init(&s,retain,&sink);
    char line[1200],hex[1025],kind; unsigned sa,lineno=0,received=0,duplicates=0,corrupt=0;
    unsigned signal_packets=0,position_packets=0,clock_packets=0,crc_good=0;
    uint32_t now=0,previous=0; bool have_time=false,announced=false;
    while(fgets(line,sizeof(line),in)) {
        lineno++; if(line[0]=='#' || line[0]=='\n') continue;
        if(sscanf(line,"%"SCNu32" %u %c %1024s",&now,&sa,&kind,hex)!=4 || sa>=254) {
            fprintf(stderr,"bad input at line%u\n",lineno); return 2;
        }
        assert(!have_time || now>=previous); previous=now; have_time=true;
        uint8_t bytes[512]; size_t n=unhex(hex,bytes);
        if(kind=='A') {
            assert(sailor_telem_discover(&s,(uint8_t)sa,bytes,n,now));
            sailor_telem_on_session(&s,true,now); announced=true; drain(&s,&sink,now);
            continue;
        }
        assert(kind=='R' && announced && sa==s.peer_sa && n>=3);
        uint16_t wire_crc=(uint16_t)(bytes[n-2]|(bytes[n-1]<<8));
        crc_good+=wire_crc==sailor_telem_crc(bytes,n-2);
        assert(sailor_telem_rx(&s,bytes,n,now)); received++;
        drain(&s,&sink,now);
        /* Branch copies probe duplicates/CRC errors without modifying timeline. */
        sailor_telem_t copy=s;
        assert(sailor_telem_rx(&copy,bytes,n,now+1u));
        unchanged_measurements(&s,&copy); duplicates++;
        memcpy(&copy,&s,sizeof(copy)); bytes[n-1]^=1;
        assert(sailor_telem_rx(&copy,bytes,n,now+1u));
        unchanged_measurements(&s,&copy); corrupt++; bytes[n-1]^=1;
        if(n>=14 && bytes[0]==2 && bytes[8]==2) {
            unsigned op=((unsigned)bytes[9]<<8)|bytes[10];
            signal_packets+=op==0x4050; position_packets+=op==0x4108; clock_packets+=op==0x4004;
        }
    }
    assert(announced && received>0 && crc_good==received);
    protocol_antenna_status_t final,stale,offline;
    sailor_telem_snapshot(&s,now,&final);
    sailor_telem_snapshot(&s,now+6000u,&stale);
    sailor_telem_snapshot(&s,now+16001u,&offline);
    assert(!stale.position_fresh && stale.position_valid==final.position_valid);
    assert(!offline.online && !offline.signal_valid && !offline.position_fresh);
    assert(offline.position_valid==final.position_valid);
    if(final.position_valid) assert(offline.latitude==final.latitude && offline.longitude==final.longitude);
    printf("{\"packets\":%u,\"crc_valid\":%u,\"duplicate_probes\":%u,\"bad_crc_probes\":%u,"
           "\"signal_packets\":%u,\"position_packets\":%u,\"clock_packets\":%u,"
           "\"retained_controls\":%u,\"recorded_poll_sends\":%u,\"duration_ms\":%"PRIu32","
           "\"serial\":\"%s\",\"online\":%s,\"cn0\":%u,\"bars\":%u,"
           "\"position_valid\":%s,\"position_fresh\":%s,\"position_utc\":%"PRIu32","
           "\"latitude\":%.9f,\"longitude\":%.9f,\"stale_check\":\"PASS\"}\n",
           received,crc_good,duplicates,corrupt,signal_packets,position_packets,clock_packets,
           sink.controls,sink.requests,now,final.serial,final.online?"true":"false",final.cn0_dbhz,final.signal_bars,
           final.position_valid?"true":"false",final.position_fresh?"true":"false",final.position_utc,
           final.latitude,final.longitude);
    fclose(in); if(sink.output) fclose(sink.output);
    return 0;
}
