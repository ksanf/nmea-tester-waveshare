/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
/* TT6006 service profile: address claim, discovery and independent NDP clients.
 * All protocol state is serialized by g_lock; UI reads short snapshots only. */
#include "fsm.h"
#include "sailor_ndp_session.h"
#include "bridge_can_config.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define TAG "TT6006"
#define CLAIM_MS 300u
#define DISCOVERY_MS 3000u
#define PEER_TIMEOUT_MS 15000u

typedef struct {
    bool inited, claimed, peer_selected, peer_live;
    uint8_t local_sa, local_name[8];
    uint16_t claim_tries;
    char serial[32];
    uint32_t now, claim_at, discover_at, peer_at, resolve_at;
    sailor_ndp_peer_t peer;
    sailor_ndp_session_t ndp;
    sailor_telem_t telem;
    sp_fsm_term_rx_cb_t terminal_rx;
    void *terminal_user;
    sp_l2_evt_cb_t ext_l2_evt;
    sp_tr_evt_cb_t ext_tr_evt;
    void *ext_user;
} profile_t;
static profile_t g;
static SemaphoreHandle_t g_lock;
static atomic_int g_state = ATOMIC_VAR_INIT(SP_ST_BOOT);
static atomic_uchar g_peer_sa = ATOMIC_VAR_INIT(0xff);
static portMUX_TYPE g_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static protocol_antenna_status_t g_snapshot;
static uint32_t g_snapshot_at;

static uint32_t clock_ms(void) { return (uint32_t)(esp_timer_get_time()/1000); }
static bool due(uint32_t now, uint32_t at) { return (int32_t)(now-at)>=0; }
static bool lock(void) { return g_lock && xSemaphoreTake(g_lock,portMAX_DELAY)==pdTRUE; }
static void unlock(void) { xSemaphoreGive(g_lock); }
static void publish(void) {
    protocol_antenna_status_t out;
    sailor_telem_snapshot(&g.telem,g.now,&out);
    portENTER_CRITICAL(&g_snapshot_lock);
    g_snapshot=out; g_snapshot_at=g.now;
    portEXIT_CRITICAL(&g_snapshot_lock);
    if(g.claimed) atomic_store(&g_state,
        sailor_ndp_session_is_up(&g.ndp,SAILOR_NDP_TERMINAL_SLOT)?SP_ST_ONLINE:SP_ST_WAIT_POLL);
    atomic_store(&g_peer_sa,g.peer_live?g.peer.sa:0xff);
}
static uint32_t age_add(uint32_t age,uint32_t elapsed) {
    return UINT32_MAX-age<elapsed?UINT32_MAX:age+elapsed;
}
void sp_fsm_get_antenna_status(protocol_antenna_status_t *out) {
    if(!out) return;
    portENTER_CRITICAL(&g_snapshot_lock);
    *out=g_snapshot; uint32_t at=g_snapshot_at;
    portEXIT_CRITICAL(&g_snapshot_lock);
    uint32_t elapsed=clock_ms()-at;
    out->signal_age_ms=age_add(out->signal_age_ms,elapsed);
    out->position_age_ms=age_add(out->position_age_ms,elapsed);
    out->registration_age_ms=age_add(out->registration_age_ms,elapsed);
    out->protocol_age_ms=age_add(out->protocol_age_ms,elapsed);
    out->channel_age_ms=age_add(out->channel_age_ms,elapsed);
    out->channel_state_age_ms=age_add(out->channel_state_age_ms,elapsed);
    if(elapsed>PEER_TIMEOUT_MS) out->online=false;
    if(!out->online || out->signal_age_ms>15000) out->signal_valid=false;
    if(!out->online || out->position_age_ms>5000) out->position_fresh=false;
    if(!out->online || out->registration_age_ms>SAILOR_TELEM_NETWORK_MS) {
        out->registration_fresh=false;
        out->ocean_fresh=false;
    }
    if(!out->online || out->protocol_age_ms>SAILOR_TELEM_NETWORK_MS) out->protocol_fresh=false;
    if(!out->online || out->channel_age_ms>SAILOR_TELEM_NETWORK_MS) out->channel_fresh=false;
    if(!out->online || out->channel_state_age_ms>SAILOR_TELEM_NETWORK_MS) out->channel_state_fresh=false;
}

static bool wire_tx(void *user,uint8_t peer,uint8_t series,uint8_t channel,
                    const uint8_t *p,uint16_t n,bool ack) {
    (void)user;
    if(!g.claimed || g.local_sa>=252) return false;
    sp_id_fields_t id={.pri=(ack || series>=0x10)?3:6,.dp=1,.pf=0xef,.ps=peer,.sa=g.local_sa};
    if(ack) return sp_tr_send_short_ack(&id,series,channel)==SP_OK;
    sp_app_block_t b={.sig={0x5f,0x99,2},.series=series,.channel_le=channel,.app_data=p,.app_len=n};
    return sp_tr_send_app(&id,&b,NULL)==SP_OK;
}
static bool binary_tx(void *user,const uint8_t *p,uint16_t n,uint32_t cookie) {
    (void)user;
    return sailor_ndp_session_send(&g.ndp,SAILOR_NDP_BINARY_SLOT,p,n,cookie,g.now);
}
static bool received(void *user,uint8_t slot,const uint8_t *p,uint16_t n,uint32_t now) {
    (void)user;
    g.peer_at=now;
    if(slot==SAILOR_NDP_BINARY_SLOT) return sailor_telem_rx(&g.telem,p,n,now);
    return !n || (g.terminal_rx && g.terminal_rx(p,n,g.terminal_user));
}
static void state_changed(void *user,uint8_t slot,sailor_ndp_session_state_t state,
                          sailor_ndp_session_reason_t reason,uint32_t now) {
    (void)user;
    if(slot==SAILOR_NDP_BINARY_SLOT)
        sailor_telem_on_session(&g.telem,state==SAILOR_NDP_SESSION_UP,now);
    HLOGI(TAG,"%s session state=%u reason=%u",slot==SAILOR_NDP_BINARY_SLOT?"Binary":"Terminal",(unsigned)state,(unsigned)reason);
}
static void completed(void *user,uint8_t slot,uint32_t cookie,bool ok,uint32_t now) {
    (void)user;
    if(slot==SAILOR_NDP_BINARY_SLOT) sailor_telem_tx_complete(&g.telem,cookie,ok,now);
}
static bool claim(void) {
    return sp_l2_send_quick(6,0,0xee,0xff,g.local_sa,g.local_name,8)==SP_OK;
}
static void request_name(uint8_t sa) {
    const uint8_t p[]={0x00,0xee,0x00};
    if(g.claimed && due(g.now,g.resolve_at)) {
        (void)sp_l2_send_quick(6,0,0xea,sa,g.local_sa,p,sizeof(p));
        g.resolve_at=g.now+1000;
    }
}
static void discovery_tx(bool announce) {
    uint8_t p[50]={1};
    uint16_t len=1;
    if(announce) {
        p[0]=2; p[1]=0; /* No PPP server is advertised by this service profile. */
        memcpy(p+2,g.local_name,8); p[10]=1; p[12]=1;
        p[14]=1; /* Bridge profile version 1.0, independent of vendor firmware. */
        memcpy(p+18,g.serial,32); len=sizeof(p);
    }
    sp_id_fields_t id={.pri=3,.dp=1,.pf=0xef,.ps=0xff,.sa=g.local_sa};
    sp_app_block_t b={.sig={0x5f,0x99,3},.series=8,.channel_le=0,.app_data=p,.app_len=len};
    (void)sp_tr_send_app(&id,&b,NULL);
}
static void bind_services(const sailor_ndp_peer_t *p) {
    const bool present[]={p->binary_service,p->terminal_service};
    const uint8_t ports[]={p->binary_port,p->terminal_port};
    for(uint8_t i=0;i<SAILOR_NDP_SESSION_SLOTS;i++) {
        sailor_ndp_session_slot_t *s=&g.ndp.slots[i];
        if(s->state!=SAILOR_NDP_SESSION_UNBOUND &&
           (!present[i] || s->remote_port!=ports[i]))
            sailor_ndp_session_unbind(&g.ndp,i,g.now);
        if(present[i]) (void)sailor_ndp_session_bind(&g.ndp,i,p,ports[i],g.now);
    }
}
static void discover(uint8_t sa,const uint8_t *p,uint16_t n) {
    sailor_ndp_peer_t peer;
    if(!sailor_ndp_session_parse_discovery(sa,p,n,&peer) ||
       !memcmp(peer.name,g.local_name,8)) return;
    if(g.peer_selected && memcmp(peer.name,g.peer.name,8)) return;
    if(g.peer_selected && peer.sa!=g.peer.sa) {
        request_name(peer.sa); return;
    }
    g.peer=peer; g.peer_selected=true; g.peer_live=true; g.peer_at=g.now;
    bind_services(&peer);
    (void)sailor_telem_discover(&g.telem,sa,p,n,g.now);
}
static void tr_app(const sp_id_fields_t *id,uint32_t pgn,uint8_t sid,
                   const sp_app_block_t *b,void *user) {
    (void)pgn; (void)sid; (void)user;
    if(!id || !b || !lock()) return;
    g.now=clock_ms();
    if(g.inited && g.claimed && id->sa!=g.local_sa &&
       (id->ps==g.local_sa || id->ps==0xff) &&
       b->sig[0]==0x5f && b->sig[1]==0x99) {
        if(b->sig[2]==3 && b->channel_le==0 && b->series>=8 && b->series<=15) {
            if(b->app_len==1 && b->app_data[0]==1) discovery_tx(true);
            else discover(id->sa,b->app_data,b->app_len);
        } else if(b->sig[2]==2 && id->ps==g.local_sa) {
            sailor_ndp_session_rx(&g.ndp,id->sa,id->ps,b->series,
                (uint8_t)b->channel_le,b->app_data,b->app_len,g.now);
        }
        publish();
    }
    unlock();
}
static int name_compare(const uint8_t a[8],const uint8_t b[8]) {
    for(int i=7;i>=0;i--) if(a[i]!=b[i]) return a[i]<b[i]?-1:1;
    return 0;
}
static void on_claim(const sp_l2_frame_t *f) {
    if(f->dlc!=8 || f->id.ps!=0xff || f->id.sa>=252) return;
    if(f->id.sa==g.local_sa) {
        int order=name_compare(g.local_name,f->data);
        if(order<0) { (void)claim(); return; }
        if(!order) return;
        g.claimed=false;
        if(++g.claim_tries>=252) {
            g.local_sa=0xfe; sailor_ndp_session_set_local_sa(&g.ndp,0xfe,g.now);
            (void)claim(); atomic_store(&g_state,SP_ST_OFF); return;
        }
        g.local_sa=(uint8_t)((g.local_sa+1u)%252u);
        sailor_ndp_session_set_local_sa(&g.ndp,g.local_sa,g.now);
        if(claim()) { g.claim_at=g.now+CLAIM_MS; atomic_store(&g_state,SP_ST_CLAIM); }
        else { g.claim_at=g.now+200; atomic_store(&g_state,SP_ST_BOOT); }
        return;
    }
    bool resolved=sailor_ndp_session_peer_claim(&g.ndp,f->data,f->id.sa,g.now);
    if(g.peer_selected && !memcmp(f->data,g.peer.name,8)) {
        if(resolved || !g.peer_live) {
            g.peer.sa=f->id.sa; g.peer_live=true; g.peer_at=g.now;
            bind_services(&g.peer);
        }
    } else if(g.peer_live && f->id.sa==g.peer.sa) {
        g.peer.sa=0xfe; /* A different NAME owns the old address now. */
    }
}
static void l2_rx(const sp_l2_frame_t *f,void *user) {
    (void)user;
    if(!f || !lock()) return;
    bool transport=false;
    g.now=clock_ms();
    if(g.inited) {
        if(!f->id.dp && f->id.pf==0xee) on_claim(f);
        else if(!f->id.dp && f->id.pf==0xea && f->dlc>=3 &&
                (f->id.ps==0xff || f->id.ps==g.local_sa) &&
                f->data[0]==0 && f->data[1]==0xee && f->data[2]==0) (void)claim();
        else transport=g.claimed && f->id.dp==1 && f->id.pf==0xef &&
                f->id.sa!=g.local_sa && (f->id.ps==g.local_sa || f->id.ps==0xff);
        publish();
    }
    unlock();
    /* Transport delivers synchronously and takes the profile lock in tr_app.
     * Its RX buffers have one producer; never hold g_lock across this call. */
    if(transport) sp_tr_on_l2_frame(f);
}
static void l2_event(sp_event_t e,void *u) { (void)u; if(g.ext_l2_evt) g.ext_l2_evt(e,g.ext_user); }
static void tr_event(sp_event_t e,const sp_id_fields_t *id,uint8_t sid,void *u) {
    (void)u; if(g.ext_tr_evt) g.ext_tr_evt(e,id,sid,g.ext_user);
}

sp_err_t sp_fsm_init(const sp_fsm_config_t *cfg) {
    if(!cfg) return SP_E_INVAL;
    if(!g_lock) g_lock=xSemaphoreCreateMutex();
    if(!lock()) return SP_E_NOMEM;
    if(g.inited) { unlock(); return SP_E_STATE; }
    memset(&g,0,sizeof(g)); g.now=clock_ms(); g.claim_at=g.now;
    g.local_sa=cfg->local_sa<252?cfg->local_sa:0;
    uint8_t mac[6];
    if(esp_read_mac(mac,ESP_MAC_WIFI_STA)!=ESP_OK) { unlock(); return SP_E_STATE; }
    uint32_t identity=2166136261u;
    for(unsigned i=0;i<6;i++) identity=(identity^mac[i])*16777619u;
    uint64_t name=UINT64_C(0xc0a0a0002be00000)|(identity&0x1fffffu);
    for(unsigned i=0;i<8;i++) g.local_name[i]=(uint8_t)(name>>(8*i));
    uint8_t nonzero=0;
    for(unsigned i=0;i<8;i++) nonzero|=cfg->local_name[i];
    if(nonzero) memcpy(g.local_name,cfg->local_name,8);
    snprintf(g.serial,sizeof(g.serial),"%lu",(unsigned long)identity);
    const sailor_ndp_session_callbacks_t cb={.transmit=wire_tx,.receive=received,
        .state=state_changed,.complete=completed};
    sailor_telem_init(&g.telem,binary_tx,NULL);
    sailor_ndp_session_init(&g.ndp,g.local_name,g.local_sa,&cb,NULL);
    g.inited=true; atomic_store(&g_state,SP_ST_BOOT); publish(); unlock(); return SP_OK;
}
void sp_fsm_tick(void) {
    if(!lock()) return;
    if(!g.inited) { unlock(); return; }
    uint32_t now=clock_ms(); /* RX may have advanced state while this caller waited for g_lock. */
    g.now=now;
    sp_state_t state=(sp_state_t)atomic_load(&g_state);
    if(state==SP_ST_BOOT && due(now,g.claim_at)) {
        if(claim()) { g.claim_at=now+CLAIM_MS; atomic_store(&g_state,SP_ST_CLAIM); }
        else g.claim_at=now+200;
    } else if(state==SP_ST_CLAIM && due(now,g.claim_at)) {
        g.claimed=true; g.discover_at=now;
    }
    if(g.claimed) {
        if(g.peer_live && now-g.peer_at>PEER_TIMEOUT_MS) {
            for(uint8_t i=0;i<SAILOR_NDP_SESSION_SLOTS;i++) sailor_ndp_session_unbind(&g.ndp,i,now);
            g.peer_live=false;
        }
        if(due(now,g.discover_at)) {
            discovery_tx(false); discovery_tx(true); g.discover_at=now+DISCOVERY_MS;
        }
        sailor_telem_tick(&g.telem,now);
        sailor_ndp_session_tick(&g.ndp,now);
    }
    publish(); unlock();
}
void sp_fsm_set_term_rx(sp_fsm_term_rx_cb_t cb,void *user) {
    if(!lock()) return;
    g.terminal_rx=cb; g.terminal_user=user; unlock();
}
sp_err_t sp_fsm_term_send(const uint8_t *p,uint16_t n) {
    if(!p || !n || n>SAILOR_NDP_SESSION_MAX_DATA) return SP_E_INVAL;
    if(!lock()) return SP_E_STATE;
    g.now=clock_ms();
    bool ok=g.inited && g.claimed && sailor_ndp_session_send(&g.ndp,SAILOR_NDP_TERMINAL_SLOT,p,n,0,g.now);
    unlock(); return ok?SP_OK:SP_E_STATE;
}
bool sp_fsm_term_input_can_send(void) {
    if(!lock()) return false;
    bool ok=g.inited && g.claimed && sailor_ndp_session_is_up(&g.ndp,SAILOR_NDP_TERMINAL_SLOT) &&
        g.ndp.slots[SAILOR_NDP_TERMINAL_SLOT].count<SAILOR_NDP_SESSION_QUEUE;
    unlock(); return ok;
}
void sp_fsm_term_cancel_pending(void) {
    if(!lock()) return;
    g.now=clock_ms();
    if(g.inited && g.ndp.slots[SAILOR_NDP_TERMINAL_SLOT].count) {
        sailor_ndp_session_unbind(&g.ndp,SAILOR_NDP_TERMINAL_SLOT,g.now);
        if(g.peer_live && g.peer.terminal_service)
            (void)sailor_ndp_session_bind(&g.ndp,SAILOR_NDP_TERMINAL_SLOT,&g.peer,g.peer.terminal_port,g.now);
        publish();
    }
    unlock();
}
sp_state_t sp_fsm_get_state(void) { return (sp_state_t)atomic_load(&g_state); }
uint8_t sp_fsm_get_peer_sa(void) { return atomic_load(&g_peer_sa); }
void sp_fsm_deinit(void) {
    if(!lock()) return;
    if(!g.inited) { unlock(); return; }
    g.now=clock_ms();
    for(uint8_t i=0;i<SAILOR_NDP_SESSION_SLOTS;i++) sailor_ndp_session_unbind(&g.ndp,i,g.now);
    g.claimed=false;
    sailor_telem_disconnect(&g.telem,g.now);
    g.inited=false; g.peer_live=false; atomic_store(&g_state,SP_ST_BOOT); publish(); unlock();
}
sp_err_t sp_fsm_bind_layers(const sp_l2_config_t *l2,const sp_timing_t *timing,
    uint16_t slots,sp_l2_evt_cb_t on_l2,
    sp_tr_evt_cb_t on_tr,void *user) {
    if(!l2 || !g.inited) return SP_E_INVAL;
    g.ext_l2_evt=on_l2; g.ext_tr_evt=on_tr; g.ext_user=user;
    sp_err_t err=sp_tr_init(timing,slots?slots:4,tr_app,tr_event,NULL);
    if(err!=SP_OK) return err;
    sp_l2_config_t cfg=*l2; cfg.local_sa=g.local_sa;
    err=sp_l2_init(&cfg,l2_rx,l2_event,NULL);
    if(err!=SP_OK) sp_tr_deinit();
    return err;
}
bool sp_fsm_unbind_layers(void) {
    if(!sp_l2_deinit()) return false;
    sp_tr_deinit(); return true;
}
