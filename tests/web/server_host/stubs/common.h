#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_SIZE -3
#define ESP_ERR_INVALID_STATE -4
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define HTTP_GET 0
#define HTTPD_WS_TYPE_TEXT 1
#define HTTPD_WS_TYPE_CLOSE 8
#define HTTPD_WS_CLIENT_WEBSOCKET 2
#define SHUT_RDWR 2
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
typedef void *httpd_handle_t;
typedef void (*httpd_work_fn_t)(void *);
typedef struct {int method; void *sess_ctx; void (*free_ctx)(void *);} httpd_req_t;
typedef struct {int type; bool final; uint8_t *payload; size_t len;} httpd_ws_frame_t;
typedef struct {int stack_size,max_open_sockets,max_uri_handlers,recv_wait_timeout,send_wait_timeout;bool lru_purge_enable;} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
typedef struct {const char *uri;int method;esp_err_t (*handler)(httpd_req_t*);bool is_websocket;} httpd_uri_t;
esp_err_t httpd_resp_set_type(httpd_req_t*,const char*);
esp_err_t httpd_resp_set_hdr(httpd_req_t*,const char*,const char*);
esp_err_t httpd_resp_send_chunk(httpd_req_t*,const char*,size_t);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*,const char*,char*,size_t);
int httpd_req_to_sockfd(httpd_req_t*);
esp_err_t httpd_ws_recv_frame(httpd_req_t*,httpd_ws_frame_t*,size_t);
int httpd_ws_get_fd_info(httpd_handle_t,int);
esp_err_t httpd_ws_send_frame_async(httpd_handle_t,int,httpd_ws_frame_t*);
esp_err_t httpd_queue_work(httpd_handle_t,httpd_work_fn_t,void*);
esp_err_t httpd_start(httpd_handle_t*,httpd_config_t*);
esp_err_t httpd_stop(httpd_handle_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t,const httpd_uri_t*);
void *heap_caps_malloc(size_t,int);
QueueHandle_t xQueueCreate(size_t,size_t);
int xQueueSend(QueueHandle_t,const void*,unsigned);
int xQueueReceive(QueueHandle_t,void*,unsigned);
void vQueueDelete(QueueHandle_t);
int xTaskCreate(void(*)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*);
void vTaskDelay(unsigned);
void vTaskDelete(TaskHandle_t);
int shutdown(int,int);
void app_controller_web_disconnected(int);
bool app_controller_web_submit(int,const char*,size_t);
