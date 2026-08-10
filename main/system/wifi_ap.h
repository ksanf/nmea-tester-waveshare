/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef WIFI_AP_H
#define WIFI_AP_H

#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_ap_init(void);
esp_err_t wifi_ap_start(void);
esp_err_t wifi_ap_stop(void);
bool wifi_ap_is_enabled(void);
bool wifi_ap_get_ip(char *buf, size_t buf_sz);

#ifdef __cplusplus
}
#endif

#endif
