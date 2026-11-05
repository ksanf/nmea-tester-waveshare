#include "update/firmware_info.h"
#include "update/firmware_update.h"
#include "update_format.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include <stdio.h>

esp_err_t firmware_info_http_handler(httpd_req_t *req)
{
    char out[400];
    const esp_partition_t *running = esp_ota_get_running_partition();
    int length = snprintf(out, sizeof(out),
        "product=nmea-tester\nboard=%s\nversion=%s\nlayout=%u\n"
        "settings=%u\nslot=%s\nmax_size=%u\nbusy=%u\nupdate_mode=%u\n",
        NMEA_BOARD_ID, esp_app_get_description()->version, NMEA_LAYOUT_VERSION,
        NMEA_SETTINGS_SCHEMA, running ? running->label : "unknown", NMEA_APP_SIZE,
        (unsigned)firmware_update_busy(), (unsigned)firmware_update_active());
    if (length < 0 || (size_t)length >= sizeof(out)) return ESP_FAIL;
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, out);
}
