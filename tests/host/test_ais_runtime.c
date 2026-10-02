#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "AISdecoder/aisdecoder_runtime.h"
#include "freertos/task.h"

static _Atomic uint32_t s_tick = 1000;
TickType_t xTaskGetTickCount(void) { return atomic_load(&s_tick); }
static char s_line[192];
static void bits_(uint8_t *out, unsigned at, unsigned count, uint32_t value)
{
    for (unsigned i = 0; i < count; ++i) out[at + i] = (value >> (count - i - 1)) & 1U;
}
static void make_payload_(char *out)
{
    uint8_t bits[168] = {0};
    bits_(bits, 0, 6, 1); bits_(bits, 8, 30, 123456789);
    bits_(bits, 50, 10, 123); bits_(bits, 61, 28, 11 * 600000);
    bits_(bits, 89, 27, 48 * 600000); bits_(bits, 116, 12, 900);
    for (unsigned i = 0; i < 28; ++i) {
        unsigned value = 0;
        for (unsigned b = 0; b < 6; b++) value = (value << 1) | bits[i*6+b];
        out[i] = (char)(value + (value < 40 ? 48 : 56));
    }
    out[28] = 0;
}
static void frame_(char *out, const char *body)
{
    unsigned checksum = 0;
    for (const char *p = body + 1; *p; ++p) checksum ^= (unsigned char)*p;
    snprintf(out, 192, "%s*%02X", body, checksum);
}
static size_t collect_(void)
{
    aisdecoder_target_t t[4];
    size_t n = aisdecoder_runtime_snapshot(t, 4, NULL);
    if (n) { assert(t[0].mmsi == 123456789); assert(t[0].lat == 48); assert(t[0].lon == 11); }
    return n;
}
static void lifecycle_(void)
{
    aisdecoder_runtime_status_t status;
    char payload[29], body[128], first[192], second[192];
    make_payload_(payload);
    snprintf(body, sizeof(body), "!AIVDM,1,1,,A,%s,0", payload);
    frame_(s_line, body);
    aisdecoder_runtime_feed(s_line);
    assert(collect_() == 0);
    assert(aisdecoder_runtime_start());
    aisdecoder_runtime_feed(s_line);
    assert(collect_() == 1);
    assert(aisdecoder_runtime_start()); /* opening another consumer keeps data */
    assert(collect_() == 1);
    aisdecoder_runtime_status(&status);
    assert(status.active && status.lines_seen == 1 && status.decoded_updates == 1);
    assert(strcmp(status.last_line, s_line) == 0);
    atomic_store(&s_tick, 1000 + AISDEC_STALE_MS + 1);
    assert(collect_() == 0);
    aisdecoder_runtime_stop();
    aisdecoder_runtime_status(&status);
    assert(!status.active && collect_() == 0);
    atomic_store(&s_tick, 2000);
    assert(aisdecoder_runtime_start());
    snprintf(body, sizeof(body), "!AIVDM,2,1,1,A,%.14s,0", payload);
    frame_(first, body);
    snprintf(body, sizeof(body), "!AIVDM,2,2,1,A,%s,0", payload + 14);
    frame_(second, body);
    aisdecoder_runtime_feed(first); assert(collect_() == 0);
    aisdecoder_runtime_stop(); assert(aisdecoder_runtime_start());
    aisdecoder_runtime_feed(second); assert(collect_() == 0); /* no old fragment */
    aisdecoder_runtime_feed(first); aisdecoder_runtime_feed(second);
    assert(collect_() == 1);
    aisdecoder_runtime_stop();
}
static void *writer_(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 2000; i++) aisdecoder_runtime_feed(s_line);
    return NULL;
}
static void concurrent_(void)
{
    pthread_t writer;
    assert(aisdecoder_runtime_start());
    assert(pthread_create(&writer, NULL, writer_, NULL) == 0);
    for (unsigned i = 0; i < 2000; i++) { assert(collect_() <= 1); }
    pthread_join(writer, NULL);
    uint32_t updates;
    aisdecoder_target_t target;
    assert(aisdecoder_runtime_snapshot(&target, 1, &updates) == 1);
    assert(updates == 2000);
    aisdecoder_runtime_stop();
}
int main(void)
{
    lifecycle_(); concurrent_();
    puts("AIS runtime lifecycle, fragment reset, freshness and concurrency tests passed");
    return 0;
}
