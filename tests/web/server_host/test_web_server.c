/* Compile the real dispatcher against narrow host fakes. No ESP-IDF install
 * or board is needed. The fakes intentionally do not model lwIP or task races. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../main/web/web_server.c"
#include "../../../main/update/firmware_info.c"

static outgoing_t *queue[WEB_PENDING_LIMIT];
static unsigned queued;
static bool queue_fail, send_fail, schedule_fail, stop_after_schedule;
static unsigned sends, shutdowns, scheduled;
static int shutdown_fd;
static httpd_work_fn_t captured_fn;
static void *captured_arg;
static jmp_buf dispatch_exit;
static httpd_uri_t registered[4];
static unsigned registrations,task_creations,controller_calls;
static int server_port;
static bool update_active,update_busy,info_send_fail,missing_partition;
static char info_text[400];
static bool no_store,close_http;
const unsigned char web_index_start[]={0};
const unsigned char web_index_end[]={0};

QueueHandle_t xQueueCreate(size_t count,size_t size){assert(count==WEB_PENDING_LIMIT && size==sizeof(outgoing_t *));return (void *)1;}
void vQueueDelete(QueueHandle_t q){(void)q;}
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out){
    (void)fn;(void)name;(void)stack;(void)arg;(void)priority;++task_creations;*out=(void *)1;return pdPASS;
}
esp_err_t httpd_start(httpd_handle_t *server,httpd_config_t *cfg){server_port=cfg->server_port;*server=(void *)1;return ESP_OK;}
esp_err_t httpd_stop(httpd_handle_t server){(void)server;return ESP_OK;}
esp_err_t httpd_register_uri_handler(httpd_handle_t server,const httpd_uri_t *uri){
    (void)server;assert(registrations<4);registered[registrations++]=*uri;return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r,const char *s){(void)r;(void)s;return ESP_OK;}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r,const char *key,const char *value){
    (void)r;
    if(!strcmp(key,"Cache-Control"))no_store=!strcmp(value,"no-store");
    if(!strcmp(key,"Connection"))close_http=!strcmp(value,"close");
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r,const char *s){(void)r;assert(strlen(s)<sizeof(info_text));strcpy(info_text,s);return info_send_fail?ESP_FAIL:ESP_OK;}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r,const char *s,size_t n){(void)r;(void)s;(void)n;return ESP_OK;}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r,const char *key,char *out,size_t size){(void)r;(void)key;(void)out;(void)size;return ESP_FAIL;}
int httpd_req_to_sockfd(httpd_req_t *r){(void)r;return 11;}
esp_err_t httpd_ws_recv_frame(httpd_req_t *r,httpd_ws_frame_t *frame,size_t n){(void)r;(void)frame;(void)n;return ESP_FAIL;}
void app_controller_web_disconnected(int id){(void)id;++controller_calls;}
bool app_controller_web_submit(int id,const char *text,size_t n){(void)id;(void)text;(void)n;++controller_calls;return false;}
bool firmware_update_active(void){return update_active;}
bool firmware_update_busy(void){return update_busy;}
const esp_app_desc_t *esp_app_get_description(void){static const esp_app_desc_t desc={.version="1.3"};return &desc;}
const esp_partition_t *esp_ota_get_running_partition(void){
    static const esp_partition_t partition={.address=NMEA_APP1_OFFSET,.size=NMEA_APP_SIZE,.label="ota_1"};
    return missing_partition?NULL:&partition;
}

void *heap_caps_malloc(size_t count, int caps)
{
    (void)caps;
    return malloc(count);
}

int xQueueSend(QueueHandle_t handle, const void *item, unsigned wait)
{
    (void)handle;
    assert(wait == 0); /* Controller submission must never wait. */
    if (queue_fail || queued == WEB_PENDING_LIMIT) return 0;
    queue[queued++] = *(outgoing_t *const *)item;
    return pdTRUE;
}

int xQueueReceive(QueueHandle_t handle, void *item, unsigned wait)
{
    (void)handle;
    (void)wait;
    if (!queued) {
        atomic_store(&s_dispatch_stop, true);
        return 0;
    }
    *(outgoing_t **)item = queue[0];
    memmove(queue, queue + 1, --queued * sizeof(*queue));
    return pdTRUE;
}

int httpd_ws_get_fd_info(httpd_handle_t server, int fd)
{
    (void)server;
    (void)fd;
    return HTTPD_WS_CLIENT_WEBSOCKET;
}

esp_err_t httpd_ws_send_frame_async(httpd_handle_t server, int fd,
                                   httpd_ws_frame_t *frame)
{
    (void)server;
    (void)fd;
    assert(frame->len == 3);
    assert(memcmp(frame->payload, "abc", 3) == 0);
    ++sends;
    return send_fail ? ESP_FAIL : ESP_OK;
}

esp_err_t httpd_queue_work(httpd_handle_t server, httpd_work_fn_t fn, void *arg)
{
    (void)server;
    ++scheduled;
    captured_fn = fn;
    captured_arg = arg;
    return schedule_fail ? ESP_FAIL : ESP_OK;
}

void vTaskDelay(unsigned ticks)
{
    (void)ticks;
    if (stop_after_schedule) longjmp(dispatch_exit, 1);
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

int shutdown(int fd, int how)
{
    assert(how == SHUT_RDWR);
    ++shutdowns;
    shutdown_fd = fd;
    return 0;
}

static void reset_(void)
{
    assert(atomic_load(&s_pending) == 0);
    memset(s_peers, 0, sizeof(s_peers));
    s_peers[0] = (peer_t){ .id = 42, .fd = 11 };
    s_server = (void *)1;
    s_outgoing = (void *)1;
    atomic_store(&s_ready, true);
    atomic_store(&s_work_pending, false);
    atomic_store(&s_dispatch_stop, false);
    queue_fail = send_fail = schedule_fail = stop_after_schedule = false;
    queued = sends = shutdowns = scheduled = 0;
    captured_fn = NULL;
    captured_arg = NULL;
}

static void run_one_dispatch_(void)
{
    stop_after_schedule = true;
    if (!setjmp(dispatch_exit)) dispatch_task_(NULL);
    stop_after_schedule = false;
}

static void test_full_queue_and_reused_fd_(void)
{
    reset_();
    assert(web_server_peer_alive(42));
    assert(!web_server_peer_alive(0));
    assert(!web_server_send(41, "abc", 3));
    for (unsigned i = 0; i < WEB_PENDING_LIMIT; ++i)
        assert(web_server_send(42, "abc", 3));
    assert(queued == WEB_PENDING_LIMIT);
    assert(atomic_load(&s_pending) == WEB_PENDING_LIMIT);
    assert(!web_server_send(42, "abc", 3));
    assert(atomic_load(&s_pending) == WEB_PENDING_LIMIT);

    /* A full output queue must not lose or duplicate the close request. */
    web_server_close(42);
    assert(!web_server_peer_alive(42));
    assert(next_close_() == 42);
    assert(next_close_() == -1);
    retry_close_(42);
    assert(next_close_() == 42);
    close_work_((void *)(intptr_t)42);
    assert(shutdowns == 1 && shutdown_fd == 11 && scheduled == 0);

    /* The same fd now belongs to a different logical connection. */
    s_peers[0] = (peer_t){ .id = 43, .fd = 11 };
    close_work_((void *)(intptr_t)42);
    assert(shutdowns == 1);
    while (queued) send_work_(queue[--queued]);
    assert(sends == 0 && atomic_load(&s_pending) == 0);
}

static void test_copy_and_callback_accounting_(void)
{
    reset_();
    char payload[] = "abc";
    assert(web_server_send(42, payload, 3));
    memset(payload, 'x', 3);
    run_one_dispatch_();
    assert(scheduled == 1 && queued == 0);
    assert(atomic_load(&s_pending) == 1 && atomic_load(&s_work_pending));

    /* The callback handed to HTTPD still consumes one of the eight slots. */
    for (unsigned i = 1; i < WEB_PENDING_LIMIT; ++i)
        assert(web_server_send(42, "abc", 3));
    assert(!web_server_send(42, "abc", 3));
    captured_fn(captured_arg); /* Asserts copied payload still equals abc. */
    assert(sends == 1 && !atomic_load(&s_work_pending));
    while (queued) discard_output_(queue[--queued]);
    assert(atomic_load(&s_pending) == 0);
}

static void test_error_cleanup_(void)
{
    reset_();
    queue_fail = true;
    assert(!web_server_send(42, "abc", 3));
    assert(atomic_load(&s_pending) == 0);

    reset_();
    assert(web_server_send(42, "abc", 3));
    run_one_dispatch_();
    send_fail = true;
    captured_fn(captured_arg);
    assert(sends == 1 && !web_server_peer_alive(42));
    assert(atomic_load(&s_pending) == 0 && !atomic_load(&s_work_pending));
    assert(scheduled == 1); /* Failure must not recursively queue a close. */

    reset_();
    assert(web_server_send(42, "abc", 3));
    schedule_fail = true;
    dispatch_task_(NULL);
    assert(atomic_load(&s_pending) == 0 && !atomic_load(&s_work_pending));
}

static void test_close_priority_and_retry_(void)
{
    reset_();
    assert(web_server_send(42, "abc", 3));
    web_server_close(42);
    run_one_dispatch_();
    assert(scheduled == 1 && captured_fn == close_work_ && queued == 1);
    captured_fn(captured_arg);
    discard_output_(queue[--queued]);

    reset_();
    web_server_close(42);
    schedule_fail = true;
    run_one_dispatch_();
    assert(scheduled == 1 && !s_peers[0].close_queued && s_peers[0].closing);
    assert(!atomic_load(&s_work_pending));
}

static void test_read_only_info_(void)
{
    s_server=NULL;s_outgoing=NULL;atomic_store(&s_ready,false);
    assert(web_server_start()==ESP_OK && server_port==80 && registrations==3);
    esp_err_t (*handler)(httpd_req_t *)=NULL;
    for(unsigned i=0;i<registrations;i++){
        assert(registered[i].method==HTTP_GET);
        assert(strcmp(registered[i].uri,"/api/v1/firmware")!=0);
        if(!strcmp(registered[i].uri,"/api/v1/info")){
            handler=registered[i].handler;assert(!registered[i].is_websocket);
        }
    }
    assert(handler==firmware_info_http_handler);
    /* Keep an existing browser owner and pending transport output intact. */
    reset_();assert(web_server_send(42,"abc",3));
    const unsigned tasks_before=task_creations;
    peer_t peers_before[WEB_PEERS];memcpy(peers_before,s_peers,sizeof(peers_before));
    for(unsigned i=0;i<100;i++){
        update_active=false;update_busy=false;info_send_fail=false;no_store=close_http=false;
        httpd_req_t req={.method=HTTP_GET};assert(handler(&req)==ESP_OK);
        assert(!req.sess_ctx && !req.free_ctx);
        assert(strstr(info_text,"version=1.3\n") && strstr(info_text,"slot=ota_1\n"));
        assert(strstr(info_text,"busy=0\n") && strstr(info_text,"update_mode=0\n"));
        assert(no_store && close_http);
        assert(!controller_calls && task_creations==tasks_before && queued==1);
        assert(atomic_load(&s_pending)==1 && !memcmp(peers_before,s_peers,sizeof(peers_before)));
    }
    httpd_req_t req={.method=HTTP_GET};
    update_active=update_busy=true;assert(handler(&req)==ESP_OK);
    assert(strstr(info_text,"busy=1\n") && strstr(info_text,"update_mode=1\n"));
    missing_partition=true;assert(handler(&req)==ESP_OK);assert(strstr(info_text,"slot=unknown\n"));
    missing_partition=false;info_send_fail=true;assert(handler(&req)==ESP_FAIL);
    assert(!controller_calls && queued==1 && atomic_load(&s_pending)==1);
    discard_output_(queue[--queued]);
}

int main(void)
{
    test_full_queue_and_reused_fd_();
    test_copy_and_callback_accounting_();
    test_error_cleanup_();
    test_close_priority_and_retry_();
    test_read_only_info_();
    puts("PASS: bounded queue, payload copy, error cleanup, logical FD identity, "
         "close retention/priority, callback accounting, retry and read-only identity during active work");
    return 0;
}
