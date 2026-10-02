/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
/* Host regression tests for service-discovered NDP sessions.
 * Synthetic device identities and serials use observed discovery layouts.
 * Profile flags and service tables are retained for protocol coverage.
 * No vendor executable data or device/network access is needed.
 */
#include "sailor_ndp_session.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t antenna_announce[] = {
    0x02, 0x03, 0x56, 0x34, 0xe2, 0x2b, 0x00, 0xaa, 0x8c, 0xc0, 0x02, 0x00,
    0x07, 0x00, 0x01, 0x08, 0x05, 0x00, 0x38, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x0b, 0x02, 0x00, 0x0a, 0x06, 0x00, 0x01,
};

static const uint8_t terminal_announce[] = {
    0x02, 0x01, 0x67, 0x45, 0xe3, 0x2b, 0x00, 0xa0, 0xa0, 0xc0, 0x01, 0x00,
    0x01, 0x00, 0x01, 0x09, 0x05, 0x00, 0x39, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x04, 0x00, 0x0f,
};


static const uint8_t local_name[8] = {1,2,3,4,5,6,7,8};

typedef struct {
    uint8_t peer, series, channel;
    uint16_t len;
    bool ack, accepted;
    uint32_t at;
    uint8_t data[512];
} tx_record_t;
typedef struct { uint8_t slot; uint32_t cookie; bool success; } completion_t;
typedef struct {
    sailor_ndp_session_t session;
    sailor_ndp_peer_t peer;
    uint32_t now;
    tx_record_t tx[160];
    unsigned tx_count, receive_calls[2], deliveries[2];
    uint8_t received[2][512];
    uint16_t received_len[2];
    bool reject_tx, reject_receive, enqueue_reply;
    sailor_ndp_session_state_t state[2];
    sailor_ndp_session_reason_t reason[2];
    completion_t completed[64];
    unsigned complete_count;
} fixture_t;

static bool transmit(void *user, uint8_t peer, uint8_t series, uint8_t channel,
                     const uint8_t *data, uint16_t len, bool ack)
{
    fixture_t *f = user;
    assert(f->tx_count < sizeof(f->tx)/sizeof(f->tx[0]));
    assert(len <= 512 && (data != NULL || len == 0));
    tx_record_t *r = &f->tx[f->tx_count++];
    *r = (tx_record_t){.peer=peer, .series=series, .channel=channel,
                      .len=len, .ack=ack, .accepted=!f->reject_tx, .at=f->now};
    if (len) memcpy(r->data, data, len);
    return r->accepted;
}
static bool receive(void *user, uint8_t slot, const uint8_t *data,
                    uint16_t len, uint32_t now)
{
    fixture_t *f = user;
    assert(slot < 2 && len <= 512 && (data != NULL || len == 0));
    ++f->receive_calls[slot];
    if (f->reject_receive) return false;
    ++f->deliveries[slot];
    f->received_len[slot] = len;
    if (len) memcpy(f->received[slot], data, len);
    if (f->enqueue_reply) {
        const uint8_t reply[] = {0x00,0xff,0x7e};
        assert(sailor_ndp_session_send(&f->session,slot,reply,sizeof(reply),900,now));
    }
    return true;
}
static void state_changed(void *user, uint8_t slot,
                          sailor_ndp_session_state_t state,
                          sailor_ndp_session_reason_t reason, uint32_t now)
{
    fixture_t *f = user;
    (void)now;
    assert(slot < 2);
    f->state[slot] = state;
    f->reason[slot] = reason;
}
static void complete(void *user, uint8_t slot, uint32_t cookie,
                     bool success, uint32_t now)
{
    fixture_t *f = user;
    (void)now;
    assert(f->complete_count < sizeof(f->completed)/sizeof(f->completed[0]));
    f->completed[f->complete_count++] = (completion_t){slot,cookie,success};
}
static void init(fixture_t *f)
{
    const sailor_ndp_session_callbacks_t cb = {
        .transmit=transmit, .receive=receive, .state=state_changed, .complete=complete
    };
    memset(f,0,sizeof(*f));
    assert(sailor_ndp_session_parse_discovery(1,antenna_announce,
                                            sizeof(antenna_announce),&f->peer));
    sailor_ndp_session_init(&f->session,local_name,0,&cb,f);
}
static uint8_t remote_port(const fixture_t *f, unsigned slot)
{ return slot ? f->peer.terminal_port : f->peer.binary_port; }
static uint8_t outgoing(const fixture_t *f, unsigned slot)
{ return (uint8_t)((remote_port(f,slot)<<4) | (slot ? 3 : 1)); }
static uint8_t incoming(const fixture_t *f, unsigned slot)
{ return (uint8_t)(((slot ? 3 : 1)<<4) | remote_port(f,slot)); }
static void tick(fixture_t *f, uint32_t now)
{ f->now=now; sailor_ndp_session_tick(&f->session,now); }
static void feed(fixture_t *f, uint8_t sa, uint8_t da, uint8_t series,
                 uint8_t channel, const uint8_t *data, size_t len, uint32_t now)
{
    f->now=now;
    sailor_ndp_session_rx(&f->session,sa,da,series,channel,data,len,now);
}
static void names(const fixture_t *f, uint8_t out[16])
{ memcpy(out,f->peer.name,8); memcpy(out+8,local_name,8); }
static void accept(fixture_t *f, unsigned slot, uint32_t now)
{
    uint8_t n[16]; names(f,n);
    feed(f,f->peer.sa,f->session.local_sa,0x11,incoming(f,slot),n,sizeof(n),now);
    assert(sailor_ndp_session_is_up(&f->session,(uint8_t)slot));
}
static void ack(fixture_t *f, unsigned slot, uint8_t seq, uint32_t now)
{ feed(f,f->peer.sa,f->session.local_sa,seq,incoming(f,slot),NULL,0,now); }
static void data(fixture_t *f, unsigned slot, uint8_t seq,
                 const uint8_t *p, size_t n, uint32_t now)
{ feed(f,f->peer.sa,f->session.local_sa,(uint8_t)(8|seq),incoming(f,slot),p,n,now); }
static void bind_both(fixture_t *f, uint32_t now)
{
    for (unsigned i=0;i<2;++i)
        assert(sailor_ndp_session_bind(&f->session,(uint8_t)i,&f->peer,remote_port(f,i),now));
}
static void ready(fixture_t *f)
{
    init(f); bind_both(f,1); tick(f,1);
    assert(f->tx_count==2);
    for (unsigned i=0;i<2;++i) {
        assert(f->tx[i].series==0x10 && f->tx[i].len==16 && !f->tx[i].ack);
        assert(f->tx[i].channel==outgoing(f,i) && f->tx[i].peer==1);
        assert(!memcmp(f->tx[i].data,local_name,8));
        assert(!memcmp(f->tx[i].data+8,f->peer.name,8));
        accept(f,i,2);
    }
    f->tx_count=0;
}
static unsigned records(const fixture_t *f, uint8_t channel, unsigned kind)
{
    unsigned n=0;
    for (unsigned i=0;i<f->tx_count;++i) {
        const tx_record_t *r=&f->tx[i];
        if (r->channel!=channel) continue;
        if (kind==0 ? r->ack : kind==1 ? r->series>=8 && r->series<16 : r->series==kind) ++n;
    }
    return n;
}
static const tx_record_t *last_data(const fixture_t *f, uint8_t channel)
{
    for (unsigned i=f->tx_count;i>0;--i) {
        const tx_record_t *r=&f->tx[i-1];
        if (r->channel==channel && !r->ack && r->series>=8 && r->series<16) return r;
    }
    assert(!"missing transmitted DATA");
    return NULL;
}
static unsigned completions(const fixture_t *f, uint32_t cookie, bool success)
{
    unsigned n=0;
    for (unsigned i=0;i<f->complete_count;++i)
        if (f->completed[i].cookie==cookie && f->completed[i].success==success) ++n;
    return n;
}
static void invalid_discovery(uint8_t sa, const uint8_t *bytes, size_t n)
{
    sailor_ndp_peer_t out, zero={0};
    memset(&out,0xa5,sizeof(out));
    assert(!sailor_ndp_session_parse_discovery(sa,bytes,n,&out));
    assert(!memcmp(&out,&zero,sizeof(out)));
}
static void test_discovery(void)
{
    sailor_ndp_peer_t p;
    assert(sailor_ndp_session_parse_discovery(1,antenna_announce,sizeof(antenna_announce),&p));
    const uint8_t fixture_name[]={0x56,0x34,0xe2,0x2b,0x00,0xaa,0x8c,0xc0};
    assert(p.sa==1 && !memcmp(p.name,fixture_name,8));
    assert(p.device_type==2 && p.device_subtype==7 && p.service_count==3);
    assert(p.version_major==1 && p.version_minor==8 && p.build==5);
    assert(p.serial_valid && !strcmp(p.serial,"80000002"));
    assert(p.binary_service && p.binary_port==11 && p.terminal_service && p.terminal_port==10);
    /* Count03 is not a fixed "antenna type03" tag; SA00 is also legal. */
    uint8_t a[sizeof(antenna_announce)]; memcpy(a,antenna_announce,sizeof(a));
    a[1]=1; a[52]=9;
    assert(sailor_ndp_session_parse_discovery(0,a,53,&p));
    assert(p.sa==0 && p.binary_service && p.binary_port==9 && !p.terminal_service);
    memcpy(a,antenna_announce,sizeof(a)); a[52]=9; a[55]=12;
    assert(sailor_ndp_session_parse_discovery(1,a,sizeof(a),&p));
    assert(p.binary_port==9 && p.terminal_port==12);
    invalid_discovery(0,terminal_announce,sizeof(terminal_announce));
    invalid_discovery(254,a,sizeof(a)); invalid_discovery(255,a,sizeof(a));
    invalid_discovery(1,a,sizeof(a)-1); invalid_discovery(1,a,49);
    a[1]=17; invalid_discovery(1,a,sizeof(a));
    memcpy(a,antenna_announce,sizeof(a)); a[52]=16; invalid_discovery(1,a,sizeof(a));
    memcpy(a,antenna_announce,sizeof(a)); a[0]=3; invalid_discovery(1,a,sizeof(a));
    memcpy(a,antenna_announce,sizeof(a)); memset(a+2,0,8); invalid_discovery(1,a,sizeof(a));
    memset(a+2,255,8); invalid_discovery(1,a,sizeof(a));
    memcpy(a,antenna_announce,sizeof(a)); memcpy(a+56,a+50,3); invalid_discovery(1,a,sizeof(a));
    memcpy(a,antenna_announce,sizeof(a)); a[18]='x';
    assert(sailor_ndp_session_parse_discovery(1,a,sizeof(a),&p) && !p.serial_valid);
    memset(a+18,'1',20); memset(a+38,0,12);
    assert(sailor_ndp_session_parse_discovery(1,a,sizeof(a),&p) && !p.serial_valid);
    puts("  discovery: synthetic announce, service count, dynamic ports, malformed bounds");
}
static void test_control_validation(void)
{
    fixture_t f; init(&f);
    f.peer.binary_port=9; f.peer.terminal_port=12;
    assert(!sailor_ndp_session_bind(&f.session,0,&f.peer,11,1));
    bind_both(&f,1);
    const uint8_t byte=42;
    assert(!sailor_ndp_session_send(&f.session,0,&byte,1,1,1));
    tick(&f,1);
    assert(f.tx[0].channel==0x91 && f.tx[1].channel==0xc3);
    uint8_t n[17]; names(&f,n); n[16]=0;
    feed(&f,2,0,0x11,0x19,n,16,2); /* wrong source */
    feed(&f,1,2,0x11,0x19,n,16,2); /* wrong destination */
    feed(&f,1,255,0x11,0x19,n,16,2); /* broadcast cannot accept connection */
    feed(&f,1,0,0x11,0x91,n,16,2); /* wrong endpoint direction */
    feed(&f,1,0,0x11,0x19,n,15,2);
    feed(&f,1,0,0x11,0x19,n,17,2);
    n[0]^=1; feed(&f,1,0,0x11,0x19,n,16,2); n[0]^=1;
    n[8]^=1; feed(&f,1,0,0x11,0x19,n,16,2); n[8]^=1;
    assert(!sailor_ndp_session_is_up(&f.session,0));
    accept(&f,0,3); accept(&f,1,3);
    assert(sailor_ndp_session_bind(&f.session,0,&f.peer,9,4)); /* idempotent */
    unsigned sends=f.tx_count; tick(&f,4); assert(f.tx_count==sends);
    assert(sailor_ndp_session_send(&f.session,0,&byte,1,55,5)); tick(&f,5);
    n[0]^=1; feed(&f,1,0,0x12,0x19,n,16,6); n[0]^=1;
    assert(sailor_ndp_session_is_up(&f.session,0));
    feed(&f,1,0,0x12,0x19,n,16,6);
    assert(!sailor_ndp_session_is_up(&f.session,0));
    assert(sailor_ndp_session_is_up(&f.session,1));
    assert(completions(&f,55,false)==1 && completions(&f,55,true)==0);
    puts("  control: NAME pair, length, SA/DA, direction, independent abort");
}
static void test_retries_and_late_ack(void)
{
    fixture_t f; ready(&f);
    uint8_t bytes[]={0,0xff,0x7e,0x5f,0x99,0x02};
    assert(sailor_ndp_session_send(&f.session,0,bytes,sizeof(bytes),101,100));
    ack(&f,0,0,100); assert(f.complete_count==0); /* cannot ACK unsent queue */
    tick(&f,100);
    tx_record_t original=*last_data(&f,0xb1);
    memset(bytes,0xaa,sizeof(bytes)); /* caller buffer lifetime ends at send */
    tick(&f,1449); assert(records(&f,0xb1,1)==1);
    tick(&f,1450); assert(records(&f,0xb1,1)==2);
    tick(&f,2799); assert(records(&f,0xb1,1)==2);
    tick(&f,2800); assert(records(&f,0xb1,1)==3);
    for (unsigned i=0;i<f.tx_count;++i) {
        const tx_record_t *r=&f.tx[i];
        if (r->series<8 || r->series>=16) continue;
        assert(r->series==original.series && r->channel==original.channel && r->peer==original.peer);
        assert(r->len==original.len && !memcmp(r->data,original.data,r->len));
    }
    tick(&f,4149); assert(sailor_ndp_session_is_up(&f.session,0));
    tick(&f,4150);
    assert(records(&f,0xb1,1)==3 && !sailor_ndp_session_is_up(&f.session,0));
    assert(completions(&f,101,false)==1 && f.complete_count==1);
    assert(sailor_ndp_session_is_up(&f.session,1));
    unsigned opens=records(&f,0xb1,0x10);
    tick(&f,5149); assert(records(&f,0xb1,0x10)==opens);
    tick(&f,5150); assert(records(&f,0xb1,0x10)==opens+1);
    accept(&f,0,5151); tick(&f,5152); assert(f.complete_count==1);

    ready(&f);
    assert(sailor_ndp_session_send(&f.session,0,original.data,original.len,102,100));
    tick(&f,100); f.reject_tx=true; tick(&f,1450);
    assert(!last_data(&f,0xb1)->accepted);
    ack(&f,0,0,1451);
    assert(completions(&f,102,true)==1 && f.complete_count==1);
    ack(&f,0,0,1452); assert(f.complete_count==1);
    /* Never-accepted transmissions have no ACK eligibility, but remain bounded. */
    ready(&f); f.reject_tx=true;
    assert(sailor_ndp_session_send(&f.session,0,NULL,0,103,100)); tick(&f,100);
    ack(&f,0,0,101); assert(f.complete_count==0);
    tick(&f,1450); tick(&f,2800); tick(&f,4150);
    assert(records(&f,0xb1,1)==3 && completions(&f,103,false)==1);
    puts("  retries: exact bytes/series, initial+2, late ACK, lower-send failure bound");
}
static void test_sequence_and_duplicates(void)
{
    fixture_t f; ready(&f);
    for (unsigned i=0;i<18;++i) {
        uint8_t payload[2]={(uint8_t)i,0};
        uint32_t now=100+10*i;
        assert(sailor_ndp_session_send(&f.session,0,payload,sizeof(payload),200+i,now));
        tick(&f,now);
        assert(last_data(&f,0xb1)->series==(uint8_t)(8|(i&7)));
        ack(&f,0,(uint8_t)(i&7),now+2);
        assert(completions(&f,200+i,true)==1);
        unsigned before=records(&f,0xb1,0);
        data(&f,0,(uint8_t)(i&7),payload,sizeof(payload),now+3);
        assert(f.deliveries[0]==i+1 && f.received[0][0]==i);
        data(&f,0,(uint8_t)(i&7),payload,sizeof(payload),now+4);
        assert(f.deliveries[0]==i+1 && f.receive_calls[0]==i+1);
        assert(records(&f,0xb1,0)==before+2);
        assert(f.tx[f.tx_count-1].series==(i&7));
    }
    assert(sailor_ndp_session_is_up(&f.session,1) && f.deliveries[1]==0);
    const uint8_t byte=42;
    assert(sailor_ndp_session_send(&f.session,1,&byte,1,299,400)); tick(&f,400);
    assert(last_data(&f,0xa3)->series==8); /* terminal sequence is independent */
    ready(&f);
    assert(sailor_ndp_session_send(&f.session,0,&byte,1,298,100)); tick(&f,100);
    ack(&f,0,1,101); /* wrong ACK cannot complete/advance the outstanding message */
    assert(completions(&f,298,true)==0 && completions(&f,298,false)==1);
    assert(!sailor_ndp_session_is_up(&f.session,0));
    assert(f.reason[0]==SAILOR_NDP_REASON_BAD_SEQUENCE);
    assert(sailor_ndp_session_is_up(&f.session,1));
    ready(&f);
    data(&f,0,1,&byte,1,100); /* fresh session requires seq0 */
    assert(f.deliveries[0]==0 && !sailor_ndp_session_is_up(&f.session,0));
    assert(f.reason[0]==SAILOR_NDP_REASON_BAD_SEQUENCE);
    ready(&f);
    data(&f,0,0,&byte,1,100);
    const uint8_t changed=43;
    data(&f,0,0,&changed,1,101); /* same seq with different content is not a replay */
    assert(f.deliveries[0]==1 && !sailor_ndp_session_is_up(&f.session,0));
    assert(sailor_ndp_session_is_up(&f.session,1));
    puts("  sequence: TX/RX wrap7->0, wrong ACK, duplicate re-ACK, bad sequence isolation");
}
static void test_backpressure_and_queue(void)
{
    fixture_t f; ready(&f);
    uint8_t payload[513];
    for (unsigned i=0;i<sizeof(payload);++i) payload[i]=(uint8_t)i;
    f.reject_receive=true;
    data(&f,0,0,payload,512,100);
    assert(f.receive_calls[0]==1 && f.deliveries[0]==0 && records(&f,0xb1,0)==0);
    f.reject_receive=false;
    data(&f,0,0,payload,512,101);
    assert(f.deliveries[0]==1 && f.received_len[0]==512 && !memcmp(f.received[0],payload,512));
    data(&f,0,0,payload,512,102);
    assert(f.receive_calls[0]==2 && f.deliveries[0]==1 && records(&f,0xb1,0)==2);
    data(&f,0,1,payload,513,103); assert(f.deliveries[0]==1);

    ready(&f);
    assert(!sailor_ndp_session_send(&f.session,0,payload,513,399,100));
    for (unsigned i=0;i<4;++i) {
        memset(payload,(int)i,512);
        assert(sailor_ndp_session_send(&f.session,0,payload,512,400+i,100));
    }
    assert(!sailor_ndp_session_send(&f.session,0,payload,512,404,100));
    assert(sailor_ndp_session_send(&f.session,1,NULL,0,405,100));
    memset(payload,0xff,512); tick(&f,100);
    assert(last_data(&f,0xb1)->len==512 && last_data(&f,0xb1)->data[511]==0);
    assert(last_data(&f,0xa3)->len==0);
    ack(&f,1,0,101); assert(completions(&f,405,true)==1);
    for (unsigned i=0;i<4;++i) {
        if (i) tick(&f,110+10*i);
        const tx_record_t *r=last_data(&f,0xb1);
        assert(r->len==512 && r->series==(8|i));
        for (unsigned j=0;j<512;++j) assert(r->data[j]==i);
        ack(&f,0,(uint8_t)i,111+10*i);
        assert(completions(&f,400+i,true)==1);
    }
    assert(completions(&f,404,false)==0 && completions(&f,399,false)==0);
    assert(sailor_ndp_session_send(&f.session,0,payload,512,406,200));
    assert(sailor_ndp_session_send(&f.session,0,payload,512,407,200));
    sailor_ndp_session_unbind(&f.session,0,201);
    assert(completions(&f,406,false)==1 && completions(&f,407,false)==1);
    sailor_ndp_session_unbind(&f.session,0,202);
    assert(completions(&f,406,false)==1 && sailor_ndp_session_is_up(&f.session,1));
    /* receive callback may enqueue an application response without corrupting ACK state. */
    ready(&f); f.enqueue_reply=true;
    data(&f,1,0,payload,3,100); tick(&f,101);
    const tx_record_t *r=last_data(&f,0xa3);
    assert(r->len==3 && r->data[0]==0 && r->data[1]==255 && r->data[2]==0x7e);
    ack(&f,1,0,102); assert(completions(&f,900,true)==1);
    puts("  bounds: receive backpressure,512-byte copy, queue4/full, flush completion, callback enqueue");
}
static void test_identity_rebinding(void)
{
    fixture_t f; ready(&f);
    sailor_ndp_peer_t other=f.peer;
    other.sa=2;
    assert(!sailor_ndp_session_bind(&f.session,0,&other,other.binary_port,100));
    other=f.peer; other.name[0]^=1;
    assert(!sailor_ndp_session_bind(&f.session,0,&other,other.binary_port,100));
    assert(!sailor_ndp_session_peer_claim(&f.session,other.name,3,100));
    assert(!sailor_ndp_session_peer_claim(&f.session,local_name,3,100));
    assert(!sailor_ndp_session_peer_claim(&f.session,f.peer.name,0,100));
    assert(!sailor_ndp_session_peer_claim(&f.session,f.peer.name,254,100));
    assert(sailor_ndp_session_is_up(&f.session,0) && sailor_ndp_session_is_up(&f.session,1));
    const uint8_t p[]={1,2};
    assert(sailor_ndp_session_send(&f.session,0,p,2,501,100));
    assert(sailor_ndp_session_send(&f.session,1,p,2,502,100)); tick(&f,100);
    assert(sailor_ndp_session_peer_claim(&f.session,f.peer.name,2,101));
    assert(!sailor_ndp_session_is_up(&f.session,0) && !sailor_ndp_session_is_up(&f.session,1));
    assert(completions(&f,501,false)==1 && completions(&f,502,false)==1);
    f.peer.sa=2; f.tx_count=0; tick(&f,101);
    assert(f.tx_count==2 && f.tx[0].peer==2 && f.tx[1].peer==2);
    uint8_t n[16]; names(&f,n);
    feed(&f,1,0,0x11,incoming(&f,0),n,16,102); /* stale SA cannot reopen */
    assert(!sailor_ndp_session_is_up(&f.session,0));
    accept(&f,0,102); accept(&f,1,102);
    assert(sailor_ndp_session_peer_claim(&f.session,f.peer.name,2,103));
    assert(sailor_ndp_session_is_up(&f.session,0));
    /* Another NAME at the bound SA invalidates that route, never adopts it. */
    other=f.peer; other.name[0]^=1;
    assert(!sailor_ndp_session_peer_claim(&f.session,other.name,2,104));
    assert(!sailor_ndp_session_is_up(&f.session,0) && !sailor_ndp_session_is_up(&f.session,1));
    unsigned before=f.tx_count; tick(&f,2104); assert(f.tx_count==before);
    assert(sailor_ndp_session_peer_claim(&f.session,f.peer.name,4,2105));
    f.peer.sa=4; tick(&f,2105); accept(&f,0,2106); accept(&f,1,2106);
    /* Local address movement also invalidates old destination and in-flight data. */
    assert(sailor_ndp_session_send(&f.session,0,p,2,503,2200));
    sailor_ndp_session_set_local_sa(&f.session,7,2201);
    assert(completions(&f,503,false)==1);
    tick(&f,2201);
    feed(&f,4,0,0x11,incoming(&f,0),n,16,2202);
    assert(!sailor_ndp_session_is_up(&f.session,0));
    accept(&f,0,2202); accept(&f,1,2202);
    for (unsigned i=0;i<2;++i) sailor_ndp_session_unbind(&f.session,(uint8_t)i,2300);
    other=f.peer; other.name[0]^=1;
    assert(sailor_ndp_session_bind(&f.session,0,&other,other.binary_port,2301));
    puts("  identity: stable NAME pin, swapped/rebound SA, foreign claim, local SA movement");
}

static void test_open_and_peer_restart(void)
{
    fixture_t f; init(&f);
    assert(sailor_ndp_session_bind(&f.session,0,&f.peer,11,100));
    uint8_t n[16]; names(&f,n);
    feed(&f,1,0,0x11,0x1b,n,16,100);
    assert(!sailor_ndp_session_is_up(&f.session,0)); /* no successful OPEN yet */
    tick(&f,100); assert(records(&f,0xb1,0x10)==1);
    tick(&f,1449); assert(records(&f,0xb1,0x12)==0);
    tick(&f,1450); assert(records(&f,0xb1,0x12)==1);
    assert(f.state[0]==SAILOR_NDP_SESSION_RETRY_WAIT);
    tick(&f,2449); assert(records(&f,0xb1,0x10)==1);
    tick(&f,2450); assert(records(&f,0xb1,0x10)==2);
    accept(&f,0,2451);

    ready(&f);
    const uint8_t byte=42;
    data(&f,0,0,&byte,1,100);
    assert(sailor_ndp_session_send(&f.session,0,&byte,1,701,101)); tick(&f,101);
    names(&f,n);
    feed(&f,1,0,0x10,0x1b,n,16,102); /* same NAME, fresh connection epoch */
    assert(sailor_ndp_session_is_up(&f.session,0));
    assert(completions(&f,701,false)==1 && records(&f,0xb1,0x11)==1);
    data(&f,0,0,&byte,1,103);
    assert(f.deliveries[0]==2); /* same bytes now belong to the new epoch */
    assert(sailor_ndp_session_is_up(&f.session,1));
    n[0]^=1;
    feed(&f,2,0,0x10,0x19,n,16,104); /* unknown peer may be rejected, never adopted */
    assert(f.tx[f.tx_count-1].series==0x12 && f.tx[f.tx_count-1].peer==2);
    assert(f.tx[f.tx_count-1].channel==0x91);
    assert(sailor_ndp_session_is_up(&f.session,0));
    unsigned sent=f.tx_count; n[8]^=1;
    feed(&f,2,0,0x10,0x19,n,16,105); assert(f.tx_count==sent);
    puts("  open: qualified acceptance, timeout/reopen, peer restart, unknown peer rejection");
}

static void test_wrapping_clock(void)
{
    fixture_t f; ready(&f);
    const uint8_t byte=1;
    uint32_t start=UINT32_MAX-500u;
    assert(sailor_ndp_session_send(&f.session,0,&byte,1,601,start)); tick(&f,start);
    tick(&f,start+1349u); assert(records(&f,0xb1,1)==1);
    tick(&f,start+1350u); assert(records(&f,0xb1,1)==2);
    ack(&f,0,0,start+1351u); assert(completions(&f,601,true)==1);
    puts("  timing: retry deadlines across uint32_t rollover");
}
int main(void)
{
    test_discovery();
    test_control_validation();
    test_retries_and_late_ack();
    test_sequence_and_duplicates();
    test_backpressure_and_queue();
    test_identity_rebinding();
    test_open_and_peer_restart();
    test_wrapping_clock();
    puts("sailor_ndp_session: PASS");
    return 0;
}
