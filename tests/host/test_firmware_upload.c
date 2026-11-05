/* Real HTTP upload handler, isolated flash/HTTP/scheduler fakes.
 * Hash fake tests handler control flow; native Windows tests check real SHA-256. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../main/update/firmware_update.c"

static uint8_t input[10000],written[10000];
static size_t position,written_size,chunk;
static int begins,ends,aborts,selections,closes,status,fail_at,recv_calls;
static bool fail_begin,fail_write,fail_end,fail_select,cancel_read;
static int hash_fail;
static int64_t fake_us;
static int64_t time_per_read;
static char header[80],reply[200];
static esp_ota_img_states_t boot_state;
static esp_partition_t running={NMEA_APP0_OFFSET,NMEA_APP_SIZE,"ota_0"};
static esp_partition_t next={NMEA_APP1_OFFSET,NMEA_APP_SIZE,"ota_1"};
static httpd_req_t request;
static unsigned valid_marks,rollbacks,restarts,tasks;
static bool task_ok=true,ui_lock_ok=true,ui_timer_ok=true,controller_ok=true;
static void (*scheduled)(void *);

void mbedtls_sha256_init(mbedtls_sha256_context *c){memset(c,0,sizeof(*c));}
void mbedtls_sha256_free(mbedtls_sha256_context *c){(void)c;}
int mbedtls_sha256_starts(mbedtls_sha256_context *c,int mode){(void)mode;memset(c,0,sizeof(*c));return hash_fail==1?-1:0;}
int mbedtls_sha256_update(mbedtls_sha256_context *c,const uint8_t *p,size_t n){while(n--)c->bytes[c->count++%32]^=*p++;return hash_fail==2?-1:0;}
int mbedtls_sha256_finish(mbedtls_sha256_context *c,uint8_t *p){memcpy(p,c->bytes,32);return hash_fail==3?-1:0;}
size_t httpd_req_get_hdr_value_len(httpd_req_t *r,const char *k){(void)r;(void)k;return strlen(header);}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r,const char *k,char *p,size_t n){(void)r;(void)k;if(strlen(header)>=n)return ESP_FAIL;strcpy(p,header);return ESP_OK;}
int httpd_req_recv(httpd_req_t *r,char *out,size_t n){
    fake_us+=time_per_read;
    if(fail_at>=0 && recv_calls++==fail_at)return -1;
    if(n>chunk)n=chunk;
    if(n>r->content_len-position)n=r->content_len-position;
    memcpy(out,input+position,n);position+=n;
    if(cancel_read)atomic_store(&s_close,true);
    return (int)n;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r,const char *p){(void)r;(void)p;return ESP_OK;}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r,const char *a,const char *b){(void)r;(void)a;(void)b;return ESP_OK;}
esp_err_t httpd_resp_set_status(httpd_req_t *r,const char *p){(void)r;status=atoi(p);return ESP_OK;}
esp_err_t httpd_resp_sendstr(httpd_req_t *r,const char *p){(void)r;snprintf(reply,sizeof(reply),"%s",p);return ESP_OK;}
int httpd_req_to_sockfd(httpd_req_t *r){(void)r;return 7;}
esp_err_t httpd_sess_trigger_close(httpd_handle_t h,int fd){(void)h;assert(fd==7);closes++;return ESP_OK;}
const esp_partition_t *esp_ota_get_running_partition(void){return &running;}
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p){(void)p;return &next;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p,esp_ota_img_states_t *state){(void)p;*state=boot_state;return ESP_OK;}
esp_err_t esp_ota_begin(const esp_partition_t *p,size_t size,esp_ota_handle_t *handle){assert(p==&next && p->address!=running.address && size==OTA_WITH_SEQUENTIAL_WRITES);begins++;*handle=99;return fail_begin?ESP_FAIL:ESP_OK;}
esp_err_t esp_ota_write(esp_ota_handle_t h,const void *p,size_t n){assert(h==99 && n<=sizeof(written)-written_size);if(fail_write)return ESP_FAIL;memcpy(written+written_size,p,n);written_size+=n;return ESP_OK;}
esp_err_t esp_ota_abort(esp_ota_handle_t h){assert(h==99);aborts++;return ESP_OK;}
esp_err_t esp_ota_end(esp_ota_handle_t h){assert(h==99);ends++;return fail_end?ESP_FAIL:ESP_OK;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p){assert(p==&next);selections++;return fail_select?ESP_FAIL:ESP_OK;}
int64_t esp_timer_get_time(void){return fake_us;}
void vTaskDelay(unsigned n){(void)n;}
void vTaskDelete(void *p){(void)p;}
void esp_restart(void){restarts++;}
const char *esp_err_to_name(esp_err_t err){(void)err;return "test";}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void){valid_marks++;boot_state=ESP_OTA_IMG_VALID;return ESP_OK;}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void){rollbacks++;boot_state=ESP_OTA_IMG_INVALID;return ESP_OK;}
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle){
    (void)name;(void)stack;(void)arg;(void)priority;(void)handle;tasks++;scheduled=fn;return task_ok?pdPASS:0;
}
bool lvgl_port_lock(int timeout){(void)timeout;return ui_lock_ok;}
void lvgl_port_unlock(void){}
void lv_timer_del(lv_timer_t *t){(void)t;}
lv_timer_t *lv_timer_create(void (*fn)(lv_timer_t *),unsigned period,void *arg){
    (void)fn;(void)period;(void)arg;return ui_timer_ok?(lv_timer_t *)&request:NULL;
}
bool app_controller_healthy(void){return controller_ok;}

static void setup(void){
    position=written_size=0;chunk=37;begins=ends=aborts=selections=closes=recv_calls=0;status=200;fail_at=-1;
    fail_begin=fail_write=fail_end=fail_select=cancel_read=false;hash_fail=0;boot_state=ESP_OTA_IMG_VALID;
    fake_us=1234000;time_per_read=0;
    valid_marks=rollbacks=restarts=tasks=0;task_ok=ui_lock_ok=ui_timer_ok=controller_ok=true;scheduled=NULL;
    next.address=NMEA_APP1_OFFSET;next.size=NMEA_APP_SIZE;reply[0]=0;
    atomic_store(&s_active,true);atomic_store(&s_busy,false);atomic_store(&s_close,false);atomic_store(&s_reboot,false);
    memset(input,0,sizeof(input));input[0]=0xe9;input[1]=2;input[12]=9;
    nmea_write_le32(input+28,352);nmea_write_le32(input+32,0xabcd5432);
    strcpy((char *)input+48,"1.2");strcpy((char *)input+80,"nmea-tester");
    memcpy(input+288,"NMEATST1",8);strcpy((char *)input+296,NMEA_BOARD_ID);
    nmea_write_le32(input+328,NMEA_LAYOUT_VERSION);nmea_write_le32(input+332,NMEA_SETTINGS_SCHEMA);
    request=(httpd_req_t){.content_len=sizeof(input)};
    mbedtls_sha256_context h;uint8_t sum[32];mbedtls_sha256_starts(&h,0);mbedtls_sha256_update(&h,input,sizeof(input));mbedtls_sha256_finish(&h,sum);
    for(unsigned i=0;i<32;i++)snprintf(header+2*i,3,"%02x",sum[i]);
}
static void rejected(void){assert(upload_(&request)==ESP_OK);assert(status==400 && !atomic_load(&s_busy) && !atomic_load(&s_reboot) && closes==1);}
int main(void){
    setup();assert(upload_(&request)==ESP_OK);assert(status==200 && begins==1 && ends==1 && selections==1 && !aborts && !closes);
    assert(written_size==sizeof(input) && !memcmp(written,input,sizeof(input)) && atomic_load(&s_reboot) && !atomic_load(&s_busy));
    setup();atomic_store(&s_active,false);upload_(&request);assert(status==409 && !begins);
    setup();atomic_store(&s_busy,true);upload_(&request);assert(status==409 && atomic_load(&s_busy) && !begins);
    setup();request.content_len=383;rejected();assert(!begins);
    setup();request.content_len=NMEA_APP_SIZE+1;rejected();assert(!begins);
    setup();header[0]='z';rejected();assert(!begins);
    setup();header[63]=0;rejected();assert(!begins);
    setup();boot_state=ESP_OTA_IMG_PENDING_VERIFY;rejected();assert(!begins);
    setup();next.address=running.address;rejected();assert(!begins);
    setup();next.size=4096;rejected();assert(!begins);
    setup();input[296]='X';rejected();assert(!begins);
    setup();input[328]=1;rejected();assert(!begins);
    setup();cancel_read=true;rejected();assert(!begins);
    setup();fail_begin=true;rejected();assert(begins==1 && !aborts && !selections);
    setup();fail_write=true;rejected();assert(aborts==1 && !ends && !selections);
    setup();header[0]=header[0]=='0'?'1':'0';rejected();assert(aborts==1 && !ends && !selections);
    setup();fail_end=true;rejected();assert(ends==1 && !aborts && !selections);
    setup();fail_select=true;rejected();assert(ends==1 && selections==1 && !aborts);
    setup();time_per_read=31000000;rejected();assert(!begins && !selections);
    setup();chunk=384;time_per_read=31000000;rejected();assert(begins==1 && aborts==1 && !selections);
    for(int i=1;i<=3;i++){setup();hash_fail=i;rejected();assert(aborts==1 && !ends && !selections);}
    for(int i=0;i<30;i++){setup();fail_at=i;rejected();assert(!selections);assert(aborts==(begins?1:0));}
    setup();firmware_update_start_boot_guard();assert(!tasks);
    setup();boot_state=ESP_OTA_IMG_PENDING_VERIFY;firmware_update_start_boot_guard();assert(tasks==1 && scheduled==boot_guard_);
    scheduled(NULL);assert(rollbacks==1 && restarts==1);
    setup();boot_state=ESP_OTA_IMG_PENDING_VERIFY;firmware_update_start_boot_guard();boot_state=ESP_OTA_IMG_VALID;
    scheduled(NULL);assert(!rollbacks && !restarts);
    setup();boot_state=ESP_OTA_IMG_PENDING_VERIFY;task_ok=false;firmware_update_start_boot_guard();assert(rollbacks==1 && restarts==1);
    setup();boot_state=ESP_OTA_IMG_PENDING_VERIFY;firmware_update_boot_check(false);assert(rollbacks==1 && !valid_marks);
    setup();boot_state=ESP_OTA_IMG_PENDING_VERIFY;ui_timer_ok=false;firmware_update_boot_check(true);assert(rollbacks==1);
    setup();boot_state=ESP_OTA_IMG_PENDING_VERIFY;firmware_update_boot_check(true);assert(scheduled==boot_confirm_);
    atomic_store(&s_ui_ticks,3);scheduled(NULL);assert(valid_marks==1 && !rollbacks && !restarts);
    setup();boot_state=ESP_OTA_IMG_PENDING_VERIFY;atomic_store(&s_ui_ticks,2);boot_confirm_(NULL);assert(!valid_marks && rollbacks==1);
    setup();boot_state=ESP_OTA_IMG_PENDING_VERIFY;atomic_store(&s_ui_ticks,10);controller_ok=false;boot_confirm_(NULL);assert(!valid_marks && rollbacks==1);
    setup();boot_state=ESP_OTA_IMG_PENDING_VERIFY;atomic_store(&s_ui_ticks,10);ui_lock_ok=false;boot_confirm_(NULL);assert(!valid_marks && rollbacks==1);
    puts("PASS: OTA upload, 30 interruption boundaries, flash/hash/cancel/deadline errors, startup confirmation and rollback");
    return 0;
}
