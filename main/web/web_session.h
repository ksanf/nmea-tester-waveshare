#pragma once
#include <stdbool.h>
#include <stdint.h>
#define WEB_SESSION_TOKEN_LENGTH 32
#define WEB_SESSION_TIMEOUT_MS 60000u
#define WEB_SESSION_MISSED_MS 15000u
typedef enum { WEB_SESSION_LOCAL, WEB_SESSION_ACTIVE, WEB_SESSION_RECONNECT } web_session_state_t;
typedef struct {
    web_session_state_t state;
    uint32_t generation;
    uint64_t last_seen_ms;
    int fd;
    char token[WEB_SESSION_TOKEN_LENGTH + 1];
} web_session_t;
void web_session_init(web_session_t *s);
bool web_session_claim(web_session_t *s, int fd, const char *token,
                       const char *fresh_token, uint64_t now_ms);
bool web_session_authorized(const web_session_t *s, int fd, const char *token, uint32_t generation);
void web_session_heartbeat(web_session_t *s, uint64_t now_ms);
void web_session_disconnect(web_session_t *s, int fd);
bool web_session_tick(web_session_t *s, uint64_t now_ms);
void web_session_revoke(web_session_t *s);

/* A queued fresh claim may not outlive the ownership state it was submitted in. */
bool web_session_claim_queued(web_session_t *s, int fd, const char *token,
    const char *fresh_token, uint32_t submitted_generation,
    bool submitted_while_local, uint64_t now_ms);
