#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
typedef struct {uint32_t address,size; char label[17];} esp_partition_t;
typedef enum {ESP_OTA_IMG_NEW,ESP_OTA_IMG_PENDING_VERIFY,ESP_OTA_IMG_VALID,ESP_OTA_IMG_INVALID} esp_ota_img_states_t;
typedef uint32_t esp_ota_handle_t;
#define OTA_WITH_SEQUENTIAL_WRITES 0xfffffffeu
typedef void *httpd_handle_t;
typedef struct {size_t content_len; httpd_handle_t handle;} httpd_req_t;
typedef struct {const char *uri; int method; esp_err_t (*handler)(httpd_req_t *);} httpd_uri_t;
typedef struct {int server_port,ctrl_port,stack_size,max_open_sockets,max_uri_handlers,recv_wait_timeout,send_wait_timeout;} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define HTTP_GET 0
#define HTTP_POST 1
size_t httpd_req_get_hdr_value_len(httpd_req_t *,const char *);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *,const char *,char *,size_t);
int httpd_req_recv(httpd_req_t *,char *,size_t);
esp_err_t httpd_resp_set_type(httpd_req_t *,const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *,const char *,const char *);
esp_err_t httpd_resp_set_status(httpd_req_t *,const char *);
esp_err_t httpd_resp_sendstr(httpd_req_t *,const char *);
int httpd_req_to_sockfd(httpd_req_t *);
esp_err_t httpd_sess_trigger_close(httpd_handle_t,int);
esp_err_t httpd_start(httpd_handle_t *,const httpd_config_t *);
esp_err_t httpd_stop(httpd_handle_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t,const httpd_uri_t *);
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *,esp_ota_img_states_t *);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);
esp_err_t esp_ota_begin(const esp_partition_t *,size_t,esp_ota_handle_t *);
esp_err_t esp_ota_write(esp_ota_handle_t,const void *,size_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
typedef struct {char version[32];} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
int64_t esp_timer_get_time(void);
void esp_restart(void);
const char *esp_err_to_name(esp_err_t);
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_LOGE(tag,...) ((void)(tag))
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(n) (n)
#define pdPASS 1
void vTaskDelay(unsigned);
void vTaskDelete(void *);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,void *);
typedef struct {unsigned count; uint8_t bytes[32];} mbedtls_sha256_context;
void mbedtls_sha256_init(mbedtls_sha256_context *);
void mbedtls_sha256_free(mbedtls_sha256_context *);
int mbedtls_sha256_starts(mbedtls_sha256_context *,int);
int mbedtls_sha256_update(mbedtls_sha256_context *,const uint8_t *,size_t);
int mbedtls_sha256_finish(mbedtls_sha256_context *,uint8_t *);
typedef struct lv_obj_t lv_obj_t;
typedef struct lv_timer_t lv_timer_t;
typedef struct lv_event_t lv_event_t;
extern int lv_font_montserrat_28,lv_font_montserrat_18;
enum {LV_ALIGN_TOP_MID,LV_ALIGN_CENTER,LV_ALIGN_BOTTOM_MID,LV_EVENT_CLICKED,LV_TEXT_ALIGN_CENTER,LV_ANIM_OFF,LV_STATE_DISABLED};
bool lvgl_port_lock(int);
void lvgl_port_unlock(void);
void lv_timer_del(lv_timer_t *);
lv_timer_t *lv_timer_create(void (*)(lv_timer_t *),unsigned,void *);
lv_obj_t *lv_obj_create(lv_obj_t *);
lv_obj_t *lv_label_create(lv_obj_t *);
lv_obj_t *lv_bar_create(lv_obj_t *);
lv_obj_t *lv_btn_create(lv_obj_t *);
void lv_label_set_text(lv_obj_t *,const char *);
void lv_label_set_text_fmt(lv_obj_t *,const char *,...);
void lv_obj_set_style_text_font(lv_obj_t *,const void *,int);
void lv_obj_set_style_text_align(lv_obj_t *,int,int);
void lv_obj_align(lv_obj_t *,int,int,int);
void lv_obj_set_width(lv_obj_t *,int);
void lv_obj_set_size(lv_obj_t *,int,int);
void lv_obj_add_event_cb(lv_obj_t *,void (*)(lv_event_t *),int,void *);
void lv_obj_center(lv_obj_t *);
void lv_scr_load(lv_obj_t *);
void lv_obj_del(lv_obj_t *);
void lv_obj_add_state(lv_obj_t *,int);
void lv_bar_set_value(lv_obj_t *,int,int);
void ui_theme_apply_screen(lv_obj_t *);
void screen_ui_show(void);
bool wifi_manager_is_started(void);
void wifi_manager_get_ip_str(char *,size_t);
esp_err_t nmea_templates_save_now(void);
bool app_controller_healthy(void);
