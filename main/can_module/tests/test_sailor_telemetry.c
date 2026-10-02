/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#include "sailor_telemetry.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint8_t data[32]; uint16_t len; uint32_t cookie; } sent_t;
typedef struct { sent_t sent[128]; unsigned count; int available; } wire_t;
static bool send_cb(void *user,const uint8_t *data,uint16_t len,uint32_t cookie) {
    wire_t *w=user;
    if(!w->available) return false;
    assert(w->count<128 && len<=sizeof(w->sent[0].data));
    sent_t *p=&w->sent[w->count++]; p->len=len; p->cookie=cookie; memcpy(p->data,data,len);
    if(w->available>0) w->available--;
    return true;
}
static const uint8_t announce[]={
    0x02, 0x03, 0x45, 0x23, 0xe1, 0x2b, 0x00, 0xaa, 0x8c, 0xc0, 0x02, 0x00,
    0x07, 0x00, 0x01, 0x07, 0x03, 0x00, 0x38, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x0b, 0x02, 0x00, 0x0a, 0x06, 0x00, 0x01,
};
/* Synthetic wire fixtures following observed packet layouts. Device identity,
 * UTC and coordinates are examples; modified envelopes have recomputed CRC-16/X25. */
static const uint8_t v4050[]={0x2,0x0,0x5,0x1,0x10,0x20,0xc1,0x7f,0x2,0x40,0x50,0x1,0x29,0x0,0x5,0x40,0x1,0x0,0x0,0x0,0x0,0x0,0x0,0x3,0x3e,0x58};
static const uint8_t v4108[]={
    0x02, 0x00, 0x04, 0x01, 0x10, 0x20, 0x7a, 0x63, 0x02, 0x41, 0x08, 0x01,
    0x67, 0x74, 0x85, 0x80, 0x00, 0x0c, 0x22, 0x02, 0x37, 0x00, 0x38, 0x07,
    0x03, 0x7a, 0x84, 0x30, 0x00, 0x01, 0x02, 0x03, 0xaf, 0xbf,
};
static const uint8_t v4004[]={
    0x02, 0x00, 0x06, 0x01, 0x10, 0x20, 0x0c, 0x5a, 0x02, 0x40, 0x04, 0x67,
    0x74, 0x85, 0x80, 0x03, 0x7d, 0x8d,
};

static uint16_t sequence(const sent_t *p) { return (uint16_t)((p->data[1]<<8)|p->data[2]); }
static void crc_at(uint8_t *p,size_t n) {
    uint16_t c=sailor_telem_crc(p,n); p[n]=(uint8_t)c; p[n+1]=(uint8_t)(c>>8);
}
static void fix(uint8_t *p,size_t n,uint16_t seq) {
    p[1]=(uint8_t)(seq>>8); p[2]=(uint8_t)seq; crc_at(p,6); crc_at(p,n-2);
}
static size_t control(uint8_t *p,uint16_t seq,uint8_t kind) {
    p[0]=2; p[1]=(uint8_t)(seq>>8); p[2]=(uint8_t)seq;
    p[3]=1; p[4]=0x10; p[5]=0x20; crc_at(p,6); p[8]=kind; crc_at(p,9); return 11;
}
static void freeze_polls(sailor_telem_t *s,uint32_t now) {
    for(unsigned i=0;i<SAILOR_TELEM_POLLS;i++) s->polls[i].due=now+1000000u;
}
static void ready(sailor_telem_t *s,wire_t *w,uint32_t now) {
    memset(w,0,sizeof(*w)); w->available=-1;
    sailor_telem_init(s,send_cb,w);
    assert(sailor_telem_discover(s,0,announce,sizeof(announce),now));
    sailor_telem_on_session(s,true,now); freeze_polls(s,now); sailor_telem_tick(s,now);
    assert(w->count==1 && w->sent[0].cookie==0 && w->sent[0].data[8]==5);
    assert(sequence(&w->sent[0])==0x7fff);
    w->count=0;
}
static unsigned outstanding(const sailor_telem_t *s) {
    unsigned count=0;
    for(unsigned i=0;i<SAILOR_TELEM_UPPER_WINDOW;i++) count+=s->upper[i].state!=SAILOR_TELEM_TX_FREE;
    return count;
}
static void rx_control(sailor_telem_t *s,uint16_t seq,uint8_t kind,uint32_t now) {
    uint8_t p[16]; size_t n=control(p,seq,kind); assert(sailor_telem_rx(s,p,n,now));
}
static void test_wire_fixtures_and_validation(void) {
    sailor_telem_t s; wire_t w; protocol_antenna_status_t out;
    ready(&s,&w,100); sailor_telem_snapshot(&s,100,&out);
    assert(out.identity_valid && !strcmp(out.serial,"80000001"));
    assert(!out.online && out.signal_age_ms==UINT32_MAX && out.position_age_ms==UINT32_MAX);
    uint8_t p[64];
    memcpy(p,v4050,sizeof(v4050)); p[12]^=1;
    assert(sailor_telem_rx(&s,p,sizeof(v4050),110));
    assert(!s.have_signal && !s.have_rx && s.control_count==1);
    sailor_telem_tick(&s,110);
    assert(w.sent[0].data[8]==0x15 && w.sent[0].data[4]==0x20 && w.sent[0].data[5]==0x10);
    /* A valid outer CRC must not mask a corrupt header CRC. */
    memcpy(p,v4050,sizeof(v4050)); p[6]^=1; crc_at(p,sizeof(v4050)-2);
    assert(sailor_telem_rx(&s,p,sizeof(v4050),120) && !s.control_count && !s.have_rx);
    assert(sailor_telem_rx(&s,v4050,sizeof(v4050),130));
    sailor_telem_snapshot(&s,130,&out);
    assert(out.online && out.signal_valid && out.cn0_dbhz==41 && out.signal_bars==5);
    sailor_telem_tick(&s,130); assert(w.sent[1].data[8]==6 && w.sent[1].cookie==0);
    assert(sailor_telem_rx(&s,v4050,sizeof(v4050),630));
    sailor_telem_snapshot(&s,630,&out); assert(out.signal_age_ms==500);
    sailor_telem_tick(&s,630); assert(w.count==3 && !memcmp(w.sent[1].data,w.sent[2].data,11));
    /* Duplicate identity is sequence+fragment, independent of payload hash. */
    memcpy(p,v4050,sizeof(v4050)); p[12]=22; crc_at(p,sizeof(v4050)-2);
    assert(sailor_telem_rx(&s,p,sizeof(v4050),640) && s.signal_at==130);
    sailor_telem_tick(&s,640);
    assert(sailor_telem_rx(&s,v4108,sizeof(v4108),700));
    assert(sailor_telem_rx(&s,v4004,sizeof(v4004),710));
    sailor_telem_snapshot(&s,710,&out);
    assert(out.position_valid && out.position_fresh);
    assert(fabs(out.latitude-(12+34.567/60))<1e-9);
    assert(fabs(out.longitude-(56+7.890/60))<1e-9);
    /* The last THREE accepted identities suppress upper retransmissions. */
    assert(sailor_telem_rx(&s,v4050,sizeof(v4050),720) && s.signal_at==130);
    sailor_telem_tick(&s,720);
    /* Short/unknown application payloads are consumed, ACKed and never decoded. */
    memcpy(p,v4050,sizeof(v4050)); p[9]=0x40; p[10]=0x05; fix(p,sizeof(v4050),20);
    assert(sailor_telem_rx(&s,p,sizeof(v4050),730));
    assert(!strcmp(s.status.serial,"80000001"));
    sailor_telem_tick(&s,730);
    memcpy(p,v4050,sizeof(v4050)); p[sizeof(v4050)-3]=4; fix(p,sizeof(v4050),21);
    assert(sailor_telem_rx(&s,p,sizeof(v4050),740));
    assert(s.controls[s.control_head].data[8]==0x15 && s.signal_at==130);
    sailor_telem_tick(&s,740);
    /* Truncation boundaries and null input must not read past the supplied data. */
    for(size_t n=0;n<sizeof(v4108);n++) assert(sailor_telem_rx(&s,v4108,n,750));
    assert(sailor_telem_rx(&s,NULL,0,751));
    sailor_telem_tick(&s,752);
    sailor_telem_snapshot(&s,17000,&out);
    assert(!out.online && !out.signal_valid && out.position_valid && !out.position_fresh);
}
static void test_coordinates_and_stale(void) {
    sailor_telem_t s; wire_t w; protocol_antenna_status_t out; uint8_t p[64];
    ready(&s,&w,100);
    assert(sailor_telem_rx(&s,v4108,sizeof(v4108),110));
    assert(sailor_telem_rx(&s,v4004,sizeof(v4004),120));
    sailor_telem_snapshot(&s,120,&out); assert(out.position_fresh);
    memcpy(p,v4108,sizeof(v4108)); p[16]=1; p[21]=1; fix(p,sizeof(v4108),30);
    assert(sailor_telem_rx(&s,p,sizeof(v4108),130));
    sailor_telem_snapshot(&s,130,&out);
    assert(fabs(out.latitude+(12+34.567/60))<1e-9 && fabs(out.longitude+(56+7.890/60))<1e-9);
    double lat=out.latitude,lon=out.longitude;
    p[16]=2; fix(p,sizeof(v4108),31); assert(sailor_telem_rx(&s,p,sizeof(v4108),140));
    assert(s.status.latitude==lat && s.position_at==130);
    p[16]=0; p[18]=60; fix(p,sizeof(v4108),32); assert(sailor_telem_rx(&s,p,sizeof(v4108),150));
    assert(s.status.longitude==lon && s.position_at==130);
    sailor_telem_tick(&s,160);
    memcpy(p,v4108,sizeof(v4108)); p[11]=0; fix(p,sizeof(v4108),33);
    assert(sailor_telem_rx(&s,p,sizeof(v4108),170));
    sailor_telem_snapshot(&s,170,&out);
    assert(out.position_valid && !out.position_fresh && out.latitude==lat && s.position_at==130);
    memcpy(p,v4108,sizeof(v4108)); p[11]=2; /* GUI uses nonzero, not enum==1. */
    memset(p+16,0,10); fix(p,sizeof(v4108),34);
    assert(sailor_telem_rx(&s,p,sizeof(v4108),180));
    assert(s.status.latitude==0 && s.status.longitude==0);
    /* Exact poles/antimeridian are valid, fractions beyond them are not. */
    p[17]=90; p[22]=180; fix(p,sizeof(v4108),35);
    assert(sailor_telem_rx(&s,p,sizeof(v4108),190));
    assert(s.status.latitude==90 && s.status.longitude==180);
    p[20]=1; fix(p,sizeof(v4108),36); assert(sailor_telem_rx(&s,p,sizeof(v4108),200));
    assert(s.position_at==190);
    sailor_telem_tick(&s,210);
    /* A new sequence carrying an old GPS epoch does not make it fresh. */
    memcpy(p,v4108,sizeof(v4108)); fix(p,sizeof(v4108),37);
    assert(sailor_telem_rx(&s,p,sizeof(v4108),7000));
    sailor_telem_snapshot(&s,7000,&out); assert(out.position_valid && !out.position_fresh);
    sailor_telem_on_session(&s,false,7010); sailor_telem_snapshot(&s,7010,&out);
    assert(!out.online && out.position_valid && !out.position_fresh && !s.control_count && !outstanding(&s));
    sailor_telem_on_session(&s,true,7020); freeze_polls(&s,7020);
    assert(sailor_telem_rx(&s,v4050,sizeof(v4050),7030));
    sailor_telem_snapshot(&s,7030,&out); assert(out.online && out.signal_valid && !out.position_fresh);
}
static void test_upper_timer_and_retry(void) {
    sailor_telem_t s; wire_t w; ready(&s,&w,100);
    s.polls[0].due=100; sailor_telem_tick(&s,100);
    assert(w.count==1 && w.sent[0].cookie && outstanding(&s)==1);
    sent_t initial=w.sent[0];
    assert(s.upper[0].state==SAILOR_TELEM_TX_NDP);
    sailor_telem_tick(&s,20000); assert(w.count==1); /* no upper timer before NDP ACK */
    sailor_telem_tx_complete(&s,initial.cookie,true,25000);
    assert(s.upper[0].state==SAILOR_TELEM_TX_ACK && s.upper[0].due==35000);
    sailor_telem_tick(&s,34999); assert(w.count==1);
    sailor_telem_tick(&s,35000); assert(w.count==2 && s.upper[0].retries==1);
    assert(w.sent[1].cookie!=initial.cookie && !memcmp(w.sent[1].data,initial.data,initial.len));
    sailor_telem_tx_complete(&s,initial.cookie,true,36000); /* late old completion ignored */
    assert(s.upper[0].state==SAILOR_TELEM_TX_NDP);
    sailor_telem_tick(&s,59000); assert(w.count==2);
    sailor_telem_tx_complete(&s,w.sent[1].cookie,true,60000);
    sailor_telem_tick(&s,69999); assert(w.count==2);
    sailor_telem_tick(&s,70000); assert(w.count==3 && s.upper[0].retries==2);
    assert(!memcmp(w.sent[2].data,initial.data,initial.len));
    sailor_telem_tx_complete(&s,w.sent[2].cookie,true,71000);
    sailor_telem_tick(&s,81000); assert(w.count==3 && !outstanding(&s));
    assert(!s.polls[0].pending); /* no fourth transmission */
    /* A transport enqueue refusal consumes neither upper retry budget nor timer. */
    ready(&s,&w,100); s.polls[0].due=100; w.available=0; sailor_telem_tick(&s,100);
    assert(w.count==0 && s.upper[0].state==SAILOR_TELEM_TX_QUEUED && !s.upper[0].sent);
    sailor_telem_tick(&s,20000); assert(!w.count && !s.upper[0].retries);
    w.available=-1; sailor_telem_tick(&s,20001); assert(w.count==1);
    sailor_telem_tx_complete(&s,w.sent[0].cookie,false,20002);
    assert(!outstanding(&s)); /* lower failure does not become an upper timeout */
    /* Wrapping uint32 clock: due checks retain the ten-second interval. */
    uint32_t start=UINT32_MAX-500;
    ready(&s,&w,start); s.polls[0].due=start; sailor_telem_tick(&s,start);
    sailor_telem_tx_complete(&s,w.sent[0].cookie,true,start);
    sailor_telem_tick(&s,start+9999u); assert(w.count==1);
    sailor_telem_tick(&s,start+10000u); assert(w.count==2);
}
static void test_ack_nak_window_and_late_events(void) {
    sailor_telem_t s; wire_t w; ready(&s,&w,100);
    s.polls[0].due=100; s.polls[1].due=100; s.polls[2].due=100;
    sailor_telem_tick(&s,100); assert(w.count==2 && outstanding(&s)==2);
    assert(w.sent[0].data[9]==0x20 && w.sent[0].data[10]==4);
    assert(w.sent[1].data[9]==0x21 && w.sent[1].data[10]==8);
    sailor_telem_tick(&s,101); assert(w.count==2); /* bounded two-packet window */
    uint32_t old_cookie=w.sent[1].cookie;
    rx_control(&s,0x9999,6,102); assert(outstanding(&s)==2);
    /* ACK for the later packet retransmits the oldest, not cumulative removal. */
    rx_control(&s,sequence(&w.sent[1]),6,110);
    assert(outstanding(&s)==1 && s.upper[0].state==SAILOR_TELEM_TX_RETRY);
    assert(s.polls[1].pending); /* ACK is not the requested GPS response. */
    sailor_telem_tick(&s,111); assert(outstanding(&s)==2 && w.count==4);
    assert(!memcmp(w.sent[2].data,w.sent[0].data,w.sent[0].len));
    uint32_t new_cookie=s.upper[1].cookie;
    assert(new_cookie!=old_cookie && s.upper[1].state==SAILOR_TELEM_TX_NDP);
    sailor_telem_tx_complete(&s,old_cookie,true,112);
    assert(s.upper[1].state==SAILOR_TELEM_TX_NDP && s.upper[1].cookie==new_cookie);
    /* ACK can arrive before lower completion; late completion cannot revive it. */
    uint32_t acked_cookie=s.upper[0].cookie;
    rx_control(&s,s.upper[0].sequence,6,113);
    sailor_telem_tx_complete(&s,acked_cookie,true,114);
    assert(s.upper[0].state==SAILOR_TELEM_TX_FREE);
    /* NAK for second queued packet schedules both in sequence order. */
    ready(&s,&w,100); s.polls[0].due=100; s.polls[1].due=100; sailor_telem_tick(&s,100);
    rx_control(&s,sequence(&w.sent[1]),0x15,110);
    assert(s.upper[0].state==SAILOR_TELEM_TX_RETRY && s.upper[1].state==SAILOR_TELEM_TX_RETRY);
    sailor_telem_tick(&s,111); assert(w.count==4);
    assert(!memcmp(w.sent[0].data,w.sent[2].data,14) && !memcmp(w.sent[1].data,w.sent[3].data,14));
    assert(s.upper[0].retries==1 && s.upper[1].retries==1);
    /* Reused array slot0 can be newer than slot1: retry wire order follows
     * transmission age, never physical slot index. */
    ready(&s,&w,100); s.polls[0].due=100; s.polls[1].due=100; sailor_telem_tick(&s,100);
    rx_control(&s,sequence(&w.sent[0]),6,110);
    s.polls[0].pending=false; s.polls[0].due=111; sailor_telem_tick(&s,111);
    assert(w.count==3 && sequence(&w.sent[2])!=sequence(&w.sent[0]));
    rx_control(&s,sequence(&w.sent[2]),0x15,112); sailor_telem_tick(&s,113);
    assert(w.count==5 && !memcmp(w.sent[3].data,w.sent[1].data,14));
    assert(!memcmp(w.sent[4].data,w.sent[2].data,14));
    /* Explicit reconnect clears upper queue/history, but preserves monotonic cookies. */
    old_cookie=s.upper[0].cookie; uint16_t old_seq=s.upper[0].sequence;
    sailor_telem_on_session(&s,false,120); assert(!outstanding(&s));
    sailor_telem_on_session(&s,true,130); freeze_polls(&s,130); s.polls[0].due=130;
    sailor_telem_tick(&s,130); assert(outstanding(&s)==1 && s.upper[0].sequence!=old_seq);
    new_cookie=s.upper[0].cookie; sailor_telem_tx_complete(&s,old_cookie,false,140);
    rx_control(&s,old_seq,6,141);
    assert(outstanding(&s)==1 && s.upper[0].cookie==new_cookie && s.upper[0].state==SAILOR_TELEM_TX_NDP);
    /* Logical sequence wrap is encoded BE and never returns reserved FFFE/FFFF. */
    ready(&s,&w,100); s.binary_seq=0xfffd; s.polls[0].due=100; s.polls[1].due=100;
    sailor_telem_tick(&s,100); assert(sequence(&w.sent[0])==0xfffd && sequence(&w.sent[1])==1);
}
static void test_backpressure_and_unsupported(void) {
    sailor_telem_t s; wire_t w; uint8_t p[64]; ready(&s,&w,100); w.available=0;
    for(unsigned i=0;i<SAILOR_TELEM_CONTROL_QUEUE;i++) rx_control(&s,(uint16_t)(100+i),5,110+i);
    assert(s.control_count==SAILOR_TELEM_CONTROL_QUEUE && !w.count);
    uint32_t last=s.last_rx;
    assert(!sailor_telem_rx(&s,v4108,sizeof(v4108),130));
    assert(!s.have_position && s.last_rx==last && !s.rx_count);
    /* Retained identical reply can satisfy duplicate ENQ even when queue is full. */
    rx_control(&s,100,5,131); assert(s.control_count==SAILOR_TELEM_CONTROL_QUEUE);
    memcpy(p,v4050,sizeof(v4050)); p[12]^=1;
    assert(!sailor_telem_rx(&s,p,sizeof(v4050),132));
    p[6]^=1; assert(sailor_telem_rx(&s,p,sizeof(v4050),133)); /* bad header is discard, not pressure */
    w.available=1; sailor_telem_tick(&s,140); assert(s.control_count==SAILOR_TELEM_CONTROL_QUEUE-1);
    assert(sailor_telem_rx(&s,v4108,sizeof(v4108),141) && s.have_position && s.position_at==141);
    w.available=-1; sailor_telem_tick(&s,150); assert(!s.control_count);
    /* Multipart telemetry cannot publish a plausible partial fragment. */
    ready(&s,&w,100); memcpy(p,v4050,sizeof(v4050)); p[sizeof(v4050)-3]=0x17;
    fix(p,sizeof(v4050),200); assert(sailor_telem_rx(&s,p,sizeof(v4050),110));
    assert(!s.have_signal && s.control_count==1);
    p[3]=2; p[sizeof(v4050)-3]=3; fix(p,sizeof(v4050),200);
    assert(sailor_telem_rx(&s,p,sizeof(v4050),111) && !s.have_signal && s.control_count==2);
    assert(s.controls[s.control_head].data[8]==6);
    sailor_telem_tick(&s,120);
    /* Compact C1E0 event layout: verify the exact compact upper ACK. */
    const uint8_t event[]={0x10,0,1,2,0xc1,0xe0,0,0x75,0,0,3,0x57,0x62};
    const uint8_t ack[]={0x10,0,1,6,0x91,0x43};
    assert(sailor_telem_rx(&s,event,sizeof(event),130));
    assert(s.controls[s.control_head].len==sizeof(ack));
    assert(!memcmp(s.controls[s.control_head].data,ack,sizeof(ack)));
    assert(!s.have_signal && !s.have_position);
    /* A compact packet is not an addressed telemetry packet. */
    uint8_t compact[32]={0x10,0,2,2,0x40,0x50};
    memcpy(compact+6,v4050+11,12); compact[18]=3; crc_at(compact,19);
    assert(sailor_telem_rx(&s,compact,21,140) && !s.have_signal);
}
static void test_selected_discovery(void) {
    sailor_telem_t s; wire_t w; uint8_t p[sizeof(announce)]; protocol_antenna_status_t out;
    ready(&s,&w,100); assert(sailor_telem_rx(&s,v4108,sizeof(v4108),110));
    assert(!sailor_telem_discover(&s,0,announce,sizeof(announce)-1,120));
    memcpy(p,announce,sizeof(p)); p[50]=2;
    assert(!sailor_telem_discover(&s,0,p,sizeof(p),120));
    /* Remote port is advertised and owned by NDP, never hardcoded to B. */
    memcpy(p,announce,sizeof(p)); p[52]=4;
    assert(sailor_telem_discover(&s,2,p,sizeof(p),130));
    assert(s.peer_sa==2 && s.accepted && s.have_position);
    /* A delayed announcement must not stop a session already reopened by the
     * manager after its validated NAME address-claim migration. */
    sailor_telem_on_session(&s,false,131); sailor_telem_on_session(&s,true,132);
    freeze_polls(&s,132);
    assert(sailor_telem_discover(&s,4,p,sizeof(p),140) && s.accepted);
    s.polls[0].due=140; sailor_telem_tick(&s,140); assert(outstanding(&s)==1);
    /* Only caller-selected NAME changes reach this API. Such a change clears stale cache. */
    p[2]^=1; memcpy(p+18,"1234567890123456",16); p[34]=0;
    assert(sailor_telem_discover(&s,3,p,sizeof(p),150));
    sailor_telem_snapshot(&s,150,&out);
    assert(!out.position_valid && !out.online && !strcmp(out.serial,"1234567890123456"));
    assert(out.identity_valid && !s.accepted);
    sailor_telem_disconnect(&s,160); assert(!s.discovered && s.peer_sa==0xff);
    sailor_telem_on_session(&s,true,170); assert(!s.accepted);
    /* Count is an actual service count, not a fixed protocol version. */
    memcpy(p,announce,sizeof(p)); p[1]=1; p[52]=7;
    assert(sailor_telem_discover(&s,0,p,53,180));
    p[1]=2; memcpy(p+53,p+50,3);
    assert(!sailor_telem_discover(&s,0,p,56,181)); /* duplicate binary service */
    p[1]=17; assert(!sailor_telem_discover(&s,0,p,sizeof(p),182));
    memcpy(p,announce,sizeof(p)); p[10]=1;
    assert(!sailor_telem_discover(&s,0,p,sizeof(p),183)); /* terminal, not antenna */
}

/* Fixed wire-format regression envelopes for 404C, 4073, 4043 and 4054.
 * Unsupported registration detail bytes are synthetic; packet CRCs remain checked. */
static void rx_hex(sailor_telem_t *s,const char *hex,uint32_t now) {
    uint8_t p[64]; size_t n=strlen(hex)/2;
    assert(n<=sizeof(p) && strlen(hex)==n*2);
    for(size_t i=0;i<n;i++) { unsigned b; assert(sscanf(hex+i*2,"%2x",&b)==1); p[i]=(uint8_t)b; }
    assert(sailor_telem_rx(s,p,n,now));
}
static void rx_application(sailor_telem_t *s,uint16_t sequence_number,uint16_t op,
                            const uint8_t *data,size_t count,uint32_t now) {
    uint8_t p[64]={2,0,0,1,0x10,0x20,0,0,2};
    assert(count<=sizeof(p)-14);
    p[9]=(uint8_t)(op>>8); p[10]=(uint8_t)op;
    memcpy(p+11,data,count); p[11+count]=3;
    fix(p,14+count,sequence_number);
    assert(sailor_telem_rx(s,p,14+count,now));
}
static void test_network_fixtures_and_lifecycle(void) {
    sailor_telem_t s; wire_t w; protocol_antenna_status_t out;
    ready(&s,&w,100); sailor_telem_snapshot(&s,100,&out);
    assert(!out.registration_valid && !out.protocol_valid && !out.channel_valid && !out.ocean_valid);
    assert(out.registration_age_ms==UINT32_MAX && out.protocol_age_ms==UINT32_MAX);
    assert(out.channel_age_ms==UINT32_MAX && out.channel_state_age_ms==UINT32_MAX);
    rx_hex(&s,"021019011020149f02404c01deadbeef0012345678010101010102000003ce06",110);
    rx_hex(&s,"02101b01102062a602407300000000040403c0ea",120);
    rx_hex(&s,"021016011020ed2d02404301031e4e",130);
    rx_hex(&s,"02101701102056310240540100f431240107037545",140);
    sailor_telem_snapshot(&s,150,&out);
    assert(out.registration_valid && out.registration_fresh && out.registration_state==1);
    assert(!out.ocean_valid && !out.ocean_fresh && !out.registered_ncs && !out.registered_channel);
    assert(out.protocol_valid && out.protocol_fresh && out.current_protocol==0);
    assert(out.channel_valid && out.channel_fresh && out.channel_number==12580);
    assert(out.tdm_state==1 && out.tdm_origin==244 && out.tdm_frame==263);
    assert(out.channel_state_valid && out.channel_state_fresh && out.channel_state==1);
    assert(out.registration_age_ms==40 && out.protocol_age_ms==30 && out.channel_age_ms==10);
    sailor_telem_tick(&s,150); w.count=0;
    /* The same-sequence retransmission cannot refresh the group's timestamp. */
    rx_hex(&s,"02101701102056310240540100f431240107037545",500);
    sailor_telem_snapshot(&s,500,&out); assert(out.channel_age_ms==360);
    sailor_telem_tick(&s,500); w.count=0;
    /* 404C short and extended versions agree on the registered NCS/channel. */
    uint8_t reg[18]={0,0,244,0x31,0x24,0,0xca,0x5b,0xa1,0xd5,0x48,1,1,1,1,1,2,0};
    rx_application(&s,6000,0x404c,reg,sizeof(reg),600);
    sailor_telem_snapshot(&s,600,&out);
    assert(out.registration_state==0 && out.registered_ncs==244 && out.registered_channel==12580);
    assert(out.ocean_valid && out.ocean_fresh && out.ocean_region==2);
    /* Other packet traffic cannot refresh a stale group. The age limit is inclusive. */
    uint8_t proto[6]={0,11,0,0,0,0};
    rx_application(&s,6001,0x4073,proto,sizeof(proto),15600);
    sailor_telem_snapshot(&s,15600,&out);
    assert(out.registration_fresh && out.ocean_fresh && out.protocol_fresh && !out.channel_fresh);
    sailor_telem_snapshot(&s,15601,&out);
    assert(!out.registration_fresh && !out.ocean_fresh && out.protocol_fresh && out.current_protocol==11);
    sailor_telem_tick(&s,15601); w.count=0;
    /* A same-peer disconnect/reconnect preserves cache, never its freshness.
     * Receiving only protocol data cannot revive the cached region/channel. */
    sailor_telem_on_session(&s,false,15610);
    sailor_telem_on_session(&s,true,15620); freeze_polls(&s,15620);
    rx_application(&s,6002,0x4073,proto,sizeof(proto),15630);
    sailor_telem_snapshot(&s,15630,&out);
    assert(out.online && out.protocol_fresh && out.registration_valid && out.ocean_valid && out.channel_valid);
    assert(!out.registration_fresh && !out.ocean_fresh && !out.channel_fresh && !out.channel_state_fresh);
    rx_application(&s,6003,0x404c,reg,5,15640);
    sailor_telem_snapshot(&s,15640,&out); assert(out.registration_fresh && out.ocean_fresh && !out.channel_fresh);
    /* Selecting a different physical NAME removes the previous antenna cache. */
    uint8_t other[sizeof(announce)]; memcpy(other,announce,sizeof(other)); other[2]^=1;
    assert(sailor_telem_discover(&s,2,other,sizeof(other),15650));
    sailor_telem_snapshot(&s,15650,&out);
    assert(!out.registration_valid && !out.protocol_valid && !out.channel_valid && !out.channel_state_valid && !out.ocean_valid);
    assert(out.registration_age_ms==UINT32_MAX && out.channel_age_ms==UINT32_MAX);
}
static void test_network_bounds_and_unavailable(void) {
    sailor_telem_t s; wire_t w; protocol_antenna_status_t out; uint16_t seq=1000;
    ready(&s,&w,100);
    uint8_t data[20]={0,0,244,0x31,0x24};
    for(size_t n=0;n<18;n++) {
        if(n==5) continue;
        rx_application(&s,seq++,0x404c,data,n,110);
        sailor_telem_snapshot(&s,110,&out); assert(!out.registration_valid);
        sailor_telem_tick(&s,110); w.count=0;
    }
    /* Valid current oceans use precisely the vendor guard 1..399 / 100. */
    const uint16_t ids[]={0,1,99,100,199,200,244,299,300,399,400,65535};
    for(unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);i++) {
        data[1]=(uint8_t)(ids[i]>>8); data[2]=(uint8_t)ids[i];
        rx_application(&s,seq++,0x404c,data,5,120+i);
        sailor_telem_snapshot(&s,120+i,&out);
        assert(out.registration_valid && out.registration_state==0);
        assert(out.ocean_valid==(ids[i]>=1 && ids[i]<=399));
        if(out.ocean_valid) assert(out.ocean_region==ids[i]/100);
        sailor_telem_tick(&s,120+i); w.count=0;
    }
    /* An unregistered report must not reuse NCS-looking bytes, nor old ocean. */
    data[0]=1; data[1]=0; data[2]=244;
    rx_application(&s,seq++,0x404c,data,5,200);
    sailor_telem_snapshot(&s,200,&out);
    assert(out.registration_state==1 && !out.ocean_valid && !out.registered_ncs && !out.registered_channel);
    data[0]=254; rx_application(&s,seq++,0x404c,data,1,201);
    sailor_telem_snapshot(&s,201,&out);
    assert(out.registration_state==254 && out.registration_fresh && !out.ocean_valid);
    sailor_telem_tick(&s,201); w.count=0;
    /* Unsupported enum values are retained raw for an explicit Unknown display. */
    uint8_t protocol[6]={0,254}; rx_application(&s,seq++,0x4073,protocol,6,210);
    uint8_t chan[7]={1,0,244,0x31,0x24,1,7};
    rx_application(&s,seq++,0x4054,chan,7,211);
    uint8_t state[]={255}; rx_application(&s,seq++,0x4043,state,1,212);
    sailor_telem_snapshot(&s,212,&out);
    assert(out.protocol_fresh && out.current_protocol==254 && out.channel_state==255);
    assert(out.channel_fresh && out.channel_number==12580);
    sailor_telem_tick(&s,212); w.count=0;
    /* Every short/overlong form leaves accepted values and timestamps intact. */
    const uint16_t ops[]={0x4073,0x4054,0x4043}; const size_t exact[]={6,7,1};
    for(unsigned i=0;i<3;i++) for(size_t n=0;n<=8;n++) {
        if(n==exact[i]) continue;
        rx_application(&s,seq++,ops[i],data,n,300);
        sailor_telem_snapshot(&s,300,&out);
        assert(out.current_protocol==254 && out.channel_number==12580 && out.channel_state==255);
        assert(out.protocol_age_ms==90 && out.channel_age_ms==89 && out.channel_state_age_ms==88);
        sailor_telem_tick(&s,300); w.count=0;
    }
    chan[3]=0; chan[4]=0; rx_application(&s,seq++,0x4054,chan,7,400);
    sailor_telem_snapshot(&s,400,&out); assert(!out.channel_valid && !out.channel_fresh);
    chan[3]=255; chan[4]=255; rx_application(&s,seq++,0x4054,chan,7,401);
    sailor_telem_snapshot(&s,401,&out); assert(!out.channel_valid && !out.channel_fresh);
    sailor_telem_tick(&s,401); w.count=0;
    /* Millisecond rollover does not turn recent network data stale. */
    uint32_t start=UINT32_MAX-100;
    ready(&s,&w,start); data[0]=0; data[1]=0; data[2]=244;
    rx_application(&s,seq++,0x404c,data,5,start);
    sailor_telem_snapshot(&s,start+200u,&out);
    assert(out.registration_age_ms==200 && out.registration_fresh && out.ocean_fresh);
}
static void test_all_poll_groups_are_fair(void) {
    sailor_telem_t s; wire_t w; ready(&s,&w,100);
    const uint16_t expected[]={0x2004,0x2108,0x2050,0x204c,0x2073,0x2043,0x2054};
    unsigned seen=0;
    for(unsigned cycle=0;cycle<8;cycle++) {
        /* Worst case: all groups are due again each time two upper slots clear.
         * This used to indefinitely favor UTC and GPS in fixed index order. */
        for(unsigned i=0;i<SAILOR_TELEM_POLLS;i++) { s.polls[i].due=100; s.polls[i].pending=false; }
        w.count=0; sailor_telem_tick(&s,100+cycle);
        assert(w.count==2 && outstanding(&s)==2);
        for(unsigned j=0;j<2;j++) {
            const sent_t *p=&w.sent[j]; assert(p->len==14 && p->data[8]==2 && p->data[11]==3);
            uint16_t op=(uint16_t)((p->data[9]<<8)|p->data[10]);
            unsigned index=(cycle*2+j)%SAILOR_TELEM_POLLS;
            assert(op==expected[index]); seen|=1u<<index;
            sailor_telem_tx_complete(&s,p->cookie,true,100+cycle);
            rx_control(&s,sequence(p),6,100+cycle);
        }
    }
    assert(seen==(1u<<SAILOR_TELEM_POLLS)-1u);
}
int main(void) {
    assert(sailor_telem_crc((const uint8_t *)"123456789",9)==0x906e);
    test_wire_fixtures_and_validation(); test_coordinates_and_stale();
    test_upper_timer_and_retry(); test_ack_nak_window_and_late_events();
    test_backpressure_and_unsupported(); test_selected_discovery();
    test_network_fixtures_and_lifecycle(); test_network_bounds_and_unavailable();
    test_all_poll_groups_are_fair();
    puts("sailor_telemetry: PASS (managed NDP, upper ACK/NAK/ENQ, 2-slot retries, fixture CRC, signed GPS, network status, fair polls, stale, reconnect/backpressure)");
    return 0;
}
