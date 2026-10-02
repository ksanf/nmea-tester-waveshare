/* Run the actual manager with injectable storage failures, no radio hardware. */
#include <assert.h>
#include "wifi_manager/common.h"
#include "../../main/wifi/wifi_manager.c"

unsigned host_wifi_calls, host_wifi_stops, host_wifi_starts;
static esp_err_t fail_open, fail_set, fail_commit;
static unsigned opens, closes, sets, commits;
static char last_key[16];
typedef struct { char key[16]; uint8_t value[160]; size_t len; } item_t;
static item_t storage[12];
static size_t stored;

static item_t *lookup(const char *key)
{
    for (size_t i = 0; i < stored; ++i) if (!strcmp(storage[i].key, key)) return &storage[i];
    return NULL;
}
static void seed(const char *key, const void *value, size_t len)
{
    item_t *v = lookup(key);
    if (!v) { assert(stored < 12); v = &storage[stored++]; strlcpy(v->key, key, sizeof(v->key)); }
    assert(len <= sizeof(v->value));
    memcpy(v->value, value, len); v->len = len;
}
static void seed_str(const char *key, const char *value) { seed(key, value, strlen(value) + 1); }

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{
    assert(!strcmp(ns,"wifi")); (void)mode;
    ++opens; if (fail_open) return fail_open; *h = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    assert(h == 1); item_t *v = lookup(key);
    if (!v) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < v->len) { *len = v->len; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(out,v->value,v->len); *len = v->len; return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{ return nvs_get_blob(h,key,out,len); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t len)
{
    assert(h == 1); ++sets; strlcpy(last_key,key,sizeof(last_key));
    if (fail_set) return fail_set;
    /* Model the difficult case: set reaches flash before commit fails. */
    seed(key,data,len); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { assert(h == 1); ++commits; return fail_commit; }
void nvs_close(nvs_handle_t h) { assert(h == 1); ++closes; }

static void reboot(void)
{
    s_inited = s_wifi_started = s_wifi_evt_registered = s_ip_evt_registered = false;
    s_enabled = false; s_ap_netif = s_sta_netif = NULL;
    s_mode = WIFI_MGR_MODE_AP; s_sta_state = WIFI_MGR_DISCONNECTED;
    s_sta_retry = 0; s_user_disconnect = s_scanning = s_op_busy = false;
    memset(s_ap_ssid,0,sizeof(s_ap_ssid)); memset(s_ap_pass,0,sizeof(s_ap_pass)); memset(s_ap_ip,0,sizeof(s_ap_ip));
    memset(s_sta_ssid,0,sizeof(s_sta_ssid)); memset(s_sta_pass,0,sizeof(s_sta_pass));
    assert(wifi_manager_init() == ESP_OK);
}
static void counters_clear(void)
{ opens=closes=sets=commits=host_wifi_calls=host_wifi_stops=host_wifi_starts=0; last_key[0]=0; }
static void reset_legacy(void)
{
    fail_open=fail_set=fail_commit=0; stored=0; memset(storage,0,sizeof(storage));
    seed_str("ap_ssid","old-ap"); seed_str("ap_pass","old-ap-password"); seed_str("ap_ip","192.168.7.1");
    seed_str("sta_ssid","old-sta"); seed_str("sta_pass","old-sta-password");
    reboot(); counters_clear();
}

static void failure_tests(void)
{
    for (unsigned api=0; api<3; ++api) for (unsigned stage=0; stage<3; ++stage) {
        reset_legacy();
        if (api == 2) s_mode = WIFI_MGR_MODE_STA;
        s_wifi_started=s_enabled=true; s_user_disconnect=true; s_sta_state=WIFI_MGR_CONNECTED;
        esp_err_t expected = ESP_ERR_NVS_NOT_ENOUGH_SPACE + (int)stage;
        if (stage == 0) fail_open=expected;
        if (stage == 1) fail_set=expected;
        if (stage == 2) fail_commit=expected;
        esp_err_t err = api == 0 ? wifi_manager_set_mode(WIFI_MGR_MODE_STA) :
                        api == 1 ? wifi_manager_ap_set("new-ap","new-ap-password","192.168.8.1") :
                                   wifi_manager_sta_set("new-sta","new-sta-password");
        assert(err == expected && !s_op_busy);
        assert(s_wifi_started && s_enabled && s_user_disconnect && s_sta_state == WIFI_MGR_CONNECTED);
        assert(s_mode == (api == 2 ? WIFI_MGR_MODE_STA : WIFI_MGR_MODE_AP));
        assert(!strcmp(s_ap_ssid,"old-ap") && !strcmp(s_ap_pass,"old-ap-password") && !strcmp(s_ap_ip,"192.168.7.1"));
        assert(!strcmp(s_sta_ssid,"old-sta") && !strcmp(s_sta_pass,"old-sta-password"));
        assert(host_wifi_calls == 0 && opens == 1 && closes == (stage != 0));
        assert(sets == (stage != 0) && commits == (stage == 2));
        /* A reported failure must release the operation gate for a retry. */
        fail_open=fail_set=fail_commit=0;
        assert(wifi_manager_set_mode(WIFI_MGR_MODE_AP) == ESP_OK);
    }
}

static void load_save_tests(void)
{
    reset_legacy();
    assert(!strcmp(s_ap_ssid,"old-ap") && !strcmp(s_sta_ssid,"old-sta"));
    assert(wifi_manager_ap_set("new-ap","","192.168.9.1") == ESP_OK);
    assert(sets == 1 && commits == 1 && closes == 1 && !strcmp(last_key,"ap_config"));
    assert(host_wifi_calls == 0 && !s_wifi_started);
    assert(!strcmp((char *)lookup("ap_ssid")->value,"old-ap"));
    assert(!strcmp((char *)lookup("ap_pass")->value,"old-ap-password"));
    reboot();
    assert(!strcmp(s_ap_ssid,"new-ap") && !s_ap_pass[0] && !strcmp(s_ap_ip,"192.168.9.1"));
    assert(!strcmp(s_sta_ssid,"old-sta"));
    counters_clear();
    assert(wifi_manager_ap_set("renamed-ap",NULL,NULL) == ESP_OK);
    reboot();
    assert(!strcmp(s_ap_ssid,"renamed-ap") && !s_ap_pass[0] && !strcmp(s_ap_ip,"192.168.9.1"));
    counters_clear();
    assert(wifi_manager_sta_set("new-sta","new-sta-password") == ESP_OK);
    assert(sets == 1 && commits == 1 && !strcmp(last_key,"sta_config"));
    assert(host_wifi_calls == 0);
    assert(wifi_manager_set_mode(WIFI_MGR_MODE_STA) == ESP_OK);
    reboot();
    assert(s_mode == WIFI_MGR_MODE_STA && !strcmp(s_sta_ssid,"new-sta") && !strcmp(s_sta_pass,"new-sta-password"));
    assert(!s_wifi_started && !s_enabled);
    assert(wifi_manager_sta_set("renamed-sta",NULL) == ESP_OK);
    reboot();
    assert(!strcmp(s_sta_ssid,"renamed-sta") && !strcmp(s_sta_pass,"new-sta-password"));

    /* An invalid/incomplete newer record must not hide a valid legacy config. */
    reset_legacy();
    wifi_ap_saved_t bad = {.version=NVS_CONFIG_VERSION};
    memset(bad.ssid,'x',sizeof(bad.ssid));
    seed("ap_config",&bad,sizeof(bad));
    reboot();
    assert(!strcmp(s_ap_ssid,"old-ap"));
    bad.version=99; seed("ap_config",&bad,sizeof(bad)); reboot();
    assert(!strcmp(s_ap_ssid,"old-ap"));
    seed_str("ap_pass",""); reboot(); assert(!s_ap_pass[0]);
}

static void radio_and_validation_tests(void)
{
    reset_legacy(); s_wifi_started=s_enabled=true;
    assert(wifi_manager_ap_set("new-ap","new-ap-password","192.168.8.1") == ESP_OK);
    assert(host_wifi_stops==1 && host_wifi_starts==1 && s_wifi_started);
    counters_clear();
    assert(wifi_manager_sta_set("new-sta","new-sta-password") == ESP_OK);
    assert(host_wifi_calls == 0); /* inactive STA settings don't restart AP */
    assert(wifi_manager_set_mode(WIFI_MGR_MODE_STA) == ESP_OK);
    assert(host_wifi_stops==1 && host_wifi_starts==1 && s_mode==WIFI_MGR_MODE_STA);
    counters_clear();
    assert(wifi_manager_sta_set("changed-sta",NULL) == ESP_OK);
    assert(host_wifi_stops==1 && host_wifi_starts==1 && !strcmp(s_sta_pass,"new-sta-password"));
    counters_clear();
    assert(wifi_manager_ap_set("","valid-pass","192.168.8.1") == ESP_ERR_INVALID_ARG);
    assert(wifi_manager_ap_set("ssid","short","192.168.8.1") == ESP_ERR_INVALID_ARG);
    assert(wifi_manager_ap_set("ssid",NULL,"192.168.8.1x") == ESP_ERR_INVALID_ARG);
    assert(wifi_manager_sta_set("ssid","short") == ESP_ERR_INVALID_ARG);
    assert(wifi_manager_set_mode(99) == ESP_ERR_INVALID_ARG);
    assert(opens==0 && sets==0 && host_wifi_calls==0);
}

int main(void)
{
    failure_tests(); load_save_tests(); radio_and_validation_tests();
    puts("Wi-Fi manager: injected NVS errors, grouped save, legacy load, radio state PASS");
    return 0;
}
