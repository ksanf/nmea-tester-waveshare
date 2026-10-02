#include <assert.h>
#include <stdio.h>
#include "web/web_session.h"
int main(void) {
    const char *token = "0123456789abcdef0123456789abcdef";
    web_session_t s; web_session_init(&s);
    assert(!web_session_claim(&s, 4, token, token, 0));
    assert(web_session_claim(&s, 4, "", token, 100));
    uint32_t generation = s.generation;
    assert(web_session_authorized(&s, 4, token, generation));
    assert(!web_session_authorized(&s, 5, token, generation));
    assert(!web_session_claim(&s, 5, token, token, 200));
    web_session_disconnect(&s, 5);
    assert(s.state == WEB_SESSION_ACTIVE);
    web_session_disconnect(&s, 4);
    assert(s.state == WEB_SESSION_RECONNECT);
    assert(web_session_claim(&s, 6, token, token, 30000));
    assert(s.generation == generation);
    assert(!web_session_tick(&s, 45000));
    assert(s.state == WEB_SESSION_RECONNECT);
    assert(!web_session_tick(&s, 89999));
    assert(web_session_tick(&s, 90000));
    assert(s.state == WEB_SESSION_LOCAL);
    assert(!web_session_claim(&s, 7, token, token, 90001));
    assert(!web_session_authorized(&s, 6, token, generation));
    assert(web_session_claim(&s, 7, "", token, 90002));
    generation = s.generation;
    web_session_revoke(&s); web_session_revoke(&s);
    assert(!web_session_authorized(&s, 7, token, generation));
    assert(!web_session_claim(&s, 7, token, token, 90003));
    /* A claim queued before revoke, or during the LCD button's handoff,
       must not retake ownership after the screen has returned to LOCAL. */
    assert(!web_session_claim_queued(&s,9,"",token,generation,true,100000));
    assert(!web_session_claim_queued(&s,9,"",token,s.generation,false,100000));
    assert(web_session_claim_queued(&s,9,"",token,s.generation,true,100000));
    generation=s.generation;
    web_session_disconnect(&s,9);
    assert(web_session_claim_queued(&s,10,token,token,generation,false,100001));
    web_session_disconnect(&s,9); /* delayed close of the old connection */
    assert(web_session_authorized(&s,10,token,generation));
    puts("Web session tests passed (exclusive owner, reconnect, expiry, revoke)");
    return 0;
}
