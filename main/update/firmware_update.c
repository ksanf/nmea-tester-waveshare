/* Explicit local maintenance mode; separate HTTP task, no LVGL calls from HTTP. */
#include "update/firmware_update.h"
#include "update/firmware_info.h"
#include "update_format.h"
#include "app/app_controller.h"
#include "wifi/wifi_manager.h"
#include "nmea_editor/nmea_templates.h"
#include "ui/screens/screen_ui.h"
#include "ui/ui_theme.h"
#include "lvgl_port/waveshare_lvgl_port.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define WINDOW_MS (10u*60u*1000u)
#define TRANSFER_MS (2u*60u*1000u)
static httpd_handle_t s_http;
static atomic_bool s_active, s_busy, s_close, s_reboot;
static atomic_uint s_received, s_total, s_last_activity, s_ui_ticks;
static uint32_t s_reboot_at;
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static char s_message[144];
static lv_obj_t *s_screen,*s_status,*s_bar,*s_exit;
static const char *TAG="fw_update";
static uint32_t now_(void) { return (uint32_t)(esp_timer_get_time()/1000); }
static void message_(const char *s) {
    portENTER_CRITICAL(&s_lock); snprintf(s_message,sizeof(s_message),"%s",s); portEXIT_CRITICAL(&s_lock);
}
bool firmware_update_active(void) { return atomic_load(&s_active); }
bool firmware_update_busy(void) { return atomic_load(&s_busy); }
void firmware_update_request_close(void) { atomic_store(&s_close,true); }
static void close_cb_(lv_event_t *e) { (void)e; firmware_update_request_close(); }

bool firmware_update_pending_boot(void) {
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(),&state)==ESP_OK &&
           state==ESP_OTA_IMG_PENDING_VERIFY;
}
static void boot_guard_(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(30000));
    if(firmware_update_pending_boot()) {
        ESP_LOGE(TAG,"Startup confirmation deadline expired; rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        esp_restart();
    }
    vTaskDelete(NULL);
}
void firmware_update_start_boot_guard(void) {
    if(!firmware_update_pending_boot()) return;
    if(xTaskCreate(boot_guard_,"ota_deadline",4096,NULL,4,NULL)==pdPASS) return;
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart();
}
static void boot_ui_tick_(lv_timer_t *t) { (void)t; atomic_fetch_add(&s_ui_ticks,1); }
static void boot_confirm_(void *arg) {
    lv_timer_t *timer=arg;
    vTaskDelay(pdMS_TO_TICKS(10000));
    bool healthy=atomic_load(&s_ui_ticks)>=3 && app_controller_healthy();
    if(lvgl_port_lock(1000)) { lv_timer_del(timer); lvgl_port_unlock(); }
    else healthy=false;
    if(healthy) {
        esp_err_t err=esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG,"Startup confirmation: %s",esp_err_to_name(err));
        if(err!=ESP_OK) esp_restart();
    } else {
        ESP_LOGE(TAG,"Startup check failed; rolling back without erasing settings");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        esp_restart();
    }
    vTaskDelete(NULL);
}
void firmware_update_boot_check(bool startup_ok) {
    if(!firmware_update_pending_boot()) return;
    lv_timer_t *timer=NULL;
    if(startup_ok && lvgl_port_lock(-1)) {
        atomic_store(&s_ui_ticks,0);
        timer=lv_timer_create(boot_ui_tick_,1000,NULL);
        lvgl_port_unlock();
    }
    if(timer && xTaskCreate(boot_confirm_,"ota_verify",4096,timer,3,NULL)==pdPASS) return;
    ESP_LOGE(TAG,"Essential startup check failed");
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart();
}
static esp_err_t upload_(httpd_req_t *req) {
    bool expected=false;
    if(!atomic_load(&s_active) || atomic_load(&s_close) || atomic_load(&s_reboot) ||
       !atomic_compare_exchange_strong(&s_busy,&expected,true)) {
        httpd_resp_set_status(req,"409 Conflict"); return httpd_resp_sendstr(req,"Update mode is not ready");
    }
    esp_err_t err=ESP_FAIL;
    esp_ota_handle_t ota=0;
    bool begun=false;
    const char *failure="Upload interrupted; previous firmware retained";
    char checksum[65]; uint8_t expected_sha[32], actual_sha[32];
    uint8_t block[4096]; size_t used=0, received=0;
    uint32_t started=now_();
    mbedtls_sha256_context hash; mbedtls_sha256_init(&hash);
    atomic_store(&s_received,0); atomic_store(&s_total,(unsigned)req->content_len);
    atomic_store(&s_last_activity,now_());
    if(req->content_len<NMEA_IMAGE_PREFIX_SIZE || req->content_len>NMEA_APP_SIZE ||
       httpd_req_get_hdr_value_len(req,"X-NMEA-SHA256")!=64 ||
       httpd_req_get_hdr_value_str(req,"X-NMEA-SHA256",checksum,sizeof(checksum))!=ESP_OK ||
       !nmea_parse_sha256(checksum,expected_sha)) {
        failure="Invalid image size or SHA-256 header"; goto done;
    }
    if(firmware_update_pending_boot()) { failure="Current firmware has not passed startup verification"; goto done; }
    const esp_partition_t *next=esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running=esp_ota_get_running_partition();
    if(!next || !running || next->address==running->address || next->size!=NMEA_APP_SIZE ||
       (next->address!=NMEA_APP0_OFFSET && next->address!=NMEA_APP1_OFFSET)) {
        failure="OTA layout is unavailable; use USB installation"; goto done;
    }
    /* Validate the complete board descriptor before erasing any flash. */
    while(used<NMEA_IMAGE_PREFIX_SIZE) {
        if((uint32_t)(now_()-started)>=TRANSFER_MS) { failure="Update transfer timed out"; goto done; }
        int n=httpd_req_recv(req,(char *)block+used,NMEA_IMAGE_PREFIX_SIZE-used);
        if(n<=0 || atomic_load(&s_close)) goto done;
        used+=(size_t)n;
    }
    failure=nmea_validate_image(block,used,req->content_len);
    if(failure) goto done;
    failure="Upload interrupted; previous firmware retained";
    message_("Receiving firmware. Do not disconnect power.");
    err=esp_ota_begin(next,OTA_WITH_SEQUENTIAL_WRITES,&ota);
    if(err!=ESP_OK) { failure="Could not start OTA write"; goto done; }
    begun=true;
    if(mbedtls_sha256_starts(&hash,0)!=0) goto done;
    while(received<(size_t)req->content_len) {
        if((uint32_t)(now_()-started)>=TRANSFER_MS) { failure="Update transfer timed out"; goto done; }
        if(atomic_load(&s_close)) goto done;
        if(!used) {
            size_t want=(size_t)req->content_len-received;
            if(want>sizeof(block)) want=sizeof(block);
            int n=httpd_req_recv(req,(char *)block,want);
            if(n<=0) goto done;
            used=(size_t)n;
        }
        if(esp_ota_write(ota,block,used)!=ESP_OK || mbedtls_sha256_update(&hash,block,used)!=0) {
            failure="Flash write failed; previous firmware retained"; goto done;
        }
        received+=used; used=0;
        atomic_store(&s_received,(unsigned)received); atomic_store(&s_last_activity,now_());
        vTaskDelay(1);
    }
    if(mbedtls_sha256_finish(&hash,actual_sha)!=0 || memcmp(actual_sha,expected_sha,32)) {
        failure="SHA-256 mismatch; previous firmware retained"; goto done;
    }
    if(atomic_load(&s_close)) goto done;
    err=esp_ota_end(ota); begun=false;
    if(err!=ESP_OK) { failure="ESP image verification failed; previous firmware retained"; goto done; }
    err=esp_ota_set_boot_partition(next);
    if(err!=ESP_OK) { failure="Boot selection failed; previous firmware retained"; goto done; }
    failure=NULL;
done:
    if(begun) esp_ota_abort(ota);
    mbedtls_sha256_free(&hash);
    httpd_resp_set_type(req,"text/plain");
    if(failure) {
        message_(failure); httpd_resp_set_status(req,"400 Bad Request");
        /* Close the unread request body; it must not become a second request. */
        httpd_resp_set_hdr(req,"Connection","close");
        err=httpd_resp_sendstr(req,failure);
        httpd_sess_trigger_close(req->handle,httpd_req_to_sockfd(req));
    } else {
        message_("Verified. Restarting... Wi-Fi will be off after restart.");
        err=httpd_resp_sendstr(req,"VERIFIED; restarting. Enable Wi-Fi again to confirm the running version.\n");
        s_reboot_at=now_()+2500u;
        atomic_store(&s_reboot,true);
    }
    atomic_store(&s_busy,false);
    return err;
}
esp_err_t firmware_update_open(void) {
    if(atomic_load(&s_active)) return ESP_ERR_INVALID_STATE;
    if(!wifi_manager_is_started()) return ESP_ERR_INVALID_STATE;
    char ip[20]={0}; wifi_manager_get_ip_str(ip,sizeof(ip));
    if(!ip[0] || !strcmp(ip,"0.0.0.0")) return ESP_ERR_INVALID_STATE;
    esp_err_t err=nmea_templates_save_now();
    if(err!=ESP_OK) return err;
    atomic_store(&s_close,false); atomic_store(&s_reboot,false); atomic_store(&s_busy,false);
    atomic_store(&s_received,0); atomic_store(&s_total,0); atomic_store(&s_last_activity,now_());
    message_("Ready. Select the firmware package in the Windows updater.");
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
    cfg.server_port=NMEA_UPDATE_PORT; cfg.ctrl_port=32769; cfg.stack_size=10240;
    cfg.max_open_sockets=2; cfg.max_uri_handlers=2;
    cfg.recv_wait_timeout=3; cfg.send_wait_timeout=3;
    err=httpd_start(&s_http,&cfg); if(err!=ESP_OK) return err;
    const httpd_uri_t info={.uri="/api/v1/info",.method=HTTP_GET,.handler=firmware_info_http_handler};
    const httpd_uri_t upload={.uri="/api/v1/firmware",.method=HTTP_POST,.handler=upload_};
    err=httpd_register_uri_handler(s_http,&info);
    if(err==ESP_OK) err=httpd_register_uri_handler(s_http,&upload);
    if(err!=ESP_OK) { httpd_stop(s_http); s_http=NULL; return err; }
    if(!lvgl_port_lock(-1)) { httpd_stop(s_http); s_http=NULL; return ESP_ERR_TIMEOUT; }
    s_screen=lv_obj_create(NULL); ui_theme_apply_screen(s_screen);
    lv_obj_t *title=lv_label_create(s_screen); lv_label_set_text(title,"FIRMWARE UPDATE");
    lv_obj_set_style_text_font(title,&lv_font_montserrat_28,0); lv_obj_align(title,LV_ALIGN_TOP_MID,0,25);
    lv_obj_t *details=lv_label_create(s_screen);
    lv_label_set_text_fmt(details,"Version %s\nWindows updater: %s:%u\nUpdates are unsigned. Use a trusted network.",
        esp_app_get_description()->version,ip,NMEA_UPDATE_PORT);
    lv_obj_set_style_text_font(details,&lv_font_montserrat_18,0); lv_obj_align(details,LV_ALIGN_TOP_MID,0,90);
    s_status=lv_label_create(s_screen); lv_obj_set_width(s_status,700);
    lv_obj_set_style_text_align(s_status,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_font(s_status,&lv_font_montserrat_18,0); lv_obj_align(s_status,LV_ALIGN_CENTER,0,25);
    s_bar=lv_bar_create(s_screen); lv_obj_set_size(s_bar,650,25); lv_obj_align(s_bar,LV_ALIGN_CENTER,0,90);
    s_exit=lv_btn_create(s_screen); lv_obj_set_size(s_exit,220,60); lv_obj_align(s_exit,LV_ALIGN_BOTTOM_MID,0,-25);
    lv_obj_add_event_cb(s_exit,close_cb_,LV_EVENT_CLICKED,NULL);
    lv_obj_t *label=lv_label_create(s_exit); lv_label_set_text(label,"Cancel / Return"); lv_obj_center(label);
    lv_scr_load(s_screen); atomic_store(&s_active,true); lvgl_port_unlock();
    return ESP_OK;
}
void firmware_update_tick(void) {
    if(!atomic_load(&s_active)) return;
    if(atomic_load(&s_reboot) && !atomic_load(&s_busy) && (int32_t)(now_()-s_reboot_at)>=0) esp_restart();
    bool expired=(uint32_t)(now_()-atomic_load(&s_last_activity))>=WINDOW_MS;
    if((atomic_load(&s_close)||expired) && !atomic_load(&s_busy) && !atomic_load(&s_reboot)) {
        httpd_stop(s_http); s_http=NULL;
        if(lvgl_port_lock(-1)) { screen_ui_show(); lv_obj_del(s_screen); s_screen=NULL; lvgl_port_unlock(); }
        atomic_store(&s_active,false); return;
    }
    char text[sizeof(s_message)]; portENTER_CRITICAL(&s_lock); memcpy(text,s_message,sizeof(text)); portEXIT_CRITICAL(&s_lock);
    if(lvgl_port_lock(20)) {
        lv_label_set_text(s_status,text);
        unsigned total=atomic_load(&s_total),done=atomic_load(&s_received);
        lv_bar_set_value(s_bar,total?(int)((uint64_t)done*100u/total):0,LV_ANIM_OFF);
        if(atomic_load(&s_reboot)) lv_obj_add_state(s_exit,LV_STATE_DISABLED);
        lvgl_port_unlock();
    }
}
