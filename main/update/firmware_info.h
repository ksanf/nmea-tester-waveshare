#pragma once
#include "esp_http_server.h"

/* Read-only identity: no mode changes, ownership claims or LVGL access.
 * Shared by the normal HTTP server and the explicitly armed update server. */
esp_err_t firmware_info_http_handler(httpd_req_t *req);
