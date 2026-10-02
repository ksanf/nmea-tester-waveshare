/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#include "sailor_telemetry.h"
#include <string.h>

#define OFFLINE_MS 15000u
#define RESPONSE_MS 12000u
#define SIGNAL_MS 15000u
#define POSITION_MS 5000u

static const uint16_t poll_ops[SAILOR_TELEM_POLLS] = {
    0x2004, 0x2108, 0x2050, 0x204c, 0x2073, 0x2043, 0x2054
};
static const uint32_t poll_intervals[SAILOR_TELEM_POLLS] = {
    2000, 1000, 3000, 5000, 5000, 5000, 5000
};
enum { NETWORK_REGISTRATION, NETWORK_PROTOCOL, NETWORK_CHANNEL_STATE, NETWORK_TDM };
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0]|(p[1]<<8)); }
static bool reached(uint32_t now, uint32_t due) { return (int32_t)(now-due)>=0; }
static bool epoch_valid(uint32_t t) { return t>=946684800u && t<4102444800u; }

uint16_t sailor_telem_crc(const uint8_t *p, size_t n) {
    uint16_t c=0xffff;
    while(n--) {
        c^=*p++;
        for(unsigned i=0;i<8;i++) c=(uint16_t)((c>>1)^((c&1)?0x8408:0));
    }
    return c^0xffff;
}
static void append_crc(uint8_t *p, size_t n) {
    uint16_t c=sailor_telem_crc(p,n); p[n]=(uint8_t)c; p[n+1]=(uint8_t)(c>>8);
}
static void reset_session(sailor_telem_t *s, uint32_t now) {
    s->accepted=false; s->have_rx=false; s->network_seen=0; s->poll_next=0;
    s->control_count=0; s->control_head=0; s->rx_count=0; s->rx_next=0;
    memset(s->upper,0,sizeof(s->upper));
    s->status.online=false; s->have_clock=false; s->position_report_valid=false;
    for(unsigned i=0;i<SAILOR_TELEM_POLLS;i++)
        s->polls[i]=(sailor_telem_poll_t){.opcode=poll_ops[i],.due=now};
    /* Preserve cookie/sequence counters across reconnects: late completion or
     * upper ACK from a previous run must not match a newly queued request. */
}
void sailor_telem_init(sailor_telem_t *s, sailor_telem_send_fn send, void *user) {
    memset(s,0,sizeof(*s)); s->send=send; s->user=user; s->peer_sa=0xff;
    s->binary_seq=1; reset_session(s,0);
}
void sailor_telem_disconnect(sailor_telem_t *s, uint32_t now) {
    reset_session(s,now); s->discovered=false; s->peer_sa=0xff;
}
bool sailor_telem_discover(sailor_telem_t *s, uint8_t sa,
                          const uint8_t *p, size_t n, uint32_t now) {
    if(!p || n<50 || sa>=0xfe || p[0]!=2 || p[1]>16 ||
       n!=50u+3u*p[1] || le16(p+10)!=2) return false;
    bool nonzero=false, nonff=false, service=false, terminal=false;
    for(unsigned i=2;i<10;i++) { nonzero|=p[i]!=0; nonff|=p[i]!=0xff; }
    if(!nonzero || !nonff) return false;
    for(size_t i=50;i<n;i+=3) {
        uint16_t type=le16(p+i);
        if(p[i+2]>15) return false;
        if(type==1) { if(service) return false; service=true; }
        if(type==2) { if(terminal) return false; terminal=true; }
    }
    if(!service) return false;
    bool other=memcmp(s->peer_name,p+2,8)!=0;
    if(other) {
        memset(&s->status,0,sizeof(s->status));
        s->have_signal=false; s->have_position=false;
    }
    if(!s->discovered || other) reset_session(s,now);
    memcpy(s->peer_name,p+2,8); s->peer_sa=sa; s->discovered=true;
    size_t digits=0;
    while(digits<16 && p[18+digits]>='0' && p[18+digits]<='9') digits++;
    if(digits && p[18+digits]==0) {
        memcpy(s->status.serial,p+18,digits); s->status.serial[digits]=0;
        s->status.identity_valid=true;
    }
    return true;
}
static bool control_enqueue(sailor_telem_t *s, const uint8_t *p, size_t n) {
    if(n>sizeof(s->controls[0].data)) return false;
    /* Repeated upper packets can request the same retained reply. */
    for(unsigned i=0;i<s->control_count;i++) {
        const sailor_telem_control_t *c=&s->controls[(s->control_head+i)%SAILOR_TELEM_CONTROL_QUEUE];
        if(c->len==n && !memcmp(c->data,p,n)) return true;
    }
    if(s->control_count==SAILOR_TELEM_CONTROL_QUEUE) return false;
    sailor_telem_control_t *c=&s->controls[(s->control_head+s->control_count)%SAILOR_TELEM_CONTROL_QUEUE];
    memcpy(c->data,p,n); c->len=(uint8_t)n; s->control_count++;
    return true;
}
static size_t normal(uint8_t *p, uint16_t seq, uint16_t op, uint8_t kind) {
    p[0]=2; p[1]=(uint8_t)(seq>>8); p[2]=(uint8_t)seq;
    p[3]=1; p[4]=0x20; p[5]=0x10; append_crc(p,6); p[8]=kind;
    size_t n=9;
    if(kind==2) { p[9]=(uint8_t)(op>>8); p[10]=(uint8_t)op; p[11]=3; n=12; }
    append_crc(p,n); return n+2;
}
void sailor_telem_on_session(sailor_telem_t *s, bool up, uint32_t now) {
    if(up && s->accepted) return;
    reset_session(s,now);
    if(!up || !s->discovered) return;
    s->accepted=true;
    uint8_t enq[16]; size_t n=normal(enq,0x7fff,0,5);
    (void)control_enqueue(s,enq,n);
}

typedef struct {
    size_t header, plain;
    uint16_t sequence;
    uint8_t fragment, kind;
    bool crc_header;
} envelope_t;
/* A bad header cannot be addressed safely, so it is never NAKed. */
static bool header_decode(const uint8_t *p, size_t n, envelope_t *e) {
    static const uint8_t sizes[]={3,4,6,8};
    if(!p || n<3 || (p[0]&0xec)) return false;
    unsigned type=p[0]&3;
    e->plain=sizes[type]; e->crc_header=!(p[0]&0x10);
    e->header=e->plain+(e->crc_header?2u:0u);
    if(n<e->header+3) return false;
    if(e->crc_header && le16(p+e->plain)!=sailor_telem_crc(p,e->plain)) return false;
    if(type>=2 && (p[4]!=0x10 || p[5]!=0x20)) return false;
    if(type==3 && (p[6]!=0 || p[7]!=0)) return false;
    e->sequence=be16(p+1); e->fragment=type?p[3]:1;
    if(!e->fragment) return false;
    e->kind=p[e->header]; return true;
}
static bool control_reply(sailor_telem_t *s, const uint8_t *p,
                          const envelope_t *e, uint8_t kind) {
    uint8_t reply[13]; memcpy(reply,p,e->plain);
    if(e->plain>=6) { reply[4]=p[5]; reply[5]=p[4]; }
    if(e->plain==8) { reply[6]=p[7]; reply[7]=p[6]; }
    if(e->crc_header) append_crc(reply,e->plain);
    reply[e->header]=kind; append_crc(reply,e->header+1);
    return control_enqueue(s,reply,e->header+3);
}
static uint32_t packet_key(const envelope_t *e) {
    return ((uint32_t)e->sequence<<8)|e->fragment;
}
static bool duplicate(const sailor_telem_t *s, uint32_t key) {
    for(unsigned i=0;i<s->rx_count;i++) if(s->rx_keys[i]==key) return true;
    return false;
}
static void remember(sailor_telem_t *s, uint32_t key) {
    s->rx_keys[s->rx_next]=key; s->rx_next=(uint8_t)((s->rx_next+1)%3);
    if(s->rx_count<3) s->rx_count++;
}
static int poll_index(uint16_t opcode) {
    for(unsigned i=0;i<SAILOR_TELEM_POLLS;i++) if(poll_ops[i]==opcode) return (int)i;
    return -1;
}
static void drop_upper(sailor_telem_t *s, sailor_telem_tx_t *q, uint32_t now) {
    int i=poll_index(q->opcode);
    if(i>=0 && s->polls[i].pending) {
        s->polls[i].pending=false; s->polls[i].due=now+poll_intervals[i];
    }
    memset(q,0,sizeof(*q));
}
static sailor_telem_tx_t *oldest(sailor_telem_t *s) {
    sailor_telem_tx_t *q=NULL;
    for(unsigned i=0;i<SAILOR_TELEM_UPPER_WINDOW;i++) {
        sailor_telem_tx_t *candidate=&s->upper[i];
        if(candidate->state!=SAILOR_TELEM_TX_FREE && candidate->sent &&
           (!q || (int32_t)(candidate->order-q->order)<0)) q=candidate;
    }
    return q;
}
static void retry(sailor_telem_t *s, sailor_telem_tx_t *q, uint32_t now) {
    if(q->state==SAILOR_TELEM_TX_RETRY || q->state==SAILOR_TELEM_TX_QUEUED) return;
    if(q->retries>=SAILOR_TELEM_RETRIES) { drop_upper(s,q,now); return; }
    q->state=SAILOR_TELEM_TX_RETRY; q->cookie=0;
}
static void receive_ack(sailor_telem_t *s, const envelope_t *e, bool nak, uint32_t now) {
    if(e->fragment!=1) return;
    sailor_telem_tx_t *q=NULL;
    for(unsigned i=0;i<SAILOR_TELEM_UPPER_WINDOW;i++)
        if(s->upper[i].state!=SAILOR_TELEM_TX_FREE && s->upper[i].sent &&
           s->upper[i].sequence==e->sequence) q=&s->upper[i];
    if(!q) return;
    sailor_telem_tx_t *first=oldest(s);
    if(first && first!=q) retry(s,first,now);
    if(nak) retry(s,q,now);
    else memset(q,0,sizeof(*q)); /* Upper ACK is not an application response. */
}
void sailor_telem_tx_complete(sailor_telem_t *s, uint32_t cookie, bool success, uint32_t now) {
    if(!s->accepted || !cookie) return;
    for(unsigned i=0;i<SAILOR_TELEM_UPPER_WINDOW;i++) {
        sailor_telem_tx_t *q=&s->upper[i];
        if(q->state!=SAILOR_TELEM_TX_NDP || q->cookie!=cookie) continue;
        if(!success) { drop_upper(s,q,now); return; }
        q->state=SAILOR_TELEM_TX_ACK; q->due=now+SAILOR_TELEM_ACK_MS;
        int pi=poll_index(q->opcode);
        if(pi>=0 && s->polls[pi].pending) s->polls[pi].requested=now;
        return;
    }
}
static bool coordinate(const uint8_t *p, unsigned max, double *out) {
    unsigned frac=be16(p+3);
    /* TT6006 DecodeGPSPosition -> GUI convertGpsPosition: 0=N/E, 1=S/W;
     * the BE16 fraction is thousandths of one minute. */
    if(p[0]>1 || p[1]>max || p[2]>=60 || frac>=1000 ||
       (p[1]==max && (p[2] || frac))) return false;
    double value=p[1]+(p[2]+frac/1000.0)/60.0;
    *out=p[0]?-value:value; return true;
}
static void network_received(sailor_telem_t *s, unsigned group, uint32_t now) {
    s->network_seen|=(uint8_t)(1u<<group); s->network_at[group]=now;
}
static bool decode_network(sailor_telem_t *s, uint16_t op,
                           const uint8_t *p, size_t n, uint32_t now) {
    protocol_antenna_status_t *o=&s->status;
    if(op==0x404c) {
        /* TT6006 DecodeInmarsatCLoginStatus@0806f4c0: DATA0==0 is logged
         * in. Only then are DATA1..4 the registered NCS/channel. Nonzero
         * replies can contain unrelated/old bytes there: never decode them.
         * Base response has 5 bytes; the observed extended form has 18.
         * The vendor also accepts a nonzero state alone. Do not reproduce its
         * unchecked reads for truncated extended responses. */
        if(n!=5 && n!=18 && !(n==1 && p[0]!=0)) return false;
        o->registration_state=p[0]; o->registration_valid=true;
        o->registered_ncs=p[0]==0?be16(p+1):0;
        o->registered_channel=p[0]==0?be16(p+3):0;
        /* sockGetCurrentOceanRegion@0805e9f0: current registered ocean,
         * NOT the preferred-ocean configuration returned by 4061. */
        o->ocean_valid=p[0]==0 && o->registered_ncs>=1 && o->registered_ncs<=399;
        o->ocean_region=o->ocean_valid?(uint8_t)(o->registered_ncs/100u):0;
        network_received(s,NETWORK_REGISTRATION,now);
    } else if(op==0x4073 && n==6) {
        /* DecodeMESStatus@0806e050 -> GetMesStatus -> sockGetTransceiverStatus:
         * DATA1 is the current protocol. DATA0/flags are separate states. */
        o->current_protocol=p[1]; o->protocol_valid=true;
        network_received(s,NETWORK_PROTOCOL,now);
    } else if(op==0x4043 && n==1) {
        o->channel_state=p[0]; o->channel_state_valid=true;
        network_received(s,NETWORK_CHANNEL_STATE,now);
    } else if(op==0x4054 && n==7) {
        /* DecodeCurrentTDM@0806e1b0: byte state, BE16 origin/channel/frame.
         * Channel 0/FFFF means unavailable; preserve no apparently current
         * number after a new unavailable report. TDM state remains raw. */
        o->tdm_state=p[0]; o->tdm_origin=be16(p+1); o->tdm_frame=be16(p+5);
        o->channel_number=be16(p+3);
        o->channel_valid=o->channel_number!=0 && o->channel_number!=UINT16_MAX;
        network_received(s,NETWORK_TDM,now);
    } else return false;
    return true;
}
static bool decode(sailor_telem_t *s, uint16_t op, const uint8_t *p, size_t n, uint32_t now) {
    if(op==0x4050 && n==12 && p[1]<=99 && p[3]<=5) {
        s->status.cn0_dbhz=p[1]; s->status.signal_bars=p[3];
        s->signal_at=now; s->have_signal=true;
    } else if(op==0x4004 && n==4 && epoch_valid(be32(p))) {
        s->clock_utc=be32(p); s->clock_at=now; s->have_clock=true;
    } else if(op==0x4108 && n==20) {
        /* DecodeGPSPosition@806f318 -> native u32 -> GUI convertPosition@819fb23:
         * zero invalid, any nonzero valid. A reported invalid fix preserves
         * the last known point but cannot
         * leave it marked Fresh, even if its old UTC still matches the clock. */
        if(!p[0]) { s->position_report_valid=false; return true; }
        double lat,lon; uint32_t utc=be32(p+1);
        if(!epoch_valid(utc) || !coordinate(p+5,90,&lat) || !coordinate(p+10,180,&lon)) return false;
        s->status.latitude=lat; s->status.longitude=lon; s->status.position_utc=utc;
        s->position_at=now; s->have_position=true; s->position_report_valid=true;
    } else return decode_network(s,op,p,n,now);
    return true;
}
bool sailor_telem_rx(sailor_telem_t *s, const uint8_t *p, size_t n, uint32_t now) {
    if(!s->accepted) return true;
    envelope_t e;
    if(!header_decode(p,n,&e)) return true;
    bool crc=le16(p+n-2)==sailor_telem_crc(p,n-2);
    if(!crc) {
        if(e.kind==2 || e.kind==5) return control_reply(s,p,&e,0x15);
        return true;
    }
    if(e.kind==6 || e.kind==0x15 || e.kind==5) {
        if(n!=e.header+3) return true;
        if(e.kind==5 && !control_reply(s,p,&e,6)) return false;
        s->have_rx=true; s->last_rx=now;
        if(e.kind!=5) receive_ack(s,&e,e.kind==0x15,now);
        return true;
    }
    if(e.kind!=2) return true;
    if(n<e.header+4 || (p[n-3]!=3 && p[n-3]!=0x17)) return control_reply(s,p,&e,0x15);
    if(!control_reply(s,p,&e,6)) return false;
    s->have_rx=true; s->last_rx=now;
    uint32_t key=packet_key(&e);
    if(duplicate(s,key)) return true;
    remember(s,key);
    /* ACK valid unsupported traffic to keep the upper window flowing. No
     * concatenation, partial telemetry decode or guessed event semantics. */
    if(e.fragment!=1 || p[n-3]!=3 || n<e.header+6 || e.plain<6) return true;
    uint16_t op=be16(p+e.header+1);
    if(decode(s,op,p+e.header+3,n-e.header-6,now)) {
        int pi=poll_index((uint16_t)(op-0x2000));
        if(pi>=0) s->polls[pi].pending=false;
    }
    return true;
}
static bool has_opcode(const sailor_telem_t *s, uint16_t op) {
    for(unsigned i=0;i<SAILOR_TELEM_UPPER_WINDOW;i++)
        if(s->upper[i].state!=SAILOR_TELEM_TX_FREE && s->upper[i].opcode==op) return true;
    return false;
}
static uint16_t next_sequence(sailor_telem_t *s) {
    uint16_t seq=s->binary_seq++;
    if(s->binary_seq>0xfffd || !s->binary_seq) s->binary_seq=1;
    /* 7FFF is used by the ENQ probe; avoid sharing a request identity with it. */
    if(s->binary_seq==0x7fff) s->binary_seq++;
    return seq;
}
void sailor_telem_tick(sailor_telem_t *s, uint32_t now) {
    if(!s->accepted || !s->send) return;
    /* Lower NDP owns delivery retries. Upper replies always precede polling. */
    while(s->control_count) {
        const sailor_telem_control_t *c=&s->controls[s->control_head];
        if(!s->send(s->user,c->data,c->len,0)) return;
        s->control_head=(uint8_t)((s->control_head+1)%SAILOR_TELEM_CONTROL_QUEUE);
        s->control_count--;
    }
    for(unsigned i=0;i<SAILOR_TELEM_UPPER_WINDOW;i++) {
        sailor_telem_tx_t *q=&s->upper[i];
        if(q->state==SAILOR_TELEM_TX_ACK && reached(now,q->due)) retry(s,q,now);
    }
    /* Rotate the starting group after every queued poll. Even under a slow
     * two-slot upper window, frequent UTC/GPS polls cannot starve network data. */
    unsigned start=s->poll_next;
    for(unsigned offset=0;offset<SAILOR_TELEM_POLLS;offset++) {
        unsigned pi=(start+offset)%SAILOR_TELEM_POLLS;
        sailor_telem_poll_t *p=&s->polls[pi];
        if(p->pending && now-p->requested>=RESPONSE_MS && !has_opcode(s,p->opcode)) p->pending=false;
        if(p->pending || !reached(now,p->due) || has_opcode(s,p->opcode)) continue;
        sailor_telem_tx_t *q=NULL;
        for(unsigned i=0;i<SAILOR_TELEM_UPPER_WINDOW;i++)
            if(s->upper[i].state==SAILOR_TELEM_TX_FREE) { q=&s->upper[i]; break; }
        if(!q) break;
        q->sequence=next_sequence(s); q->opcode=p->opcode;
        q->len=(uint8_t)normal(q->data,q->sequence,q->opcode,2);
        q->state=SAILOR_TELEM_TX_QUEUED; q->order=++s->tx_order;
        p->pending=true; p->requested=now; p->due=now+poll_intervals[pi];
        s->poll_next=(uint8_t)((pi+1)%SAILOR_TELEM_POLLS);
    }
    for(unsigned sent=0;sent<SAILOR_TELEM_UPPER_WINDOW;sent++) {
        sailor_telem_tx_t *q=NULL;
        for(unsigned i=0;i<SAILOR_TELEM_UPPER_WINDOW;i++) {
            sailor_telem_tx_t *candidate=&s->upper[i];
            if(candidate->state!=SAILOR_TELEM_TX_QUEUED && candidate->state!=SAILOR_TELEM_TX_RETRY) continue;
            if(!q || (int32_t)(candidate->order-q->order)<0) q=candidate;
        }
        if(!q) break;
        uint32_t cookie=++s->cookie_seq;
        if(!cookie) cookie=++s->cookie_seq;
        if(!s->send(s->user,q->data,q->len,cookie)) return;
        if(q->state==SAILOR_TELEM_TX_RETRY) q->retries++;
        q->cookie=cookie; q->sent=true; q->order=++s->tx_order;
        q->state=SAILOR_TELEM_TX_NDP;
    }
}
static uint32_t network_age(const sailor_telem_t *s, unsigned group, uint32_t now,
                             bool cached) {
    return cached || (s->network_seen&(1u<<group))?now-s->network_at[group]:UINT32_MAX;
}
static bool network_fresh(const sailor_telem_t *s, unsigned group, uint32_t age,
                           bool online, bool valid) {
    return online && valid && (s->network_seen&(1u<<group)) && age<=SAILOR_TELEM_NETWORK_MS;
}
void sailor_telem_snapshot(const sailor_telem_t *s, uint32_t now, protocol_antenna_status_t *o) {
    *o=s->status;
    o->online=s->accepted && s->have_rx && now-s->last_rx<=OFFLINE_MS;
    o->signal_age_ms=s->have_signal?now-s->signal_at:UINT32_MAX;
    o->position_age_ms=s->have_position?now-s->position_at:UINT32_MAX;
    o->signal_valid=o->online && s->have_signal && o->signal_age_ms<=SIGNAL_MS;
    o->position_valid=s->have_position;
    int64_t expected=(int64_t)s->clock_utc+(now-s->clock_at)/1000u;
    int64_t skew=expected-(int64_t)o->position_utc;
    bool clock=s->have_clock && now-s->clock_at<=POSITION_MS && skew>=-2 && skew<=5;
    o->position_fresh=o->online && s->have_position && s->position_report_valid &&
                      o->position_age_ms<=POSITION_MS && clock;
    o->registration_age_ms=network_age(s,NETWORK_REGISTRATION,now,o->registration_valid);
    o->protocol_age_ms=network_age(s,NETWORK_PROTOCOL,now,o->protocol_valid);
    o->channel_state_age_ms=network_age(s,NETWORK_CHANNEL_STATE,now,o->channel_state_valid);
    o->channel_age_ms=network_age(s,NETWORK_TDM,now,o->channel_valid);
    o->registration_fresh=network_fresh(s,NETWORK_REGISTRATION,o->registration_age_ms,o->online,o->registration_valid);
    o->protocol_fresh=network_fresh(s,NETWORK_PROTOCOL,o->protocol_age_ms,o->online,o->protocol_valid);
    o->channel_state_fresh=network_fresh(s,NETWORK_CHANNEL_STATE,o->channel_state_age_ms,o->online,o->channel_state_valid);
    o->channel_fresh=network_fresh(s,NETWORK_TDM,o->channel_age_ms,o->online,o->channel_valid);
    o->ocean_fresh=o->ocean_valid && o->registration_fresh;
}
