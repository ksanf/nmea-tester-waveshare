#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "AISdecoder/aisdecoder_internal.h"
#include "freertos/task.h"

#define TEST_MAX_BITS 424U

static TickType_t s_tick = 1000U;

TickType_t xTaskGetTickCount(void)
{
    return s_tick;
}

static void fail_(const char *message)
{
    fprintf(stderr, "AIS decode test failed: %s\n", message);
    exit(EXIT_FAILURE);
}

static void expect_u32_(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: expected %u, got %u\n",
                name, (unsigned)expected, (unsigned)actual);
        exit(EXIT_FAILURE);
    }
}

static void expect_float_(const char *name, float actual, float expected, float tolerance)
{
    if (fabsf(actual - expected) > tolerance) {
        fprintf(stderr, "%s: expected %.5f, got %.5f\n", name, expected, actual);
        exit(EXIT_FAILURE);
    }
}

static void set_u_(uint8_t *bits, size_t offset, size_t length, uint32_t value)
{
    for (size_t i = 0; i < length; ++i) {
        bits[offset + i] = (uint8_t)((value >> (length - i - 1U)) & 1U);
    }
}

static void set_s_(uint8_t *bits, size_t offset, size_t length, int32_t value)
{
    const uint32_t mask = (1U << length) - 1U;
    set_u_(bits, offset, length, ((uint32_t)value) & mask);
}

static uint8_t text_value_(char c)
{
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A' + 1);
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0' + 48);
    return 32U;
}

static void set_text_(uint8_t *bits, size_t offset, size_t chars, const char *text)
{
    size_t text_len = strlen(text);

    for (size_t i = 0; i < chars; ++i) {
        uint8_t value = (i < text_len) ? text_value_(text[i]) : 32U;
        set_u_(bits, offset + (i * 6U), 6U, value);
    }
}

static void encode_payload_(const uint8_t *bits, size_t bit_count, char *payload, size_t payload_size)
{
    size_t chars = (bit_count + 5U) / 6U;

    if (payload_size <= chars) fail_("payload output buffer too small");
    for (size_t ch = 0; ch < chars; ++ch) {
        uint8_t value = 0U;
        for (size_t bit = 0; bit < 6U; ++bit) {
            size_t index = (ch * 6U) + bit;
            value = (uint8_t)((value << 1U) | ((index < bit_count) ? bits[index] : 0U));
        }
        payload[ch] = (char)(value + ((value < 40U) ? 48U : 56U));
    }
    payload[chars] = '\0';
}

static void feed_bits_(const uint8_t *bits, size_t bit_count)
{
    char payload[(TEST_MAX_BITS + 5U) / 6U + 1U];
    encode_payload_(bits, bit_count, payload, sizeof(payload));
    aisdecoder_decode_feed_payload(payload);
}

static const aisdecoder_target_t *find_target_(const aisdecoder_target_t *targets,
                                                size_t count,
                                                uint32_t mmsi)
{
    for (size_t i = 0; i < count; ++i) {
        if (targets[i].mmsi == mmsi) return &targets[i];
    }
    return NULL;
}

static void test_navigation_types_(void)
{
    uint8_t bits[TEST_MAX_BITS] = {0};
    aisdecoder_target_t targets[AISDEC_MAX_TARGETS];
    const aisdecoder_target_t *target;
    size_t count;

    aisdecoder_decode_reset();

    set_u_(bits, 0, 6, 9);
    set_u_(bits, 8, 30, 111222333U);
    set_u_(bits, 50, 10, 120U);
    set_s_(bits, 61, 28, 6300000);
    set_s_(bits, 89, 27, -12150000);
    set_u_(bits, 116, 12, 1234U);
    feed_bits_(bits, 168U);

    memset(bits, 0, sizeof(bits));
    set_u_(bits, 0, 6, 21U);
    set_u_(bits, 8, 30, 992345678U);
    set_u_(bits, 38, 5, 1U);
    set_text_(bits, 43, 20, "TEST BUOY");
    set_s_(bits, 164, 28, 84750000);
    set_s_(bits, 192, 27, -23100000);
    feed_bits_(bits, 272U);

    memset(bits, 0, sizeof(bits));
    set_u_(bits, 0, 6, 27U);
    set_u_(bits, 8, 30, 234567890U);
    set_u_(bits, 40, 4, 5U);
    set_s_(bits, 44, 18, -7500);
    set_s_(bits, 62, 17, 27150);
    set_u_(bits, 79, 6, 12U);
    set_u_(bits, 85, 9, 87U);
    feed_bits_(bits, 96U);

    memset(bits, 0, sizeof(bits));
    set_u_(bits, 0, 6, 28U);
    set_u_(bits, 8, 30, 991234567U);
    set_s_(bits, 44, 28, 90720000);
    set_s_(bits, 72, 27, -20310000);
    feed_bits_(bits, 168U);

    count = aisdecoder_decode_collect(targets, AISDEC_MAX_TARGETS, s_tick);
    expect_u32_("target count", (uint32_t)count, 4U);
    expect_u32_("decoded update count", aisdecoder_decode_updates(), 4U);

    target = find_target_(targets, count, 111222333U);
    if (!target) fail_("type 9 target missing");
    expect_u32_("type 9 message", target->msg_type, 9U);
    expect_float_("type 9 SOG", target->sog, 120.0f, 0.01f);
    expect_float_("type 9 COG", target->cog, 123.4f, 0.01f);
    expect_float_("type 9 longitude", target->lon, 10.5f, 0.0001f);
    expect_float_("type 9 latitude", target->lat, -20.25f, 0.0001f);
    if (strcmp(target->name, "SAR AIR") != 0) fail_("type 9 label mismatch");

    target = find_target_(targets, count, 992345678U);
    if (!target) fail_("type 21 target missing");
    if (strcmp(target->name, "TEST BUOY") != 0) fail_("type 21 name mismatch");
    expect_float_("type 21 longitude", target->lon, 141.25f, 0.0001f);
    expect_float_("type 21 latitude", target->lat, -38.5f, 0.0001f);

    target = find_target_(targets, count, 234567890U);
    if (!target) fail_("type 27 target missing");
    expect_u32_("type 27 status", target->nav_status, 5U);
    expect_float_("type 27 longitude", target->lon, -12.5f, 0.001f);
    expect_float_("type 27 latitude", target->lat, 45.25f, 0.001f);
    expect_float_("type 27 SOG", target->sog, 12.0f, 0.01f);
    expect_float_("type 27 COG", target->cog, 87.0f, 0.01f);

    target = find_target_(targets, count, 991234567U);
    if (!target) fail_("type 28 target missing");
    if (strcmp(target->name, "AtoN") != 0) fail_("type 28 label mismatch");
    expect_float_("type 28 longitude", target->lon, 151.2f, 0.0001f);
    expect_float_("type 28 latitude", target->lat, -33.85f, 0.0001f);

    memset(bits, 0, sizeof(bits));
    set_u_(bits, 0, 6, 14U);
    set_u_(bits, 8, 30, 555666777U);
    feed_bits_(bits, 40U);
    count = aisdecoder_decode_collect(targets, AISDEC_MAX_TARGETS, s_tick);
    expect_u32_("unsupported type target count", (uint32_t)count, 4U);
    expect_u32_("unsupported type update count", aisdecoder_decode_updates(), 4U);
}

static void test_target_capacity_(void)
{
    uint8_t bits[TEST_MAX_BITS] = {0};
    aisdecoder_target_t targets[AISDEC_MAX_TARGETS];
    size_t count;

    aisdecoder_decode_reset();
    for (uint32_t i = 0; i < AISDEC_MAX_TARGETS + 4U; ++i) {
        memset(bits, 0, sizeof(bits));
        set_u_(bits, 0, 6, 27U);
        set_u_(bits, 8, 30, 200000000U + i);
        set_s_(bits, 44, 18, (int32_t)i);
        set_s_(bits, 62, 17, (int32_t)i);
        set_u_(bits, 79, 6, 10U);
        set_u_(bits, 85, 9, 90U);
        s_tick++;
        feed_bits_(bits, 96U);
    }

    count = aisdecoder_decode_collect(targets, AISDEC_MAX_TARGETS, s_tick);
    expect_u32_("target capacity", (uint32_t)count, AISDEC_MAX_TARGETS);
    expect_u32_("capacity update count", aisdecoder_decode_updates(), AISDEC_MAX_TARGETS + 4U);
}

int main(void)
{
    test_navigation_types_();
    test_target_capacity_();
    aisdecoder_decode_deinit();
    puts("AIS navigation decode tests passed");
    return EXIT_SUCCESS;
}
