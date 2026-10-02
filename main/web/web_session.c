#include "web/web_session.h"
#include <string.h>
void web_session_init(web_session_t *s) {
    memset(s, 0, sizeof(*s)); s->fd = -1; s->generation = 1;
}
void web_session_revoke(web_session_t *s) {
    s->generation++; if (!s->generation) s->generation = 1;
    s->state = WEB_SESSION_LOCAL; s->fd = -1;
    s->token[0] = 0; s->last_seen_ms = 0;
}
bool web_session_claim(web_session_t *s, int fd, const char *token,
                       const char *fresh_token, uint64_t now_ms) {
    if (fd < 0 || !token) return false;
    if (s->state == WEB_SESSION_LOCAL) {
        /* An old tab may reconnect, but may never acquire with a revoked token. */
        if (*token || !fresh_token || strlen(fresh_token) != WEB_SESSION_TOKEN_LENGTH) return false;
        memcpy(s->token, fresh_token, sizeof(s->token));
        s->generation++; if (!s->generation) s->generation = 1;
    } else {
        if (!*token || strcmp(s->token, token) != 0) return false;
        if (s->state == WEB_SESSION_ACTIVE && s->fd >= 0 && s->fd != fd) return false;
        if (now_ms - s->last_seen_ms >= WEB_SESSION_TIMEOUT_MS) return false;
    }
    s->state = WEB_SESSION_ACTIVE; s->fd = fd; s->last_seen_ms = now_ms;
    return true;
}
bool web_session_authorized(const web_session_t *s, int fd, const char *token, uint32_t generation) {
    return s->state == WEB_SESSION_ACTIVE && s->fd == fd && token && *token &&
        generation == s->generation && strcmp(s->token, token) == 0;
}
void web_session_heartbeat(web_session_t *s, uint64_t now_ms) { s->last_seen_ms = now_ms; }
void web_session_disconnect(web_session_t *s, int fd) {
    if (s->state != WEB_SESSION_LOCAL && s->fd == fd) {
        s->state = WEB_SESSION_RECONNECT; s->fd = -1;
    }
}
bool web_session_tick(web_session_t *s, uint64_t now_ms) {
    if (s->state == WEB_SESSION_LOCAL) return false;
    if (now_ms - s->last_seen_ms >= WEB_SESSION_TIMEOUT_MS) { web_session_revoke(s); return true; }
    if (s->state == WEB_SESSION_ACTIVE && now_ms - s->last_seen_ms >= WEB_SESSION_MISSED_MS)
        s->state = WEB_SESSION_RECONNECT;
    return false;
}

bool web_session_claim_queued(web_session_t *s, int fd, const char *token,
    const char *fresh_token, uint32_t submitted_generation,
    bool submitted_while_local, uint64_t now_ms) {
    if(submitted_generation != s->generation) return false;
    if(s->state == WEB_SESSION_LOCAL && !submitted_while_local) return false;
    return web_session_claim(s,fd,token,fresh_token,now_ms);
}
