/* Exercise the real L2 RX task and poll worker with concurrent host tasks.
 * Includes RTR with deliberately retained payload, task-create failure, an
 * in-flight blocked poll during timed stop, restart and repeated lifecycle. */
#define _POSIX_C_SOURCE 200809L
#include "l2_link.h"
#include "protocol_poll_worker.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct test_task {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    unsigned notifications;
    bool entered;
    void (*fn)(void *);
    void *arg;
};
static _Thread_local TaskHandle_t self;
static TaskHandle_t tasks[128];
static unsigned task_count;
static bool fail_create;
static atomic_uint forced_delete;
static void *thread_main(void *arg) {
    self=arg;
    pthread_mutex_lock(&self->lock);
    self->entered=true; pthread_cond_broadcast(&self->wake);
    pthread_mutex_unlock(&self->lock);
    self->fn(self->arg);
    assert(!"task returned instead of self-delete"); return NULL;
}
BaseType_t xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,
                       void *arg,unsigned priority,TaskHandle_t *out) {
    (void)name; (void)stack; (void)priority;
    if(fail_create) return pdFALSE;
    assert(task_count<128);
    TaskHandle_t task=calloc(1,sizeof(*task)); assert(task);
    assert(!pthread_mutex_init(&task->lock,NULL));
    assert(!pthread_cond_init(&task->wake,NULL));
    task->fn=fn; task->arg=arg; tasks[task_count++]=task;
    assert(!pthread_create(&task->thread,NULL,thread_main,task));
    /* Deliberately schedule the created task before publishing its handle. */
    pthread_mutex_lock(&task->lock);
    while(!task->entered) pthread_cond_wait(&task->wake,&task->lock);
    pthread_mutex_unlock(&task->lock);
    *out=task; return pdPASS;
}
void vTaskDelete(TaskHandle_t task) {
    if(task) atomic_fetch_add(&forced_delete,1);
    assert(!task); pthread_exit(NULL);
}
void vTaskDelay(TickType_t ms) {
    struct timespec delay={.tv_sec=ms/1000,.tv_nsec=(long)(ms%1000)*1000000};
    nanosleep(&delay,NULL);
}
TickType_t xTaskGetTickCount(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    return (TickType_t)((uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000);
}
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t wait) {
    assert(self && clear==pdTRUE && wait==portMAX_DELAY);
    pthread_mutex_lock(&self->lock);
    while(!self->notifications) pthread_cond_wait(&self->wake,&self->lock);
    unsigned n=self->notifications; self->notifications=0;
    pthread_mutex_unlock(&self->lock); return n;
}
void xTaskNotifyGive(TaskHandle_t task) {
    pthread_mutex_lock(&task->lock);
    ++task->notifications; pthread_cond_signal(&task->wake);
    pthread_mutex_unlock(&task->lock);
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    SemaphoreHandle_t p=malloc(sizeof(*p)); assert(p);
    assert(!pthread_mutex_init(p,NULL)); return p;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t p,TickType_t wait) {
    assert(wait==portMAX_DELAY); assert(!pthread_mutex_lock(p)); return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t p) {
    assert(!pthread_mutex_unlock(p)); return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t p) {
    assert(!pthread_mutex_destroy(p)); free(p);
}
static void join_tasks(void) {
    for(unsigned i=0;i<task_count;i++) {
        assert(!pthread_join(tasks[i]->thread,NULL));
        pthread_cond_destroy(&tasks[i]->wake);
        pthread_mutex_destroy(&tasks[i]->lock); free(tasks[i]);
    }
    task_count=0;
}
static void wait_at_least(atomic_uint *value,unsigned n) {
    TickType_t start=xTaskGetTickCount();
    while(atomic_load(value)<n) {
        assert(xTaskGetTickCount()-start<2000); vTaskDelay(1);
    }
}

static twai_message_t frames[16];
static unsigned frame_count;
static atomic_uint read_index;
static atomic_uint received;
static sp_l2_frame_t accepted[16];
esp_err_t can_driver_receive(twai_message_t *msg,uint32_t timeout_ms) {
    unsigned i=atomic_fetch_add(&read_index,1);
    if(i<frame_count) {
        /* ESP-IDF 5.5 twai_receive_v2 writes only these fields. Poison the
         * unsupported bits to make accidental flags/DLC_NON_COMP use fail
         * deterministically, regardless of the host stack's initial contents. */
        assert(msg->flags == 0); /* Every receive starts with a clean object. */
        msg->flags = 0xfffffffcU;
        msg->identifier = frames[i].identifier;
        msg->data_length_code = frames[i].data_length_code;
        msg->extd = frames[i].extd;
        msg->rtr = frames[i].rtr;
        if (!msg->rtr) memcpy(msg->data, frames[i].data, sizeof(msg->data));
        return ESP_OK;
    }
    vTaskDelay(timeout_ms); return ESP_ERR_TIMEOUT;
}
esp_err_t can_driver_send(const twai_message_t *msg,uint32_t timeout_ms) {
    (void)msg; (void)timeout_ms; return ESP_OK;
}
static void on_rx(const sp_l2_frame_t *frame,void *user) {
    assert(user==accepted);
    unsigned n=atomic_load(&received); assert(n<16);
    accepted[n]=*frame; atomic_store(&received,n+1);
}
static void test_l2_frames(void) {
    frames[0]=(twai_message_t){.identifier=0x19ef0007,.flags=TWAI_MSG_FLAG_EXTD,
        .data_length_code=8,.data={0,6,0x5f,0x99,2,0,0,0x13}};
    frame_count=8;
    for(unsigned i=1;i<frame_count;i++) frames[i]=frames[0];
    frames[1].flags|=TWAI_MSG_FLAG_RTR;
    frames[2].flags=0;
    frames[3].data_length_code=9;
    /* A normal frame remains valid even when unused flags contain bit 0x10. */
    frames[4].data_length_code=2;
    frames[5].identifier=0x20000000;
    frames[6].data_length_code=0;
    frames[7].data_length_code=3;
    sp_l2_config_t cfg={0};
    atomic_store(&read_index,0); atomic_store(&received,0);
    assert(sp_l2_init(&cfg,on_rx,NULL,accepted)==SP_OK);
    wait_at_least(&read_index,frame_count+1);
    assert(sp_l2_deinit()); join_tasks();
    assert(atomic_load(&received)==4);
    assert(accepted[0].id.sa==7 && accepted[0].id.dp==1 && accepted[0].id.pf==0xef);
    assert(accepted[0].dlc==8 && !memcmp(accepted[0].data,frames[0].data,8));
    assert(accepted[1].dlc==2 && !memcmp(accepted[1].data,frames[4].data,2));
    assert(accepted[2].dlc==0);
    for(unsigned i=0;i<8;i++) assert(accepted[2].data[i]==0);
    assert(accepted[3].dlc==3);
    for(unsigned i=3;i<8;i++) assert(accepted[3].data[i]==0);
    fail_create=true; assert(sp_l2_init(&cfg,on_rx,NULL,accepted)==SP_E_NOMEM);
    fail_create=false; assert(sp_l2_deinit());
}

static atomic_uint polls;
static atomic_bool hold_poll;
static atomic_bool in_poll;
static void poll_callback(void) {
    assert(!atomic_exchange(&in_poll,true));
    atomic_fetch_add(&polls,1);
    while(atomic_load(&hold_poll)) vTaskDelay(1);
    atomic_store(&in_poll,false);
}
static void test_worker_lifecycle(void) {
    assert(!protocol_poll_worker_start(NULL));
    fail_create=true;
    assert(!protocol_poll_worker_start(poll_callback));
    assert(!protocol_poll_worker_is_running());
    fail_create=false;
    atomic_store(&hold_poll,true);
    assert(protocol_poll_worker_start(poll_callback));
    wait_at_least(&polls,1);
    assert(protocol_poll_worker_start(poll_callback));
    assert(!protocol_poll_worker_stop(25));
    assert(protocol_poll_worker_is_running() && atomic_load(&in_poll));
    assert(!protocol_poll_worker_start(poll_callback));
    assert(atomic_load(&polls)==1); /* No overlapping or queued duplicate poll. */
    atomic_store(&hold_poll,false);
    assert(protocol_poll_worker_stop(500));
    assert(!protocol_poll_worker_is_running() && !atomic_load(&in_poll));
    join_tasks();
    for(unsigned i=0;i<20;i++) {
        unsigned before=atomic_load(&polls);
        assert(protocol_poll_worker_start(poll_callback));
        wait_at_least(&polls,before+1);
        assert(protocol_poll_worker_stop(500));
        join_tasks();
        unsigned after=atomic_load(&polls); vTaskDelay(2);
        assert(atomic_load(&polls)==after && !atomic_load(&in_poll));
    }
    assert(protocol_poll_worker_stop(0));
    assert(!atomic_load(&forced_delete));
}
int main(void) {
    test_l2_frames(); test_worker_lifecycle();
    puts("CAN link/worker: PASS (real RX task, RTR/DLC filter, creation failure, blocked stop, safe restart)");
    return 0;
}
