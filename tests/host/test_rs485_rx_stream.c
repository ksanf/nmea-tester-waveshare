#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rs485/rs485_rx_stream.h"

#define MAX_EVENTS 16u

typedef struct {
    rs485_rx_stream_event_kind_t kind;
    char line[RS485_RX_STREAM_NMEA_CAPACITY];
    size_t len;
} captured_event_t;

typedef struct {
    captured_event_t events[MAX_EVENTS];
    size_t count;
} capture_t;

static void fail_(const char *test, const char *detail)
{
    fprintf(stderr, "%s: %s\n", test, detail);
    exit(EXIT_FAILURE);
}

static void capture_emit_(rs485_rx_stream_event_kind_t kind,
                          const char *line,
                          size_t len,
                          void *user)
{
    capture_t *capture = user;
    captured_event_t *event;

    if (capture->count >= MAX_EVENTS) fail_("capture", "too many events");
    if (len >= sizeof(capture->events[0].line)) fail_("capture", "line too long");

    event = &capture->events[capture->count++];
    event->kind = kind;
    event->len = len;
    memcpy(event->line, line, len);
    event->line[len] = '\0';
}

static void expect_event_(const char *test,
                          const capture_t *capture,
                          size_t index,
                          rs485_rx_stream_event_kind_t kind,
                          const char *line)
{
    if (index >= capture->count) fail_(test, "missing event");
    if (capture->events[index].kind != kind) fail_(test, "wrong event kind");
    if (strcmp(capture->events[index].line, line) != 0) {
        fprintf(stderr, "%s: expected '%s', actual '%s'\n",
                test, line, capture->events[index].line);
        exit(EXIT_FAILURE);
    }
}

static void test_fragmented_nmea_(void)
{
    rs485_rx_stream_t stream;
    capture_t capture = {0};
    static const uint8_t first[] = {0x00, 0x1F, 'a', 'b', 'c', '\r', '$', 'H', 'E'};
    static const uint8_t second[] = "HDT,26.8,T*13\r\n";

    rs485_rx_stream_init(&stream, false);
    rs485_rx_stream_feed(&stream, first, sizeof(first), capture_emit_, &capture);
    rs485_rx_stream_feed(&stream, second, sizeof(second) - 1u, capture_emit_, &capture);

    if (capture.count != 1u) fail_("fragmented NMEA", "wrong event count");
    expect_event_("fragmented NMEA", &capture, 0,
                  RS485_RX_STREAM_EVENT_NMEA, "$HEHDT,26.8,T*13");
}

static void test_delimiters_and_multiple_lines_(void)
{
    rs485_rx_stream_t stream;
    capture_t capture = {0};
    static const uint8_t data[] = {
        ' ', '$', 'G', 'P', 'V', 'T', 'G', ',', '1', 0x1F,
        '!', 'A', 'I', 'V', 'D', 'M', ',', '2', 0x7F,
    };

    rs485_rx_stream_init(&stream, false);
    rs485_rx_stream_feed(&stream, data, sizeof(data), capture_emit_, &capture);

    if (capture.count != 2u) fail_("delimiters", "wrong event count");
    expect_event_("control delimiter", &capture, 0,
                  RS485_RX_STREAM_EVENT_NMEA, "$GPVTG,1");
    expect_event_("DEL delimiter", &capture, 1,
                  RS485_RX_STREAM_EVENT_NMEA, "!AIVDM,2");
}

static void test_text_fields_keep_spaces_(void)
{
    rs485_rx_stream_t stream;
    capture_t capture = {0};
    static const uint8_t data[] =
        "$AIALR,012026,025,A,V,AIS: External lost*24\r"
        "$AITXT,01,01,07,AIS: lost*38\n";

    rs485_rx_stream_init(&stream, false);
    rs485_rx_stream_feed(&stream, data, sizeof(data) - 1u,
                         capture_emit_, &capture);

    if (capture.count != 2u) fail_("text spaces", "wrong event count");
    expect_event_("ALR spaces", &capture, 0, RS485_RX_STREAM_EVENT_NMEA,
                  "$AIALR,012026,025,A,V,AIS: External lost*24");
    expect_event_("TXT spaces", &capture, 1, RS485_RX_STREAM_EVENT_NMEA,
                  "$AITXT,01,01,07,AIS: lost*38");
}

static void test_hex_lines_and_offsets_(void)
{
    rs485_rx_stream_t stream;
    capture_t capture = {0};
    uint8_t data[32];

    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)i;

    rs485_rx_stream_init(&stream, true);
    rs485_rx_stream_feed(&stream, data, 7, capture_emit_, &capture);
    if (capture.count != 0u) fail_("partial HEX", "line emitted too early");
    rs485_rx_stream_feed(&stream, data + 7, sizeof(data) - 7u, capture_emit_, &capture);

    if (capture.count != 2u) fail_("HEX offsets", "wrong event count");
    expect_event_("HEX offset 0", &capture, 0, RS485_RX_STREAM_EVENT_HEX,
                  "0000: 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F  |................|");
    expect_event_("HEX offset 16", &capture, 1, RS485_RX_STREAM_EVENT_HEX,
                  "0010: 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F  |................|");
}

static void test_hex_keeps_nmea_decoder_(void)
{
    rs485_rx_stream_t stream;
    capture_t capture = {0};
    static const uint8_t data[] = "$HEHDT,26.8,T*13\r";

    rs485_rx_stream_init(&stream, true);
    rs485_rx_stream_feed(&stream, data, sizeof(data) - 1u, capture_emit_, &capture);

    if (capture.count != 2u) fail_("HEX plus NMEA", "wrong event count");
    if (capture.events[0].kind != RS485_RX_STREAM_EVENT_HEX) {
        fail_("HEX plus NMEA", "HEX line must be emitted first");
    }
    expect_event_("HEX plus NMEA", &capture, 1,
                  RS485_RX_STREAM_EVENT_NMEA, "$HEHDT,26.8,T*13");
}

static void test_reset_discards_partials_(void)
{
    rs485_rx_stream_t stream;
    capture_t capture = {0};
    uint8_t partial[8] = {0};
    uint8_t full[16];

    memset(full, 0x41, sizeof(full));
    rs485_rx_stream_init(&stream, true);
    rs485_rx_stream_feed(&stream, partial, sizeof(partial), capture_emit_, &capture);
    rs485_rx_stream_reset(&stream, true);
    rs485_rx_stream_feed(&stream, full, sizeof(full), capture_emit_, &capture);

    if (capture.count != 1u) fail_("reset", "wrong event count");
    expect_event_("reset", &capture, 0, RS485_RX_STREAM_EVENT_HEX,
                  "0000: 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41  |AAAAAAAAAAAAAAAA|");
}

static void test_overlong_line_matches_existing_behavior_(void)
{
    rs485_rx_stream_t stream;
    capture_t capture = {0};
    uint8_t data[RS485_RX_STREAM_NMEA_CAPACITY + 1u];

    memset(data, 'A', sizeof(data));
    data[sizeof(data) - 1u] = '\r';
    rs485_rx_stream_init(&stream, false);
    rs485_rx_stream_feed(&stream, data, sizeof(data), capture_emit_, &capture);

    if (capture.count != 1u) fail_("overlong NMEA", "wrong event count");
    if (capture.events[0].len != RS485_RX_STREAM_NMEA_CAPACITY - 1u) {
        fail_("overlong NMEA", "wrong truncated length");
    }
}

int main(void)
{
    test_fragmented_nmea_();
    test_delimiters_and_multiple_lines_();
    test_text_fields_keep_spaces_();
    test_hex_lines_and_offsets_();
    test_hex_keeps_nmea_decoder_();
    test_reset_discards_partials_();
    test_overlong_line_matches_existing_behavior_();

    puts("RS-485 RX stream tests passed");
    return EXIT_SUCCESS;
}
