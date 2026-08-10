/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   WiFi manager: AP/STA mode, NVS persistence, scan, IP.
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>
#include <esp_wifi_types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_MODE_AP = 0,
    WIFI_MGR_MODE_STA = 1,
} wifi_mgr_mode_t;

typedef enum {
    WIFI_MGR_DISCONNECTED = 0,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
} wifi_mgr_sta_state_t;

/* ── Lifecycle ─────────────────────────────────────────────────── */
esp_err_t wifi_manager_init(void);           /* read NVS; radio uses compile-time default */
esp_err_t wifi_manager_start(void);          /* explicitly start saved AP/STA mode */
esp_err_t wifi_manager_deinit(void);         /* stop radio, keep manager ready */

/* ── Mode ──────────────────────────────────────────────────────── */
wifi_mgr_mode_t wifi_manager_get_mode(void);
esp_err_t        wifi_manager_set_mode(wifi_mgr_mode_t mode);  /* restart only if already on */

/* ── AP config ─────────────────────────────────────────────────── */
esp_err_t wifi_manager_ap_set(const char *ssid, const char *pass, const char *ip);
void      wifi_manager_ap_get(char *ssid, size_t ssid_sz,
                              char *pass, size_t pass_sz,
                              char *ip,   size_t ip_sz);
/* ── STA config ────────────────────────────────────────────────── */
esp_err_t wifi_manager_sta_set(const char *ssid, const char *pass);
void      wifi_manager_sta_get(char *ssid, size_t ssid_sz,
                               char *pass, size_t pass_sz);

/* ── Scan ──────────────────────────────────────────────────────── */
uint16_t       wifi_manager_scan(wifi_ap_record_t *list, uint16_t max);
wifi_mgr_sta_state_t wifi_manager_sta_state(void);
esp_err_t       wifi_manager_sta_connect(void);  /* reconnect with saved creds */
esp_err_t       wifi_manager_sta_disconnect(void);

/* ── Status ────────────────────────────────────────────────────── */
bool wifi_manager_is_started(void);
bool wifi_manager_get_ip_str(char *buf, size_t buf_sz);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
