/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
/* Exercises the real profile + NDP + binary client, with deterministic fake
 * clock/L2/transport. No hardware, tasks or socket I/O. The fake nonrecursive
 * mutex also catches synchronous transport delivery under the profile lock. */
#include "fsm.h"
#include "esp_mac.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    sp_id_fields_t id;
    uint8_t signature, series, channel, data[512];
    uint16_t len;
    bool ack, raw;
} sent_t;
static sent_t sent[512];
static unsigned sent_count, transport_reads, accepted_count, receive_calls;
static uint32_t milliseconds;
static bool l2_alive, tr_alive, accept_terminal;
static unsigned teardown_order;
static sp_l2_rx_cb_t l2_receiver;
static sp_tr_app_rx_cb_t app_receiver;
static void *l2_user, *app_user;
static const sp_app_block_t *pending_app;
static profile_test_mutex_t mutex;
static uint8_t local_name[8];
/* Synthetic NAME identity fields; protocol profile bits remain unchanged. */
static const uint8_t antenna_name[8]={0x56,0x34,0xe2,0x2b,0x00,0xaa,0x8c,0xc0};
static const uint8_t other_name[8]={0x68,0x45,0xe3,0x2b,0x00,0xaa,0x8c,0xc0};

void profile_test_log(const char *tag,const char *format,...) { (void)tag; (void)format; }
int64_t esp_timer_get_time(void) { return (int64_t)milliseconds*1000; }
int esp_read_mac(uint8_t *mac,int type) {
    const uint8_t fixed[6]={2,0x15,0x5d,0x12,0x27,0x95};
    assert(type==ESP_MAC_WIFI_STA); memcpy(mac,fixed,6); return ESP_OK;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex; }
int xSemaphoreTake(SemaphoreHandle_t m,uint32_t timeout) {
    (void)timeout; assert(m && !m->locked); m->locked=1; return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t m) { assert(m && m->locked); m->locked=0; return pdTRUE; }
void profile_test_critical_enter(portMUX_TYPE *m) { assert(!*m); *m=1; }
void profile_test_critical_exit(portMUX_TYPE *m) { assert(*m); *m=0; }

sp_err_t sp_l2_init(const sp_l2_config_t *cfg,sp_l2_rx_cb_t cb,sp_l2_evt_cb_t evt,void *user) {
    (void)evt; assert(cfg && cb && !l2_alive); l2_receiver=cb; l2_user=user; l2_alive=true; return SP_OK;
}
bool sp_l2_deinit(void) { assert(l2_alive); l2_alive=false; teardown_order=1; return true; }
void sp_tr_deinit(void) { assert(tr_alive && teardown_order==1); tr_alive=false; teardown_order=2; }
sp_err_t sp_tr_init(const sp_timing_t *timing,uint16_t slots,sp_tr_app_rx_cb_t cb,sp_tr_evt_cb_t evt,void *user) {
    (void)timing; (void)evt; assert(slots && cb && !tr_alive); app_receiver=cb; app_user=user; tr_alive=true; return SP_OK;
}
static sent_t *reserve(const sp_id_fields_t *id) {
    assert(sent_count<sizeof(sent)/sizeof(sent[0]));
    sent_t *s=&sent[sent_count++]; memset(s,0,sizeof(*s)); s->id=*id; return s;
}
sp_err_t sp_l2_send(const sp_id_fields_t *id,const uint8_t *data,uint8_t n) {
    assert(l2_alive && n<=8); sent_t *s=reserve(id); s->raw=true; s->len=n;
    if(n) memcpy(s->data,data,n);
    return SP_OK;
}
sp_err_t sp_tr_send_app(const sp_id_fields_t *id,const sp_app_block_t *b,uint8_t *sid) {
    (void)sid; assert(tr_alive && l2_alive && b->app_len<=512);
    sent_t *s=reserve(id); s->signature=b->sig[2]; s->series=b->series;
    s->channel=(uint8_t)b->channel_le; s->len=b->app_len;
    if(s->len) memcpy(s->data,b->app_data,s->len);
    return SP_OK;
}
sp_err_t sp_tr_send_short_ack(const sp_id_fields_t *id,uint8_t seq,uint16_t channel) {
    assert(tr_alive && l2_alive); sent_t *s=reserve(id); s->ack=true;
    s->signature=2; s->series=seq; s->channel=(uint8_t)channel; return SP_OK;
}
void sp_tr_on_l2_frame(const sp_l2_frame_t *f) {
    assert(tr_alive && !mutex.locked); ++transport_reads;
    if(pending_app) app_receiver(&f->id,sp_pgn_of(f->id.dp,f->id.pf),0,pending_app,app_user);
}
static bool terminal_receive(const uint8_t *p,uint16_t n,void *user) {
    (void)user; assert(p && n); ++receive_calls;
    if(accept_terminal) ++accepted_count;
    return accept_terminal;
}
static void tick(uint32_t now) { milliseconds=now; sp_fsm_tick(); }
static unsigned count_wire(uint8_t series,uint8_t channel) {
    unsigned count=0; for(unsigned i=0;i<sent_count;++i)
        if(!sent[i].raw && sent[i].signature==2 && sent[i].series==series && sent[i].channel==channel) ++count;
    return count;
}
static unsigned count_raw(uint8_t pf,uint8_t destination) {
    unsigned count=0; for(unsigned i=0;i<sent_count;++i)
        if(sent[i].raw && sent[i].id.pf==pf && sent[i].id.ps==destination) ++count;
    return count;
}
static void reset_capture(void) { sent_count=0; }
static void start(void) {
    assert(!l2_alive && !tr_alive && !mutex.locked);
    reset_capture(); milliseconds=0; teardown_order=0; transport_reads=0;
    accepted_count=receive_calls=0; accept_terminal=true;
    sp_fsm_config_t cfg={.local_sa=0}; sp_l2_config_t l2={.local_sa=0};
    assert(sp_fsm_init(&cfg)==SP_OK);
    assert(sp_fsm_bind_layers(&l2,NULL,4,NULL,NULL,NULL)==SP_OK);
    sp_fsm_set_term_rx(terminal_receive,NULL);
}
static void stop(void) {
    sp_fsm_deinit(); assert(sp_fsm_unbind_layers()); assert(teardown_order==2);
}
static void claim_ready(void) {
    tick(0); assert(sent_count==1 && sent[0].raw && sent[0].id.pf==0xee);
    memcpy(local_name,sent[0].data,8); tick(300);
    assert(sp_fsm_get_state()==SP_ST_WAIT_POLL); reset_capture();
}
static void raw_claim(uint8_t sa,const uint8_t name[8]) {
    sp_l2_frame_t f={.id={.pri=6,.dp=0,.pf=0xee,.ps=0xff,.sa=sa},.dlc=8};
    memcpy(f.data,name,8); l2_receiver(&f,l2_user);
}
static void app(uint8_t sa,uint8_t da,uint8_t type,uint8_t series,uint8_t channel,const uint8_t *p,uint16_t n) {
    sp_app_block_t b={.sig={0x5f,0x99,type},.series=series,.channel_le=channel,.app_data=p,.app_len=n};
    sp_l2_frame_t f={.id={.pri=6,.dp=1,.pf=0xef,.ps=da,.sa=sa},.dlc=8};
    assert(!pending_app); pending_app=&b; l2_receiver(&f,l2_user); pending_app=NULL;
}
static void announce(uint8_t sa,const uint8_t name[8],uint8_t binary,uint8_t terminal) {
    uint8_t p[59]={2,3}; memcpy(p+2,name,8); p[10]=2; p[12]=1;
    p[14]=1; p[15]=8; p[16]=5; memcpy(p+18,"80000002",8);
    p[50]=1; p[52]=binary; p[53]=2; p[55]=terminal; p[56]=6; p[58]=1;
    app(sa,0xff,3,8,0,p,sizeof(p));
}
static void accepted(uint8_t sa,uint8_t local,uint8_t remote,uint8_t da) {
    uint8_t names[16]; memcpy(names,antenna_name,8); memcpy(names+8,local_name,8);
    app(sa,da,2,0x11,(uint8_t)((local<<4)|remote),names,sizeof(names));
}
static void connected(uint8_t sa,uint8_t binary,uint8_t terminal) {
    announce(sa,antenna_name,binary,terminal); tick(301);
    assert(count_wire(0x10,(uint8_t)((binary<<4)|1))==1);
    assert(count_wire(0x10,(uint8_t)((terminal<<4)|3))==1);
    accepted(sa,1,binary,0); accepted(sa,3,terminal,0);
    assert(sp_fsm_get_state()==SP_ST_ONLINE && sp_fsm_term_input_can_send());
    reset_capture();
}

/* Inject a complete CRC-valid binary application through the real profile
 * and NDP receive path, including independent lower/upper sequence numbers. */
static void binary_response(uint8_t lower_seq,uint16_t upper_seq,uint16_t op,
                            const uint8_t *data,size_t n,uint32_t now) {
    uint8_t p[64]={2,0,0,1,0x10,0x20,0,0,2};
    assert(n<=sizeof(p)-14);
    p[1]=(uint8_t)(upper_seq>>8); p[2]=(uint8_t)upper_seq;
    uint16_t crc=sailor_telem_crc(p,6); p[6]=(uint8_t)crc; p[7]=(uint8_t)(crc>>8);
    p[9]=(uint8_t)(op>>8); p[10]=(uint8_t)op;
    memcpy(p+11,data,n); p[11+n]=3;
    crc=sailor_telem_crc(p,12+n); p[12+n]=(uint8_t)crc; p[13+n]=(uint8_t)(crc>>8);
    milliseconds=now; app(7,0,2,(uint8_t)(8+(lower_seq&7)),0x1b,p,(uint16_t)(14+n));
}

static void test_cold_claim_and_collision(void) {
    start(); tick(0); assert(sent_count==1); memcpy(local_name,sent[0].data,8);
    tick(299); assert(sent_count==1); assert(!sp_fsm_term_input_can_send());
    tick(300); assert(sent_count==3); assert(sent[1].signature==3 && sent[1].len==1);
    assert(sent[2].signature==3 && sent[2].len==50 && sent[2].data[1]==0);
    assert(!memcmp(sent[2].data+2,local_name,8));
    uint8_t first_name[8]; memcpy(first_name,local_name,8); stop();
    start(); tick(0); assert(!memcmp(first_name,sent[0].data,8));
    const uint8_t lower[8]={1}; milliseconds=100; reset_capture(); raw_claim(0,lower);
    assert(sent_count==1 && sent[0].raw && sent[0].id.sa==1 && sent[0].id.pf==0xee);
    assert(sp_fsm_get_state()==SP_ST_CLAIM); tick(399); assert(sent_count==1);
    tick(400); assert(sp_fsm_get_state()==SP_ST_WAIT_POLL && sent_count==3);
    uint8_t greater[8]; memset(greater,0xff,8); reset_capture(); raw_claim(1,greater);
    assert(sent_count==1 && sent[0].id.sa==1 && sp_fsm_get_state()==SP_ST_WAIT_POLL);
    stop();
}
static void test_discovery_and_dynamic_ports(void) {
    start(); claim_ready(); connected(7,12,13);
    protocol_antenna_status_t snapshot; sp_fsm_get_antenna_status(&snapshot);
    assert(snapshot.identity_valid && !strcmp(snapshot.serial,"80000002"));
    const uint8_t input[]="help\n";
    assert(sp_fsm_term_send(input,sizeof(input)-1)==SP_OK); tick(302);
    assert(count_wire(8,0xd3)==1); assert(!count_wire(8,0xa0));
    reset_capture(); announce(9,other_name,11,10); tick(303);
    assert(sp_fsm_get_peer_sa()==7 && !count_wire(0x10,0xa3));
    /* A validated service update reopens only the changed terminal endpoint. */
    reset_capture(); announce(7,antenna_name,12,14); tick(304);
    assert(count_wire(0x12,0xd3)==1 && count_wire(0x10,0xe3)==1);
    assert(!count_wire(0x10,0xc1)); assert(!sp_fsm_term_input_can_send());
    accepted(7,3,14,0); assert(sp_fsm_term_input_can_send()); stop();
}
static void test_backpressure_and_routing(void) {
    start(); claim_ready(); connected(7,11,10); accept_terminal=false;
    const uint8_t text[]="can0:/$ "; unsigned reads=transport_reads;
    app(7,9,2,8,0x3a,text,sizeof(text)-1); assert(transport_reads==reads);
    app(9,0,2,8,0x3a,text,sizeof(text)-1); assert(receive_calls==0);
    app(7,0,2,8,0x3a,text,sizeof(text)-1);
    assert(receive_calls==1 && accepted_count==0 && !count_wire(0,0xa3));
    accept_terminal=true; app(7,0,2,8,0x3a,text,sizeof(text)-1);
    assert(receive_calls==2 && accepted_count==1 && count_wire(0,0xa3)==1);
    app(7,0,2,8,0x3a,text,sizeof(text)-1);
    assert(receive_calls==2 && accepted_count==1 && count_wire(0,0xa3)==2);
    stop();
}
static void test_ttl_requires_address_claim(void) {
    start(); claim_ready(); connected(7,11,10);
    /* A tick uses the current locked clock without expiring a fresh peer. */
    milliseconds=302; sp_fsm_tick(); assert(sp_fsm_get_peer_sa()==7);
    tick(15301); assert(sp_fsm_get_peer_sa()==0xff && !sp_fsm_term_input_can_send());
    reset_capture(); milliseconds=16000; announce(9,antenna_name,11,10); tick(16000);
    assert(count_raw(0xea,9)==1 && !count_wire(0x10,0xb1) && !count_wire(0x10,0xa3));
    assert(sent[0].len==3 && sent[0].data[0]==0 && sent[0].data[1]==0xee && sent[0].data[2]==0);
    raw_claim(9,other_name); tick(16001); assert(sp_fsm_get_peer_sa()==0xff);
    raw_claim(9,antenna_name); tick(16002); assert(sp_fsm_get_peer_sa()==9);
    assert(count_wire(0x10,0xb1)==1 && count_wire(0x10,0xa3)==1);
    accepted(9,1,11,0); accepted(9,3,10,0); assert(sp_fsm_term_input_can_send());
    stop();
}
static void test_foreign_claim_and_owner_revoke(void) {
    start(); claim_ready(); connected(7,11,10);
    const uint8_t old[]="old-owner\n"; assert(sp_fsm_term_send(old,sizeof(old)-1)==SP_OK);
    sp_fsm_term_cancel_pending(); assert(!sp_fsm_term_input_can_send());
    tick(302); assert(count_wire(0x12,0xa3)==1 && count_wire(0x10,0xa3)==1);
    assert(!count_wire(8,0xa3) && !count_wire(0x12,0xb1));
    accepted(7,3,10,0); assert(sp_fsm_term_input_can_send());
    reset_capture(); raw_claim(7,other_name); tick(303);
    assert(!sp_fsm_term_input_can_send() && !count_wire(0x10,0xa3));
    milliseconds=1304; announce(9,antenna_name,11,10); assert(count_raw(0xea,9)==1);
    raw_claim(9,antenna_name); tick(1305);
    assert(sp_fsm_get_peer_sa()==9 && count_wire(0x10,0xa3)==1); stop();
}
static void test_shutdown_stops_callbacks(void) {
    start(); claim_ready(); connected(7,11,10); reset_capture();
    sp_fsm_deinit(); assert(count_wire(0x12,0xb1)==1 && count_wire(0x12,0xa3)==1);
    assert(sp_fsm_get_peer_sa()==0xff && !sp_fsm_term_input_can_send());
    unsigned before=sent_count; const uint8_t text[]="late output";
    app(7,0,2,8,0x3a,text,sizeof(text)-1); tick(1000);
    assert(sent_count==before && !receive_calls && !sp_fsm_term_input_can_send());
    assert(sp_fsm_unbind_layers() && teardown_order==2);
}
static void test_network_snapshot_ages_without_tick(void) {
    start(); claim_ready(); connected(7,11,10);
    const uint8_t registration[]={0,0,244,0x31,0x24};
    const uint8_t protocol[]={0,11,0,0,0,0};
    const uint8_t channel_state[]={1};
    const uint8_t channel[]={1,0,244,0x31,0x24,1,7};
    binary_response(0,100,0x404c,registration,sizeof(registration),500);
    binary_response(1,101,0x4073,protocol,sizeof(protocol),600);
    binary_response(2,102,0x4043,channel_state,sizeof(channel_state),700);
    binary_response(3,103,0x4054,channel,sizeof(channel),800);
    protocol_antenna_status_t status; sp_fsm_get_antenna_status(&status);
    assert(status.online && status.registration_fresh && status.ocean_fresh);
    assert(status.protocol_fresh && status.channel_state_fresh && status.channel_fresh);
    assert(status.ocean_region==2 && status.current_protocol==11 && status.channel_number==12580);
    assert(status.registration_age_ms==300 && status.protocol_age_ms==200);
    assert(status.channel_state_age_ms==100 && status.channel_age_ms==0);
    unsigned before=sent_count;
    /* No tick, CAN delivery, or publication: UI getters must age the last
     * snapshot independently, including the inclusive 15-second boundary. */
    milliseconds=15500; sp_fsm_get_antenna_status(&status);
    assert(status.online && status.registration_fresh && status.ocean_fresh);
    assert(status.registration_age_ms==15000 && status.protocol_age_ms==14900);
    milliseconds=15501; sp_fsm_get_antenna_status(&status);
    assert(status.online && !status.registration_fresh && !status.ocean_fresh);
    assert(status.protocol_fresh && status.channel_state_fresh && status.channel_fresh);
    milliseconds=15701; sp_fsm_get_antenna_status(&status);
    assert(status.online && !status.protocol_fresh && !status.channel_state_fresh);
    assert(status.channel_fresh && status.channel_age_ms==14901);
    milliseconds=15801; sp_fsm_get_antenna_status(&status);
    assert(!status.online && !status.channel_fresh && status.channel_age_ms==15001);
    assert(status.registration_valid && status.ocean_valid && status.protocol_valid);
    assert(status.channel_valid && status.channel_state_valid && status.channel_number==12580);
    assert(sent_count==before); /* Reading status never sends or polls. */
    /* New traffic refreshes only its own group even after the getter aged
     * all groups offline. Cached registration/channel stay visibly stale. */
    binary_response(4,104,0x4073,protocol,sizeof(protocol),15900);
    sp_fsm_get_antenna_status(&status);
    assert(status.online && status.protocol_fresh && status.protocol_age_ms==0);
    assert(!status.registration_fresh && !status.ocean_fresh && !status.channel_fresh);
    assert(!status.channel_state_fresh && status.registration_age_ms==15400);
    stop(); sp_fsm_get_antenna_status(&status);
    assert(!status.online && !status.protocol_fresh && status.protocol_valid);
    assert(status.registration_valid && status.channel_valid && status.ocean_valid);
}
static void test_network_snapshot_age_saturation(void) {
    start(); claim_ready(); connected(7,11,10);
    const uint8_t registration[]={0,0,244,0x31,0x24};
    binary_response(0,100,0x404c,registration,sizeof(registration),500);
    /* Publish a nearly UINT32_MAX age, then cross the millisecond wrap using
     * only the getter. The accumulated age must saturate, never look recent. */
    milliseconds=UINT32_MAX-100u; raw_claim(8,other_name);
    protocol_antenna_status_t status; sp_fsm_get_antenna_status(&status);
    assert(status.registration_age_ms==UINT32_MAX-600u);
    milliseconds=700; sp_fsm_get_antenna_status(&status);
    assert(status.registration_age_ms==UINT32_MAX && !status.registration_fresh);
    assert(!status.ocean_fresh && status.registration_valid && status.ocean_valid);
    assert(status.protocol_age_ms==UINT32_MAX && status.channel_age_ms==UINT32_MAX);
    assert(status.channel_state_age_ms==UINT32_MAX);
    stop();
}
int main(void) {
    test_cold_claim_and_collision(); test_discovery_and_dynamic_ports();
    test_backpressure_and_routing(); test_ttl_requires_address_claim();
    test_foreign_claim_and_owner_revoke(); test_shutdown_stops_callbacks();
    test_network_snapshot_ages_without_tick(); test_network_snapshot_age_saturation();
    puts("TT6006 profile: 8 integration groups passed"); return 0;
}
