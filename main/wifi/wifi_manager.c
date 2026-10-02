/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "wifi/wifi_manager.h"
#include "config/config_nmea_tester.h"
#include "config/memory_config.h"

#include <string.h>
#include <stdio.h>
#include <esp_event.h>
#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_WIFI_AP
#include "config_logs.h"
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_wifi_default.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>

static const char *TAG = "wifi_mgr";
static const char *NVS_NS = "wifi";

#define NVS_KEY_MODE     "mode"
#define NVS_KEY_AP_SSID  "ap_ssid"
#define NVS_KEY_AP_PASS  "ap_pass"
#define NVS_KEY_AP_IP    "ap_ip"
#define NVS_KEY_STA_SSID "sta_ssid"
#define NVS_KEY_STA_PASS "sta_pass"
#define NVS_KEY_AP_CONFIG "ap_config"
#define NVS_KEY_STA_CONFIG "sta_config"
#define NVS_CONFIG_VERSION 1u

/* One NVS item per group prevents a partially updated SSID/password/IP pair.
 * Character arrays give the persisted layout no compiler-dependent padding. */
typedef struct {
    uint8_t version;
    char ssid[33], pass[65], ip[16];
} wifi_ap_saved_t;
typedef struct {
    uint8_t version;
    char ssid[33], pass[65];
} wifi_sta_saved_t;

#define AP_SSID_DEFAULT  WIFI_AP_SSID
#define AP_PASS_DEFAULT  WIFI_AP_PASSWORD
#define AP_IP_DEFAULT    "192.168.4.1"
#define AP_CHANNEL       WIFI_AP_CHANNEL
#define AP_MAX_CONN      WIFI_AP_MAX_CONNECTIONS
#define AP_INACTIVE_SEC  WIFI_AP_INACTIVE_TIMEOUT_SEC
#define STA_MAX_RETRY    3

static bool s_inited = false;
static bool s_wifi_started = false;
static bool s_wifi_evt_registered = false;
static bool s_ip_evt_registered = false;
/* Power-on enable state is intentional and never restored from NVS. */
static bool s_enabled = WIFI_AP_DEFAULT_ENABLED;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;

static wifi_mgr_mode_t s_mode = WIFI_MGR_MODE_AP;
static wifi_mgr_sta_state_t s_sta_state = WIFI_MGR_DISCONNECTED;
static int s_sta_retry = 0;
static bool s_user_disconnect = false;
static bool s_scanning = false;
static bool s_op_busy = false;
static portMUX_TYPE s_op_mux = portMUX_INITIALIZER_UNLOCKED;

static char s_ap_ssid[33];
static char s_ap_pass[65];
static char s_ap_ip[16];
static char s_sta_ssid[33];
static char s_sta_pass[65];

/* ── Forward ─────────────────────────────────────────────────── */
static esp_err_t apply_ap_(void);
static esp_err_t apply_sta_(void);
static esp_err_t parse_ip_(const char *str, esp_netif_ip_info_t *ip);

static esp_err_t stop_wifi_(void)
{
    esp_err_t err;

    if (!s_wifi_started) return ESP_OK;
    err = esp_wifi_stop();
    if (err == ESP_OK || err == ESP_ERR_WIFI_NOT_STARTED) {
        s_wifi_started = false;
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WiFi stop failed: %s", esp_err_to_name(err));
    return err;
}

static bool begin_op_(const char *op)
{
    bool busy;

    portENTER_CRITICAL(&s_op_mux);
    busy = s_op_busy;
    if (!busy) {
        s_op_busy = true;
    }
    portEXIT_CRITICAL(&s_op_mux);

    if (busy) {
        ESP_LOGW(TAG, "%s ignored: WiFi manager is busy", op);
        return false;
    }
    return true;
}

static void end_op_(void)
{
    portENTER_CRITICAL(&s_op_mux);
    s_op_busy = false;
    portEXIT_CRITICAL(&s_op_mux);
}

/* ── NVS ──────────────────────────────────────────────────────── */
static bool saved_credentials_valid_(const char *ssid, const char *pass)
{
    const size_t ssid_len = strnlen(ssid, 33);
    const size_t pass_len = strnlen(pass, 65);
    return ssid_len > 0 && ssid_len <= 32 &&
           (pass_len == 0 || (pass_len >= 8 && pass_len <= 63));
}

static void nvs_load_(void)
{
    bool ap_password_saved = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) goto defaults;

    uint8_t m = 0;
    size_t sz = sizeof(m);
    if (nvs_get_blob(h, NVS_KEY_MODE, &m, &sz) == ESP_OK && sz == 1)
        s_mode = (m <= WIFI_MGR_MODE_STA) ? (wifi_mgr_mode_t)m : WIFI_MGR_MODE_AP;

    /* Read legacy keys first so an upgrade preserves existing settings. They
     * remain untouched when a new configuration group is saved. */
    sz = sizeof(s_ap_ssid); nvs_get_str(h, NVS_KEY_AP_SSID, s_ap_ssid, &sz);
    sz = sizeof(s_ap_pass); ap_password_saved = nvs_get_str(h, NVS_KEY_AP_PASS, s_ap_pass, &sz) == ESP_OK;
    sz = sizeof(s_ap_ip);   nvs_get_str(h, NVS_KEY_AP_IP,   s_ap_ip,   &sz);
    sz = sizeof(s_sta_ssid); nvs_get_str(h, NVS_KEY_STA_SSID, s_sta_ssid, &sz);
    sz = sizeof(s_sta_pass); nvs_get_str(h, NVS_KEY_STA_PASS, s_sta_pass, &sz);

    wifi_ap_saved_t ap = {0};
    esp_netif_ip_info_t parsed_ip;
    sz = sizeof(ap);
    if (nvs_get_blob(h, NVS_KEY_AP_CONFIG, &ap, &sz) == ESP_OK &&
        sz == sizeof(ap) && ap.version == NVS_CONFIG_VERSION &&
        saved_credentials_valid_(ap.ssid, ap.pass) &&
        memchr(ap.ip, 0, sizeof(ap.ip)) && parse_ip_(ap.ip, &parsed_ip) == ESP_OK) {
        memcpy(s_ap_ssid, ap.ssid, sizeof(s_ap_ssid));
        memcpy(s_ap_pass, ap.pass, sizeof(s_ap_pass));
        memcpy(s_ap_ip, ap.ip, sizeof(s_ap_ip));
        ap_password_saved = true;
    }
    wifi_sta_saved_t sta = {0};
    sz = sizeof(sta);
    if (nvs_get_blob(h, NVS_KEY_STA_CONFIG, &sta, &sz) == ESP_OK &&
        sz == sizeof(sta) && sta.version == NVS_CONFIG_VERSION &&
        saved_credentials_valid_(sta.ssid, sta.pass)) {
        memcpy(s_sta_ssid, sta.ssid, sizeof(s_sta_ssid));
        memcpy(s_sta_pass, sta.pass, sizeof(s_sta_pass));
    }
    nvs_close(h);

defaults:
    if (!s_ap_ssid[0]) strlcpy(s_ap_ssid, AP_SSID_DEFAULT, sizeof(s_ap_ssid));
    /* An explicitly saved empty password means an open AP, including on reboot. */
    if (!ap_password_saved) strlcpy(s_ap_pass, AP_PASS_DEFAULT, sizeof(s_ap_pass));
    if (!s_ap_ip[0])   strlcpy(s_ap_ip,   AP_IP_DEFAULT,   sizeof(s_ap_ip));
}

static esp_err_t nvs_save_blob_(const char *key, const void *data, size_t size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed for %s: %s", key, esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, key, data, size);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS save failed for %s: %s", key, esp_err_to_name(err));
    }
    /* NVS may have persisted a set before a commit failure. Report that error;
     * never claim a successful save or apply it to the active radio/RAM. */
    return err;
}

/* ── IP parse ─────────────────────────────────────────────────── */
static esp_err_t parse_ip_(const char *str, esp_netif_ip_info_t *ip)
{
    unsigned a, b, c, d;
    int consumed = 0;
    if (!str || !ip ||
        sscanf(str, "%u.%u.%u.%u%n", &a, &b, &c, &d, &consumed) != 4 ||
        str[consumed] != '\0') {
        return ESP_FAIL;
    }
    if (a == 0 || a > 223 || b > 255 || c > 255 || d == 0 || d > 254) {
        return ESP_FAIL;
    }
    ip->ip.addr = ((uint32_t)a) | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
    ip->gw.addr = ip->ip.addr;
    ip->netmask.addr = 0x00FFFFFF;
    return ESP_OK;
}

/* ── Events ───────────────────────────────────────────────────── */
static void wifi_evt_(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base != WIFI_EVENT) return;
    switch (id) {
    case WIFI_EVENT_AP_START:       ESP_LOGI(TAG, "AP started: %s", s_ap_ssid); break;
    case WIFI_EVENT_AP_STACONNECTED: ESP_LOGI(TAG, "AP client connected"); break;
    case WIFI_EVENT_AP_STADISCONNECTED: ESP_LOGI(TAG, "AP client disconnected"); break;
    case WIFI_EVENT_STA_START:
        if (!s_scanning && !s_user_disconnect && s_mode == WIFI_MGR_MODE_STA && s_sta_ssid[0]) {
            esp_wifi_connect();
        }
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        s_sta_state = WIFI_MGR_DISCONNECTED;
        if (!s_scanning && !s_user_disconnect && s_sta_ssid[0] &&
            s_sta_retry < STA_MAX_RETRY && s_mode == WIFI_MGR_MODE_STA) {
            s_sta_retry++; s_sta_state = WIFI_MGR_CONNECTING;
            esp_wifi_connect();
        }
        break;
    }
}

static void ip_evt_(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_sta_state = WIFI_MGR_CONNECTED;
        s_sta_retry = 0;
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&ev->ip_info.ip));

        /* Apply the latency optimization only after DHCP has completed.
         * Failure must never tear down an otherwise healthy connection. */
        esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ps_err != ESP_OK) {
            ESP_LOGW(TAG, "Disabling STA power save failed: %s",
                     esp_err_to_name(ps_err));
        }
    }
}

/* ── Apply ────────────────────────────────────────────────────── */
static esp_err_t apply_ap_(void)
{
    wifi_config_t cfg = {0};
    wifi_country_t country = {.cc="US",.schan=1,.nchan=11,.max_tx_power=20,.policy=WIFI_COUNTRY_POLICY_MANUAL};
    esp_netif_ip_info_t ip;
    esp_err_t err;

    cfg.ap.ssid_len = strlen(s_ap_ssid);
    memcpy(cfg.ap.ssid, s_ap_ssid, cfg.ap.ssid_len);
    strlcpy((char*)cfg.ap.password, s_ap_pass, sizeof(cfg.ap.password));
    cfg.ap.max_connection = AP_MAX_CONN;
    cfg.ap.authmode = (strlen(s_ap_pass) < 8) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    cfg.ap.channel = AP_CHANNEL;
    cfg.ap.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_country(&country);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_protocol(WIFI_IF_AP, WIFI_AP_B_ONLY ? WIFI_PROTOCOL_11B
                                                           : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G));
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) return err;

    err = parse_ip_(s_ap_ip, &ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Invalid AP IP: %s", s_ap_ip);
        return ESP_ERR_INVALID_ARG;
    }

    err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "DHCP server stop failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_netif_set_ip_info(s_ap_netif, &ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP IP setup failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_netif_dhcps_start(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGE(TAG, "DHCP server start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_wifi_started = true;
    (void)esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    (void)esp_wifi_set_inactive_time(WIFI_IF_AP, AP_INACTIVE_SEC);

    ESP_LOGI(TAG, "AP: %s ch=%d", s_ap_ssid, AP_CHANNEL);
    return ESP_OK;
}

static esp_err_t apply_sta_(void)
{
    wifi_config_t cfg = {0};
    esp_err_t err;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    if (s_sta_ssid[0]) {
        memcpy(cfg.sta.ssid, s_sta_ssid, strlen(s_sta_ssid));
        strlcpy((char*)cfg.sta.password, s_sta_pass, sizeof(cfg.sta.password));
        cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        cfg.sta.pmf_cfg.capable = true;
        cfg.sta.pmf_cfg.required = false;

        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (err != ESP_OK) return err;
        s_sta_state = s_user_disconnect ? WIFI_MGR_DISCONNECTED : WIFI_MGR_CONNECTING;
    } else {
        s_sta_state = WIFI_MGR_DISCONNECTED;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_wifi_started = true;
    s_sta_retry = 0;
    if (s_sta_ssid[0] && !s_user_disconnect) {
        ESP_LOGI(TAG, "STA connecting %s", s_sta_ssid);
    } else if (s_sta_ssid[0]) {
        ESP_LOGI(TAG, "STA ready, user disconnected from %s", s_sta_ssid);
    } else {
        ESP_LOGI(TAG, "STA scan-ready, no saved SSID");
    }
    return ESP_OK;
}

/* ── Public API ───────────────────────────────────────────────── */

esp_err_t wifi_manager_init(void)
{
    if (s_inited) return ESP_OK;

    nvs_load_();

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_ap_netif || !s_sta_netif) return ESP_ERR_NO_MEM;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    /* ESP_ERR_INVALID_STATE = already inited, that's fine */
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;

    if (!s_wifi_evt_registered) {
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt_, NULL);
        if (err != ESP_OK) return err;
        s_wifi_evt_registered = true;
    }
    if (!s_ip_evt_registered) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_evt_, NULL);
        if (err != ESP_OK) return err;
        s_ip_evt_registered = true;
    }

    s_inited = true;

    if (!s_enabled) {
        ESP_LOGI(TAG, "WiFi disabled");
        return ESP_OK;
    }

    if (s_mode == WIFI_MGR_MODE_AP) return apply_ap_();
    return apply_sta_();
}

esp_err_t wifi_manager_deinit(void)
{
    if (!begin_op_("deinit")) return ESP_ERR_INVALID_STATE;
    esp_err_t err = stop_wifi_();
    if (err != ESP_OK) {
        end_op_();
        return err;
    }
    s_enabled = false;
    s_sta_state = WIFI_MGR_DISCONNECTED;
    end_op_();
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    esp_err_t err;

    if (!s_inited) {
        err = wifi_manager_init();
        if (err != ESP_OK) return err;
    }
    if (s_wifi_started) {
        s_enabled = true;
        return ESP_OK;
    }
    if (!begin_op_("start")) return ESP_ERR_INVALID_STATE;

    s_enabled = true;
    s_user_disconnect = false;
    err = (s_mode == WIFI_MGR_MODE_AP) ? apply_ap_() : apply_sta_();
    if (err != ESP_OK) {
        (void)stop_wifi_();
        s_enabled = false;
        s_sta_state = WIFI_MGR_DISCONNECTED;
    }
    end_op_();
    return err;
}

wifi_mgr_mode_t wifi_manager_get_mode(void) { return s_mode; }

esp_err_t wifi_manager_set_mode(wifi_mgr_mode_t mode)
{
    if (mode != WIFI_MGR_MODE_AP && mode != WIFI_MGR_MODE_STA) return ESP_ERR_INVALID_ARG;
    if (!s_inited) {
        esp_err_t err = wifi_manager_init();
        if (err != ESP_OK) return err;
    }
    if (!begin_op_("set_mode")) return ESP_ERR_INVALID_STATE;
    const uint8_t saved_mode = (uint8_t)mode;
    esp_err_t err = nvs_save_blob_(NVS_KEY_MODE, &saved_mode, sizeof(saved_mode));
    if (err != ESP_OK) {
        end_op_();
        return err;
    }
    const bool restart = s_wifi_started;
    if (restart) {
        err = stop_wifi_();
        if (err != ESP_OK) {
            end_op_();
            return err;
        }
    }
    s_mode = mode;
    s_sta_state = WIFI_MGR_DISCONNECTED;
    s_user_disconnect = false;
    if (restart) {
        err = (mode == WIFI_MGR_MODE_AP) ? apply_ap_() : apply_sta_();
        if (err != ESP_OK) {
            s_enabled = false;
            s_sta_state = WIFI_MGR_DISCONNECTED;
        }
    }
    end_op_();
    return err;
}

esp_err_t wifi_manager_ap_set(const char *ssid, const char *pass, const char *ip)
{
    esp_netif_ip_info_t parsed_ip;
    const size_t ssid_len = ssid ? strnlen(ssid, sizeof(s_ap_ssid)) : 0;
    const size_t pass_len = pass ? strnlen(pass, sizeof(s_ap_pass)) : 0;

    if (ssid_len == 0 || ssid_len > sizeof(((wifi_config_t *)0)->ap.ssid)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass && (pass_len >= sizeof(((wifi_config_t *)0)->ap.password) ||
                 (pass_len > 0 && pass_len < 8))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ip && ip[0] && parse_ip_(ip, &parsed_ip) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!begin_op_("ap_set")) return ESP_ERR_INVALID_STATE;
    wifi_ap_saved_t next = {.version = NVS_CONFIG_VERSION};
    strlcpy(next.ssid, ssid, sizeof(next.ssid));
    strlcpy(next.pass, pass ? pass : s_ap_pass, sizeof(next.pass));
    strlcpy(next.ip, (ip && ip[0]) ? ip : s_ap_ip, sizeof(next.ip));
    esp_err_t err = nvs_save_blob_(NVS_KEY_AP_CONFIG, &next, sizeof(next));
    if (err != ESP_OK) {
        end_op_();
        return err;
    }
    memcpy(s_ap_ssid, next.ssid, sizeof(s_ap_ssid));
    memcpy(s_ap_pass, next.pass, sizeof(s_ap_pass));
    memcpy(s_ap_ip, next.ip, sizeof(s_ap_ip));
    if (s_mode == WIFI_MGR_MODE_AP && s_wifi_started) {
        err = stop_wifi_();
        if (err == ESP_OK) err = apply_ap_();
    }
    end_op_();
    return err;
}

void wifi_manager_ap_get(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz, char *ip, size_t ip_sz)
{
    if (ssid && ssid_sz) strlcpy(ssid, s_ap_ssid, ssid_sz);
    if (pass && pass_sz) strlcpy(pass, s_ap_pass, pass_sz);
    if (ip && ip_sz)     strlcpy(ip,   s_ap_ip,   ip_sz);
}

esp_err_t wifi_manager_sta_set(const char *ssid, const char *pass)
{
    const size_t ssid_len = ssid ? strnlen(ssid, sizeof(s_sta_ssid)) : 0;
    const size_t pass_len = pass ? strnlen(pass, sizeof(s_sta_pass)) : 0;

    if (ssid_len == 0 || ssid_len > sizeof(((wifi_config_t *)0)->sta.ssid)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass && (pass_len >= sizeof(((wifi_config_t *)0)->sta.password) ||
                 (pass_len > 0 && pass_len < 8))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!begin_op_("sta_set")) return ESP_ERR_INVALID_STATE;
    wifi_sta_saved_t next = {.version = NVS_CONFIG_VERSION};
    strlcpy(next.ssid, ssid, sizeof(next.ssid));
    strlcpy(next.pass, pass ? pass : s_sta_pass, sizeof(next.pass));
    esp_err_t err = nvs_save_blob_(NVS_KEY_STA_CONFIG, &next, sizeof(next));
    if (err != ESP_OK) {
        end_op_();
        return err;
    }
    s_user_disconnect = false;
    memcpy(s_sta_ssid, next.ssid, sizeof(s_sta_ssid));
    memcpy(s_sta_pass, next.pass, sizeof(s_sta_pass));
    if (s_mode == WIFI_MGR_MODE_STA && s_wifi_started) {
        err = stop_wifi_();
        if (err == ESP_OK) {
            s_sta_state = WIFI_MGR_DISCONNECTED;
            err = apply_sta_();
        }
    }
    end_op_();
    return err;
}

void wifi_manager_sta_get(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    if (ssid && ssid_sz) strlcpy(ssid, s_sta_ssid, ssid_sz);
    if (pass && pass_sz) strlcpy(pass, s_sta_pass, pass_sz);
}

uint16_t wifi_manager_scan(wifi_ap_record_t *list, uint16_t max)
{
    if (!list || !max) return 0;
    if (!s_inited && wifi_manager_init() != ESP_OK) return 0;
    if (!begin_op_("scan")) return 0;

    s_scanning = true;
    bool temp_started = false;
    uint16_t count = max;
    esp_err_t err = ESP_OK;

    if (!s_wifi_started) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) {
            err = esp_wifi_start();
        }
        if (err == ESP_OK) {
            s_wifi_started = true;
            temp_started = true;
        }
    } else if (s_mode == WIFI_MGR_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    } else {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }

    if (err == ESP_OK) {
        /* Give the radio a moment to settle before scanning */
        vTaskDelay(pdMS_TO_TICKS(120));

        wifi_scan_config_t sc = {.show_hidden = false};
        err = esp_wifi_scan_start(&sc, true);
        if (err == ESP_OK) {
            err = esp_wifi_scan_get_ap_records(&count, list);
        }
    }

    if (temp_started) {
        esp_err_t stop_err = stop_wifi_();
        if (err == ESP_OK) err = stop_err;
    } else if (s_wifi_started) {
        wifi_mode_t restore_mode = (s_mode == WIFI_MGR_MODE_AP) ? WIFI_MODE_AP : WIFI_MODE_STA;
        esp_err_t restore_err = esp_wifi_set_mode(restore_mode);
        if (restore_err != ESP_OK) {
            ESP_LOGW(TAG, "scan restore mode failed: %s", esp_err_to_name(restore_err));
            if (err == ESP_OK) err = restore_err;
        }
    }

    s_scanning = false;

    if (s_mode == WIFI_MGR_MODE_STA && s_sta_ssid[0] && !s_user_disconnect &&
        s_sta_state != WIFI_MGR_CONNECTED) {
        s_sta_state = WIFI_MGR_CONNECTING;
        esp_err_t conn_err = esp_wifi_connect();
        if (conn_err != ESP_OK && conn_err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "STA reconnect after scan failed: %s", esp_err_to_name(conn_err));
        }
    }

    end_op_();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    }
    return (err == ESP_OK) ? count : 0;
}

wifi_mgr_sta_state_t wifi_manager_sta_state(void)
{
    return s_wifi_started ? s_sta_state : WIFI_MGR_DISCONNECTED;
}
esp_err_t wifi_manager_sta_connect(void)
{
    if (!begin_op_("sta_connect")) return ESP_ERR_INVALID_STATE;
    s_user_disconnect = false;
    s_sta_state = WIFI_MGR_CONNECTING;
    s_sta_retry = 0;
    esp_err_t err = esp_wifi_connect();
    end_op_();
    return err;
}

esp_err_t wifi_manager_sta_disconnect(void)
{
    if (!begin_op_("sta_disconnect")) return ESP_ERR_INVALID_STATE;
    s_sta_state = WIFI_MGR_DISCONNECTED;
    s_user_disconnect = true;
    esp_err_t err = esp_wifi_disconnect();
    end_op_();
    return err;
}
bool wifi_manager_is_started(void) { return s_wifi_started; }

bool wifi_manager_get_ip_str(char *buf, size_t buf_sz)
{
    esp_netif_ip_info_t ip;
    if (!buf || !buf_sz) return false;
    buf[0] = 0;
    esp_netif_t *n = (s_mode == WIFI_MGR_MODE_AP) ? s_ap_netif : s_sta_netif;
    if (!n || !s_wifi_started || esp_netif_get_ip_info(n, &ip) != ESP_OK || ip.ip.addr == 0) {
        return false;
    }
    snprintf(buf, buf_sz, IPSTR, IP2STR(&ip.ip));
    return true;
}
