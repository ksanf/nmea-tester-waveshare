/* Focused ESP-IDF test doubles for the actual Wi-Fi manager. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <esp_err.h>
#include "../stubs/templates/compat.h"

#define ESP_ERR_WIFI_NOT_STARTED 0x3002
#define ESP_ERR_WIFI_CONN 0x3003
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 0x4001
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED 0x4002
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1105
#define WIFI_AP_DEFAULT_ENABLED 0
#define WIFI_AP_SSID "test-default"
#define WIFI_AP_PASSWORD "default-password"
#define WIFI_AP_CHANNEL 6
#define WIFI_AP_MAX_CONNECTIONS 1
#define WIFI_AP_INACTIVE_TIMEOUT_SEC 600
#define WIFI_AP_B_ONLY 1
#define ESP_LOGI(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW ESP_LOGI
#define ESP_LOGE ESP_LOGI
#define LOG_CFG_WIFI_AP 0

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(ms) (ms)
static inline void vTaskDelay(unsigned ms) { (void)ms; }

typedef const char *esp_event_base_t;
static const char host_wifi_event[] = "wifi";
static const char host_ip_event[] = "ip";
#define WIFI_EVENT host_wifi_event
#define IP_EVENT host_ip_event
#define ESP_EVENT_ANY_ID -1
#define IP_EVENT_STA_GOT_IP 0
#define WIFI_EVENT_AP_START 0
#define WIFI_EVENT_AP_STACONNECTED 1
#define WIFI_EVENT_AP_STADISCONNECTED 2
#define WIFI_EVENT_STA_START 3
#define WIFI_EVENT_STA_DISCONNECTED 4

typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, gw, netmask; } esp_netif_ip_info_t;
typedef struct { esp_netif_ip_info_t ip_info; } ip_event_got_ip_t;
typedef struct { unsigned dummy; } esp_netif_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) (unsigned)((p)->addr & 255), (unsigned)(((p)->addr >> 8) & 255), (unsigned)(((p)->addr >> 16) & 255), (unsigned)((p)->addr >> 24)

typedef int wifi_mode_t;
typedef int wifi_auth_mode_t;
#define WIFI_MODE_AP 1
#define WIFI_MODE_STA 2
#define WIFI_MODE_APSTA 3
#define WIFI_STORAGE_RAM 0
#define WIFI_IF_AP 0
#define WIFI_IF_STA 1
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA2_PSK 2
#define WIFI_COUNTRY_POLICY_MANUAL 0
#define WIFI_PROTOCOL_11B 1
#define WIFI_PROTOCOL_11G 2
#define WIFI_ALL_CHANNEL_SCAN 0
#define WIFI_CONNECT_AP_BY_SIGNAL 0
#define WIFI_BW_HT20 0
#define WIFI_PS_NONE 0
typedef struct { char cc[3]; int schan,nchan,max_tx_power,policy; } wifi_country_t;
typedef struct { bool required,capable; } wifi_pmf_config_t;
typedef struct {
    struct { uint8_t ssid[32],password[64]; size_t ssid_len; int max_connection,authmode,channel; wifi_pmf_config_t pmf_cfg; } ap;
    struct { uint8_t ssid[32],password[64]; int scan_method,sort_method; struct { int authmode; } threshold; wifi_pmf_config_t pmf_cfg; } sta;
} wifi_config_t;
typedef struct { int dummy; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() {0}
typedef struct { bool show_hidden; } wifi_scan_config_t;
typedef struct { uint8_t ssid[33]; int rssi; wifi_auth_mode_t authmode; } wifi_ap_record_t;

extern unsigned host_wifi_calls, host_wifi_stops, host_wifi_starts;
static inline esp_err_t esp_wifi_stop(void) { ++host_wifi_calls; ++host_wifi_stops; return ESP_OK; }
static inline esp_err_t esp_wifi_start(void) { ++host_wifi_calls; ++host_wifi_starts; return ESP_OK; }
#define WIFI_STUB0(name) static inline esp_err_t name(void) { ++host_wifi_calls; return ESP_OK; }
#define WIFI_STUB1(name,t1) static inline esp_err_t name(t1 a) { (void)a; ++host_wifi_calls; return ESP_OK; }
#define WIFI_STUB2(name,t1,t2) static inline esp_err_t name(t1 a,t2 b) { (void)a;(void)b; ++host_wifi_calls; return ESP_OK; }
WIFI_STUB0(esp_wifi_connect)
WIFI_STUB0(esp_wifi_disconnect)
WIFI_STUB0(esp_netif_init)
WIFI_STUB0(esp_event_loop_create_default)
WIFI_STUB1(esp_wifi_set_ps,int)
WIFI_STUB1(esp_wifi_set_mode,int)
WIFI_STUB1(esp_wifi_set_country,const wifi_country_t *)
WIFI_STUB1(esp_wifi_set_storage,int)
WIFI_STUB1(esp_wifi_init,const wifi_init_config_t *)
WIFI_STUB1(esp_netif_dhcps_stop,esp_netif_t *)
WIFI_STUB1(esp_netif_dhcps_start,esp_netif_t *)
WIFI_STUB2(esp_wifi_set_protocol,int,int)
WIFI_STUB2(esp_wifi_set_config,int,const wifi_config_t *)
WIFI_STUB2(esp_wifi_set_bandwidth,int,int)
WIFI_STUB2(esp_wifi_set_inactive_time,int,int)
WIFI_STUB2(esp_netif_set_ip_info,esp_netif_t *,const esp_netif_ip_info_t *)
WIFI_STUB2(esp_netif_get_ip_info,esp_netif_t *,esp_netif_ip_info_t *)
WIFI_STUB2(esp_wifi_scan_start,const wifi_scan_config_t *,bool)
WIFI_STUB2(esp_wifi_scan_get_ap_records,uint16_t *,wifi_ap_record_t *)
static inline esp_err_t esp_event_handler_register(esp_event_base_t base,int id,void (*cb)(void *,esp_event_base_t,int32_t,void *),void *arg)
{ (void)base;(void)id;(void)cb;(void)arg;return ESP_OK; }
static inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key)
{ static esp_netif_t n; (void)key;return &n; }
static inline esp_netif_t *esp_netif_create_default_wifi_ap(void) { return esp_netif_get_handle_from_ifkey("AP"); }
static inline esp_netif_t *esp_netif_create_default_wifi_sta(void) { return esp_netif_get_handle_from_ifkey("STA"); }

typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *,int,nvs_handle_t *);
esp_err_t nvs_get_str(nvs_handle_t,const char *,char *,size_t *);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
