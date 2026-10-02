#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/nmea_log_markup.h"

typedef enum {
    MARKUP_WAIT,
    MARKUP_PARAMETER,
    MARKUP_TEXT,
} markup_state_t;

static uint16_t fixed_width_(unsigned char current,
                             unsigned char next,
                             void *context)
{
    (void)current;
    (void)next;
    return *(const uint16_t *)context;
}

static bool decode_markup_(const char *src, char *dst, size_t dst_size)
{
    markup_state_t state = MARKUP_WAIT;
    size_t dst_len = 0u;

    if (!src || !dst || dst_size == 0u) return false;

    while (*src) {
        const char c = *src++;

        if (c == '\n') {
            if (state != MARKUP_WAIT || dst_len + 1u >= dst_size) return false;
            dst[dst_len++] = c;
            continue;
        }

        if (c == '#') {
            if (state == MARKUP_WAIT) {
                state = MARKUP_PARAMETER;
                continue;
            }
            if (state == MARKUP_PARAMETER) {
                state = MARKUP_WAIT;
            } else {
                state = MARKUP_WAIT;
                continue;
            }
        } else if (state == MARKUP_PARAMETER) {
            if (c == ' ') state = MARKUP_TEXT;
            continue;
        }

        if (dst_len + 1u >= dst_size) return false;
        dst[dst_len++] = c;
    }

    if (state != MARKUP_WAIT) return false;
    dst[dst_len] = '\0';
    return true;
}

static void test_wrap_closes_every_physical_line_(void)
{
    static const char prefix[] = "$";
    static const char talker[] = "HEHDT";
    static const char fields[] = ",123.4,T";
    static const char checksum[] = "*00";
    const uint16_t glyph_width = 10u;
    const nmea_log_markup_span_t spans[] = {
        {prefix, strlen(prefix), UINT32_C(0xF0F0F0)},
        {talker, strlen(talker), UINT32_C(0xFFFF00)},
        {fields, strlen(fields), UINT32_C(0x00FF00)},
        {checksum, strlen(checksum), UINT32_C(0xFF0000)},
    };
    char formatted[256];
    char visible[128];

    assert(nmea_log_markup_format(formatted, sizeof(formatted),
                                  spans, 4u, 50u, 0,
                                  fixed_width_, (void *)&glyph_width));
    assert(decode_markup_(formatted, visible, sizeof(visible)));
    assert(strcmp(visible, "$HEHD\nT,123\n.4,T*\n00") == 0);
}

static void test_literal_hash_is_preserved_(void)
{
    static const char text[] = "ABC#12";
    const uint16_t glyph_width = 8u;
    const nmea_log_markup_span_t span = {
        text, strlen(text), UINT32_C(0x00FF00)
    };
    char formatted[128];
    char visible[128];

    assert(nmea_log_markup_format(formatted, sizeof(formatted),
                                  &span, 1u, 200u, 0,
                                  fixed_width_, (void *)&glyph_width));
    assert(decode_markup_(formatted, visible, sizeof(visible)));
    assert(strcmp(visible, text) == 0);
}

static void test_plain_hash_escape_(void)
{
    char escaped[128];
    char visible[128];

    assert(nmea_log_markup_escape_plain(escaped, sizeof(escaped),
                                        "noise #F0F0F0 tail"));
    assert(decode_markup_(escaped, visible, sizeof(visible)));
    assert(strcmp(visible, "noise #F0F0F0 tail") == 0);
}

static void test_small_destination_fails_cleanly_(void)
{
    static const char text[] = "1234567890";
    const uint16_t glyph_width = 8u;
    const nmea_log_markup_span_t span = {
        text, strlen(text), UINT32_C(0x123456)
    };
    char formatted[12];

    assert(!nmea_log_markup_format(formatted, sizeof(formatted),
                                   &span, 1u, 200u, 0,
                                   fixed_width_, (void *)&glyph_width));
    assert(formatted[0] == '\0');
}

int main(void)
{
    test_wrap_closes_every_physical_line_();
    test_literal_hash_is_preserved_();
    test_plain_hash_escape_();
    test_small_destination_fails_cleanly_();
    puts("NMEA log markup tests passed");
    return 0;
}
