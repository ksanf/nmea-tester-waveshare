/* Runs the actual clock initialization/worker with deterministic RTC, CPU
 * clock and task scheduling. No wall-clock changes or attached hardware. */
#include "system/nmea_clock.h"
#include "system/rtc_driver.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "config/config_nmea_tester.h"
#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static char saved_utc[7]="invalid", saved_date[7]="invalid";
static char published_utc[7], published_date[7];
static unsigned updates, cpu_writes, rtc_writes;
static time_t cpu_now, rtc_now;
static int cpu_set_error;
static esp_err_t rtc_read_error=ESP_FAIL, rtc_write_error=ESP_FAIL;
static bool malformed_rtc;
static void (*worker)(void *);
static _Thread_local jmp_buf tick_done;
static _Thread_local unsigned clock_lock_depth;
struct clock_test_mutex { pthread_mutex_t mutex; };
static bool allocation_fail;
static pthread_mutex_t probe_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t probe_cond = PTHREAD_COND_INITIALIZER;
static unsigned lock_attempts;
static bool block_read, read_entered, release_read;

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    if (allocation_fail) return NULL;
    SemaphoreHandle_t mutex = malloc(sizeof(*mutex));
    assert(mutex && pthread_mutex_init(&mutex->mutex, NULL) == 0);
    return mutex;
}
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait) {
    assert(wait == portMAX_DELAY && clock_lock_depth == 0);
    assert(pthread_mutex_lock(&probe_mutex) == 0);
    ++lock_attempts; pthread_cond_broadcast(&probe_cond);
    assert(pthread_mutex_unlock(&probe_mutex) == 0);
    assert(pthread_mutex_lock(&mutex->mutex) == 0);
    ++clock_lock_depth;
    return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t mutex) {
    assert(clock_lock_depth == 1);
    --clock_lock_depth;
    assert(pthread_mutex_unlock(&mutex->mutex) == 0);
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t mutex) {
    assert(pthread_mutex_destroy(&mutex->mutex) == 0); free(mutex);
}
static void pause_read_if_requested(void) {
    assert(clock_lock_depth == 1);
    assert(pthread_mutex_lock(&probe_mutex) == 0);
    if (block_read) {
        read_entered = true; pthread_cond_broadcast(&probe_cond);
        while (!release_read) pthread_cond_wait(&probe_cond, &probe_mutex);
    }
    assert(pthread_mutex_unlock(&probe_mutex) == 0);
}

void clock_test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void nmea_templates_gps_time_snapshot(char *utc,char *date) {
    assert(clock_lock_depth == 1);
    memcpy(utc,saved_utc,7); memcpy(date,saved_date,7);
}
void nmea_templates_gps_time_update(const char *utc,const char *date) {
    assert(clock_lock_depth == 1);
    memcpy(published_utc,utc,7); memcpy(published_date,date,7); ++updates;
}
esp_err_t app_rtc_init(void) { assert(clock_lock_depth == 1); return rtc_read_error; }
esp_err_t rtc_get_time_tm(struct tm *out) {
    assert(clock_lock_depth == 1);
    if (rtc_read_error!=ESP_OK) return rtc_read_error;
    assert(gmtime_r(&rtc_now,out));
    pause_read_if_requested();
    if (malformed_rtc) { out->tm_mon=1; out->tm_mday=31; }
    return ESP_OK;
}
esp_err_t rtc_set_time_epoch(time_t epoch) {
    assert(clock_lock_depth == 1);
    ++rtc_writes;
    if (rtc_write_error!=ESP_OK) return rtc_write_error;
    rtc_now=epoch; return ESP_OK;
}
time_t clock_test_time(time_t *out) {
    assert(clock_lock_depth == 1);
    time_t read = cpu_now;
    pause_read_if_requested();
    if(out)*out=read;
    return read;
}
int clock_test_settimeofday(const struct timeval *tv,const struct timezone *zone) {
    assert(clock_lock_depth == 1);
    (void)zone; ++cpu_writes;
    if(cpu_set_error) return -1;
    cpu_now=tv->tv_sec; return 0;
}
TickType_t xTaskGetTickCount(void) { return 0; }
void vTaskDelayUntil(TickType_t *tick,TickType_t period) {
    (void)tick; assert(period==1000 && clock_lock_depth == 0); longjmp(tick_done,1);
}
int xTaskCreatePinnedToCore(void (*task)(void *),const char *name,unsigned stack,
                          void *arg,unsigned priority,void *handle,int core) {
    (void)name;(void)stack;(void)arg;(void)priority;(void)handle;(void)core;
    worker=task; return pdPASS;
}
static void one_tick(void) {
    assert(worker);
    if(setjmp(tick_done)==0) worker(NULL);
}
static time_t epoch(int y,int m,int d,int h,int min,int sec) {
    struct tm t={.tm_year=y-1900,.tm_mon=m-1,.tm_mday=d,.tm_hour=h,.tm_min=min,.tm_sec=sec};
    return timegm(&t);
}
static void stored(const char *utc,const char *date) {
    memcpy(saved_utc,utc,7); memcpy(saved_date,date,7);
}
static void published(const char *utc,const char *date) {
    assert(!strcmp(published_utc,utc)); assert(!strcmp(published_date,date));
}
static void *reader_thread(void *unused) {
    (void)unused; one_tick(); return NULL;
}
typedef struct { time_t value; esp_err_t result; } writer_arg_t;
static void *writer_thread(void *arg) {
    writer_arg_t *write = arg;
    write->result = nmea_clock_set_time_epoch(write->value);
    return NULL;
}
static void test_manual_set_serializes_with_old_read(void) {
    rtc_write_error=ESP_OK; rtc_read_error=ESP_OK;
    rtc_now=cpu_now=epoch(2026,10,3,1,2,3);
    unsigned before_cpu=cpu_writes, before_rtc=rtc_writes;
    assert(pthread_mutex_lock(&probe_mutex)==0);
    block_read=true; read_entered=false; release_read=false;
    assert(pthread_mutex_unlock(&probe_mutex)==0);
    pthread_t reader, writer;
    assert(pthread_create(&reader,NULL,reader_thread,NULL)==0);
    assert(pthread_mutex_lock(&probe_mutex)==0);
    while(!read_entered) pthread_cond_wait(&probe_cond,&probe_mutex);
    unsigned before_attempt=lock_attempts;
    assert(pthread_mutex_unlock(&probe_mutex)==0);
    writer_arg_t edit={epoch(2026,10,4,12,0,0),ESP_FAIL};
    assert(pthread_create(&writer,NULL,writer_thread,&edit)==0);
    assert(pthread_mutex_lock(&probe_mutex)==0);
    while(lock_attempts==before_attempt) pthread_cond_wait(&probe_cond,&probe_mutex);
    /* The worker holds a pre-edit reading. The setter has tried to lock but
     * must not change either clock until that older publication finishes. */
    assert(cpu_writes==before_cpu && rtc_writes==before_rtc);
    release_read=true; pthread_cond_broadcast(&probe_cond);
    assert(pthread_mutex_unlock(&probe_mutex)==0);
    assert(pthread_join(reader,NULL)==0 && pthread_join(writer,NULL)==0);
    assert(edit.result==ESP_OK && cpu_now==edit.value);
    published("120000","041026");
    block_read=false;
    /* A failed first RTC read after a successful explicit edit must already
     * use the new CPU backup, with no one-second synchronization gap. */
    rtc_read_error=ESP_FAIL; cpu_now+=2;
    one_tick(); published("120002","041026");
}

int main(void) {
    allocation_fail=true;
    assert(nmea_clock_set_time_epoch(epoch(2026,10,2,0,0,0))==ESP_ERR_NO_MEM);
    assert(cpu_writes==0 && rtc_writes==0);
    allocation_fail=false;
    /* First boot with no trustworthy time must not publish epoch 1970. */
    nmea_clock_init(); one_tick(); assert(updates==0 && cpu_writes==0);
    setenv("TZ","Pacific/Honolulu",1); tzset();
#if NMEA_CLOCK_FROM_RTC
    rtc_read_error=ESP_OK; rtc_now=epoch(2026,10,2,23,59,58);
    nmea_clock_init(); assert(cpu_now==rtc_now); published("235958","021026");
    /* A read failure across midnight holds correct UTC from the CPU clock. */
    rtc_read_error=ESP_FAIL; cpu_now+=3; one_tick(); published("000001","031026");
    cpu_now+=60; one_tick(); published("000101","031026");
    /* Recovery adopts RTC time and re-anchors the CPU fallback. */
    rtc_read_error=ESP_OK; rtc_now=epoch(2026,10,3,1,2,3); one_tick();
    assert(cpu_now==rtc_now); published("010203","031026");
    /* Calendar-invalid RTC data must not be normalized/published. */
    malformed_rtc=true; cpu_now+=1; one_tick(); published("010204","031026");
    malformed_rtc=false;
    /* A broken RTC at boot still gets a running CPU clock from stored UTC. */
    stored("235959","280224"); rtc_read_error=ESP_FAIL; rtc_write_error=ESP_FAIL;
    nmea_clock_init(); assert(cpu_now==epoch(2024,2,28,23,59,59));
    cpu_now+=2; one_tick(); published("000001","290224");
    unsigned writes=cpu_writes; time_t before=cpu_now;
    stored("120000","041026"); assert(!nmea_clock_sync_from_template());
    assert(cpu_writes==writes && cpu_now==before);
    rtc_write_error=ESP_OK;
#else
    stored("235959","280224"); nmea_clock_init();
    assert(cpu_now==epoch(2024,2,28,23,59,59));
    cpu_now+=2; one_tick(); published("000001","290224");
#endif
    /* An explicit successful edit updates backup in UTC regardless of TZ. */
    stored("120000","041026"); assert(nmea_clock_sync_from_template());
    assert(cpu_now==epoch(2026,10,4,12,0,0));
    time_t before_bad=cpu_now;
    stored("120000","310226"); assert(!nmea_clock_sync_from_template());
    assert(cpu_now==before_bad);
    stored("120000","041026"); cpu_set_error=1;
    assert(!nmea_clock_sync_from_template()); cpu_set_error=0;
    unsigned before_invalid_cpu=cpu_writes, before_invalid_rtc=rtc_writes;
    assert(nmea_clock_set_time_epoch(epoch(1999,12,31,23,59,59))==ESP_ERR_INVALID_ARG);
    assert(nmea_clock_set_time_epoch(epoch(2100,1,1,0,0,0))==ESP_ERR_INVALID_ARG);
    assert(cpu_writes==before_invalid_cpu && rtc_writes==before_invalid_rtc);
    test_manual_set_serializes_with_old_read();
    /* Never translate an invalid CPU epoch into a plausible DDMMYY date. */
    rtc_read_error=ESP_FAIL; cpu_now=0; unsigned count=updates;
    one_tick(); assert(updates==count);
    puts(NMEA_CLOCK_FROM_RTC ? "RTC clock: fallback, failure/recovery, serialized edits and UTC PASS" :
                              "CPU clock: validity, leap day, serialized edits and UTC PASS");
    return 0;
}
