/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#include "sailor_ndp_pages.h"
#include <string.h>
int sailor_ndp_pages_feed(sailor_ndp_pages_t *s,uint32_t key,uint32_t now,
                         const uint8_t *w,size_t n,sailor_ndp_message_t *o) {
    if(!s || !w || !o || n<5 || n>223 || w[0]!=0x5f || w[1]!=0x99 ||
       (w[2]!=2 && w[2]!=3)) return -1;
    memset(o,0,sizeof(*o)); memcpy(o->sig,w,3); o->series=w[3];
    sailor_ndp_slot_t *slot=NULL,*free_slot=NULL;
    for(unsigned i=0;i<SAILOR_NDP_SLOTS;i++) {
        sailor_ndp_slot_t *p=&s->slots[i];
        if(p->active && now-p->at>=1100u) p->active=false;
        if(p->active && p->key==key && p->series==w[3]) slot=p;
        if(!p->active && !free_slot) free_slot=p;
    }
    if(w[4]==0) {
        if(n<6) return -1;
        o->channel=w[5];
        if(n==6 && w[2]==2 && w[3]<8) return 1;
        if(n<8) return -1;
        uint16_t total=(uint16_t)(w[6]|(w[7]<<8));
        size_t used=n-8;
        if(total>SAILOR_NDP_CAPACITY || used>total) return -1;
        if(total==used) {
            if(slot && slot->channel==w[5]) slot->active=false;
            o->data=w+8; o->len=total; return 1;
        }
        /* Only DATA blocks may span pages; a partial first page is full FP. */
        if(w[2]!=2 || w[3]<8 || w[3]>15 || n!=223) return -1;
        /* Continuations have no channel byte. Two concurrent paged messages
         * with the same route/series are ambiguous: discard both, never mix.
         * Ordinary complete packets on other channels leave the stream intact. */
        if(slot && slot->channel!=w[5]) { slot->active=false; return -1; }
        if(!slot) slot=free_slot;
        if(!slot) return -1;
        slot->active=true; slot->key=key; slot->series=w[3]; slot->channel=w[5];
        slot->at=now; slot->total=total; slot->used=(uint16_t)used; slot->next_page=1;
        memcpy(slot->data,w+8,used); return 0;
    }
    if(!slot || w[2]!=2 || n<=5) return -1;
    if(w[4]!=slot->next_page) {
        /* Duplicate last page is harmless only if its bytes match. */
        if(w[4]+1==slot->next_page && n-5<=slot->used &&
           !memcmp(slot->data+slot->used-(n-5),w+5,n-5)) return 0;
        slot->active=false; return -1;
    }
    if(n-5>(size_t)(slot->total-slot->used) ||
       (n<223 && slot->used+n-5!=slot->total)) {
        slot->active=false; return -1;
    }
    memcpy(slot->data+slot->used,w+5,n-5);
    slot->used+=(uint16_t)(n-5); slot->next_page++; slot->at=now;
    if(slot->used<slot->total) return 0;
    slot->active=false; o->channel=slot->channel;
    o->data=slot->data; o->len=slot->total; return 1;
}

bool sailor_ndp_pages_encode(const uint8_t sig[3],uint8_t series,uint8_t channel,
    const uint8_t *data,size_t len,sailor_ndp_page_send_fn send,void *user) {
    if(!sig || !send || (len && !data) || len>SAILOR_NDP_CAPACITY ||
       sig[0]!=0x5f || sig[1]!=0x99 || (sig[2]!=2 && sig[2]!=3)) return false;
    if(len>215 && (sig[2]!=2 || series<8 || series>15)) return false;
    uint8_t wire[223],page=0;
    size_t off=0;
    do {
        memcpy(wire,sig,3); wire[3]=series; wire[4]=page;
        size_t header=page?5:8;
        if(!page) { wire[5]=channel; wire[6]=(uint8_t)len; wire[7]=(uint8_t)(len>>8); }
        size_t take=len-off;
        if(take>sizeof(wire)-header) take=sizeof(wire)-header;
        if(take) memcpy(wire+header,data+off,take);
        if(!send(user,wire,header+take)) return false;
        off+=take; page++;
    } while(off<len);
    return true;
}
