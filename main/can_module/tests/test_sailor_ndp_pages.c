/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#include "sailor_ndp_pages.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void test_receiver(void) {
    sailor_ndp_pages_t s={0}; sailor_ndp_message_t o;
    uint8_t p0[223]={0x5f,0x99,2,0x0a,0,0x3a,0xdb,1};
    uint8_t p1[223]={0x5f,0x99,2,0x0a,1};
    uint8_t p2[47]={0x5f,0x99,2,0x0a,2};
    /* Captured help geometry: total475 = 215+218+42; each page has a different FP SID. */
    memset(p0+8,65,215); memset(p1+5,66,218); memset(p2+5,67,42);
    assert(sailor_ndp_pages_feed(&s,1,1,p0,223,&o)==0);
    assert(sailor_ndp_pages_feed(&s,2,2,p1,223,&o)==-1); /* foreign source */
    /* Concurrent telemetry with the same series must not erase terminal pages. */
    uint8_t b1[]={0x5f,0x99,2,0x0a,0,0x1b,2,0,0x55,0xaa};
    assert(sailor_ndp_pages_feed(&s,1,2,b1,sizeof(b1),&o)==1);
    assert(sailor_ndp_pages_feed(&s,1,3,p1,223,&o)==0);
    assert(sailor_ndp_pages_feed(&s,1,4,p1,223,&o)==0); /* duplicate page */
    assert(sailor_ndp_pages_feed(&s,1,5,p2,47,&o)==1);
    assert(o.len==475 && o.channel==0x3a && o.series==0x0a);
    assert(o.data[214]==65 && o.data[215]==66 && o.data[432]==66 && o.data[433]==67);
    assert(sailor_ndp_pages_feed(&s,1,6,p2,47,&o)==-1);
    /* Full retransmission returned again for session ACK/dedup. */
    assert(sailor_ndp_pages_feed(&s,1,10,p0,223,&o)==0);
    assert(sailor_ndp_pages_feed(&s,1,11,p2,47,&o)==-1); /* gap */
    assert(sailor_ndp_pages_feed(&s,1,12,p1,223,&o)==-1);
    assert(sailor_ndp_pages_feed(&s,1,20,p0,223,&o)==0);
    assert(sailor_ndp_pages_feed(&s,1,1120,p1,223,&o)==-1); /* exactly1100ms: stale */
    p0[6]=255; p0[7]=255;
    assert(sailor_ndp_pages_feed(&s,1,2600,p0,223,&o)==-1);
    uint8_t a[]={0x5f,0x99,2,2,0,0x1b};
    assert(sailor_ndp_pages_feed(&s,1,2601,a,sizeof(a),&o)==1 && o.len==0);
    a[3]=8; assert(sailor_ndp_pages_feed(&s,1,2602,a,sizeof(a),&o)==-1);
    puts("sailor_ndp_pages: PASS (475-byte help, duplicates, gaps, timeout, malformed length)");
}


typedef struct {
    uint8_t wire[3][223];
    size_t length[3];
    unsigned calls, fail_at;
} encoded_t;
static bool collect(void *user, const uint8_t *wire, size_t len)
{
    encoded_t *e=user;
    assert(e->calls<3 && len<=223);
    memcpy(e->wire[e->calls],wire,len);
    e->length[e->calls]=len;
    ++e->calls;
    return e->calls!=e->fail_at;
}
static void test_encode_roundtrip(void)
{
    const uint8_t sig[]={0x5f,0x99,0x02};
    const size_t lengths[]={0,1,215,216,433,434,512};
    const size_t expected[][3]={{8,0,0},{9,0,0},{223,0,0},{223,6,0},
                               {223,223,0},{223,223,6},{223,223,84}};
    uint8_t input[513];
    for (unsigned i=0;i<sizeof(input);++i) input[i]=(uint8_t)(i*29u);
    for (unsigned test=0;test<sizeof(lengths)/sizeof(lengths[0]);++test) {
        encoded_t e={0}; sailor_ndp_pages_t rx={0}; sailor_ndp_message_t message;
        size_t n=lengths[test];
        assert(sailor_ndp_pages_encode(sig,0x0b,0x3a,n?input:NULL,n,collect,&e));
        unsigned pages=0;
        while (pages<3 && expected[test][pages]) ++pages;
        assert(e.calls==pages);
        assert(e.wire[0][5]==0x3a && e.wire[0][6]==(uint8_t)n && e.wire[0][7]==(uint8_t)(n>>8));
        for (unsigned i=0;i<pages;++i) {
            assert(e.length[i]==expected[test][i]);
            assert(!memcmp(e.wire[i],sig,3) && e.wire[i][3]==0x0b && e.wire[i][4]==i);
            int result=sailor_ndp_pages_feed(&rx,0x15ef0001,100+i,e.wire[i],e.length[i],&message);
            assert(result==(i+1==pages ? 1 : 0));
        }
        assert(message.len==n && message.series==0x0b && message.channel==0x3a);
        assert(!memcmp(message.sig,sig,3));
        if(n) assert(!memcmp(message.data,input,n));
    }
    /* The475-byte terminal help capture spans215+218+42 application bytes. */
    memset(input,'A',215); memset(input+215,'B',218); memset(input+433,'C',42);
    encoded_t help={0};
    assert(sailor_ndp_pages_encode(sig,0x0a,0x3a,input,475,collect,&help));
    assert(help.calls==3 && help.length[0]==223 && help.length[1]==223 && help.length[2]==47);
    const uint8_t header[]={0x5f,0x99,2,0x0a,0,0x3a,0xdb,1};
    assert(!memcmp(help.wire[0],header,sizeof(header)));
    assert(help.wire[0][222]=='A' && help.wire[1][5]=='B' && help.wire[1][222]=='B');
    assert(help.wire[2][4]==2 && help.wire[2][5]=='C' && help.wire[2][46]=='C');

    encoded_t e={0};
    assert(!sailor_ndp_pages_encode(sig,8,0x3a,input,513,collect,&e) && e.calls==0);
    assert(!sailor_ndp_pages_encode(sig,8,0x3a,NULL,1,collect,&e) && e.calls==0);
    assert(!sailor_ndp_pages_encode(sig,0x10,0x3a,input,216,collect,&e) && e.calls==0);
    const uint8_t discovery[]={0x5f,0x99,3};
    assert(!sailor_ndp_pages_encode(discovery,8,0,input,216,collect,&e) && e.calls==0);
    /* Failure on any lower page stops immediately, without emitting later pages. */
    for (unsigned fail=1;fail<=3;++fail) {
        memset(&e,0,sizeof(e)); e.fail_at=fail;
        assert(!sailor_ndp_pages_encode(sig,8,0x3a,input,512,collect,&e));
        assert(e.calls==fail);
    }
    puts("sailor_ndp_pages encode: PASS (0/1/215/216/433/434/512,475-byte geometry, callback failure)");
}
static void test_timeout_and_capacity(void)
{
    const uint8_t sig[]={0x5f,0x99,2};
    uint8_t input[475]; memset(input,0x55,sizeof(input));
    encoded_t e={0};
    assert(sailor_ndp_pages_encode(sig,8,0x3a,input,sizeof(input),collect,&e));
    sailor_ndp_pages_t s={0}; sailor_ndp_message_t out;
    assert(sailor_ndp_pages_feed(&s,1,100,e.wire[0],e.length[0],&out)==0);
    assert(sailor_ndp_pages_feed(&s,1,1199,e.wire[1],e.length[1],&out)==0); /*1099ms valid*/
    assert(sailor_ndp_pages_feed(&s,1,2299,e.wire[2],e.length[2],&out)==-1); /*1100ms stale*/
    memset(&s,0,sizeof(s));
    uint32_t start=UINT32_MAX-500u;
    assert(sailor_ndp_pages_feed(&s,1,start,e.wire[0],e.length[0],&out)==0);
    assert(sailor_ndp_pages_feed(&s,1,start+1099u,e.wire[1],e.length[1],&out)==0);
    assert(sailor_ndp_pages_feed(&s,1,start+1100u,e.wire[2],e.length[2],&out)==1);
    assert(out.len==475 && !memcmp(out.data,input,475));
    memset(&s,0,sizeof(s));
    for(unsigned i=0;i<4;++i)
        assert(sailor_ndp_pages_feed(&s,i+1,100,e.wire[0],e.length[0],&out)==0);
    assert(sailor_ndp_pages_feed(&s,5,100,e.wire[0],e.length[0],&out)==-1);
    for(unsigned i=0;i<4;++i) {
        assert(sailor_ndp_pages_feed(&s,i+1,101,e.wire[1],e.length[1],&out)==0);
        assert(sailor_ndp_pages_feed(&s,i+1,102,e.wire[2],e.length[2],&out)==1);
        assert(out.len==475 && !memcmp(out.data,input,475));
    }
    /* Continuation has no channel: reject two partial streams sharing route+series. */
    assert(sailor_ndp_pages_feed(&s,1,200,e.wire[0],e.length[0],&out)==0);
    e.wire[0][5]=0x1b;
    assert(sailor_ndp_pages_feed(&s,1,201,e.wire[0],e.length[0],&out)==-1);
    assert(sailor_ndp_pages_feed(&s,1,202,e.wire[1],e.length[1],&out)==-1);
    puts("sailor_ndp_pages bounds: PASS (1100ms, clock wrap, four slots, ambiguous channel)");
}
int main(void)
{
    test_receiver();
    test_encode_roundtrip();
    test_timeout_and_capacity();
    return 0;
}
