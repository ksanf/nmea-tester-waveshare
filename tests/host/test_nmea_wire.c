#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nmea_editor/nmea_wire.h"

static void fail_(const char *test, const char *detail)
{
    fprintf(stderr, "%s: %s\n", test, detail);
    exit(EXIT_FAILURE);
}

static void expect_frame_(const char *test, const char *input, const char *expected)
{
    char frame[128];
    const size_t wire_len = nmea_wire_frame_build(frame, sizeof(frame), input);

    if (wire_len != strlen(expected)) fail_(test, "wrong wire length");
    if (strcmp(frame, expected) != 0) {
        fprintf(stderr, "%s: expected '%s', actual '%s'\n", test, expected, frame);
        exit(EXIT_FAILURE);
    }
    if (frame[wire_len] != '\0') fail_(test, "frame is not NUL-terminated");
}

static void test_canonical_suffix_(void)
{
    expect_frame_("plain sentence", "$HEHDT,26.8,T*13", "$HEHDT,26.8,T*13\r\n");
    expect_frame_("existing CRLF", "$HEHDT,26.8,T*13\r\n", "$HEHDT,26.8,T*13\r\n");
    expect_frame_("repeated suffix", "$HEHDT,26.8,T*13\n\r\n", "$HEHDT,26.8,T*13\r\n");
}

static void test_capacity_boundaries_(void)
{
    char exact[4];
    char short_frame[3] = "xx";
    char unterminated[4] = {'A', 'B', 'C', 'D'};

    if (nmea_wire_frame_build(exact, sizeof(exact), "A") != 3u ||
        memcmp(exact, "A\r\n\0", sizeof(exact)) != 0) {
        fail_("exact capacity", "valid frame rejected");
    }
    if (nmea_wire_frame_build(short_frame, sizeof(short_frame), "A") != 0u ||
        short_frame[0] != '\0') {
        fail_("short capacity", "oversized frame accepted");
    }
    if (nmea_wire_frame_build(short_frame, sizeof(short_frame), unterminated) != 0u) {
        fail_("unterminated input", "unterminated frame accepted");
    }
}

static void test_empty_input_(void)
{
    char frame[8] = "dirty";

    if (nmea_wire_frame_build(frame, sizeof(frame), "\r\n") != 0u || frame[0] != '\0') {
        fail_("empty suffix", "empty sentence accepted");
    }
    if (nmea_wire_frame_build(frame, sizeof(frame), NULL) != 0u || frame[0] != '\0') {
        fail_("NULL input", "NULL sentence accepted");
    }
}

static void test_sentence_validation_(void)
{
    if (!nmea_wire_sentence_valid("$HEHDT,26.8,T*13", true)) {
        fail_("checksum", "valid checksum rejected");
    }
    if (nmea_wire_sentence_valid("$HEHDT,26.8,T*12", false)) {
        fail_("checksum", "invalid optional checksum accepted");
    }
    if (!nmea_wire_sentence_valid("$HEHDT,26.8,T", false) ||
        nmea_wire_sentence_valid("$HEHDT,26.8,T", true)) {
        fail_("checksum", "optional/required checksum policy broken");
    }
    if (nmea_wire_sentence_valid("HEHDT,26.8,T*37", false) ||
        nmea_wire_sentence_valid("$HEHDT,26.8,T*13junk", false)) {
        fail_("framing", "malformed sentence accepted");
    }
}

int main(void)
{
    test_canonical_suffix_();
    test_capacity_boundaries_();
    test_empty_input_();
    test_sentence_validation_();

    puts("NMEA wire-frame tests passed");
    return EXIT_SUCCESS;
}
