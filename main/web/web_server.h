#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
esp_err_t web_server_start(void);
/* Copies into a bounded outgoing queue. Never blocks a transport task. */
bool web_server_send(int id, const char *text, size_t len);
void web_server_close(int id);
/* Logical connection identity; closing or disconnected peers are not alive. */
bool web_server_peer_alive(int id);
