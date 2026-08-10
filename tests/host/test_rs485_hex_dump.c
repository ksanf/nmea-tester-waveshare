#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rs485/rs485_hex_dump.h"

static void expect_line_(const char *name, const char *expected,
                         uint32_t offset, const uint8_t *data, size_t len)
{
    char actual[RS485_HEX_DUMP_LINE_CAPACITY];
    size_t written = rs485_hex_dump_format(actual, sizeof(actual), offset, data, len);

    if (written != strlen(expected) || strcmp(actual, expected) != 0) {
        fprintf(stderr, "%s\n  expected: '%s'\n  actual:   '%s'\n",
                name, expected, actual);
        exit(EXIT_FAILURE);
    }
}

int main(void)
{
    static const uint8_t nmea[] = "$HEHDT,26.8,T*13";
    static const uint8_t controls[] = {0x1F, 0x20, 0x7E, 0x7F, 0x80};

    expect_line_(
        "full NMEA line",
        "0000: 24 48 45 48 44 54 2C 32 36 2E 38 2C 54 2A 31 33  |$HEHDT,26.8,T*13|",
        0, nmea, sizeof(nmea) - 1);
    expect_line_(
        "printable boundaries",
        "0010: 1F 20 7E 7F 80                                   |. ~..           |",
        0x10, controls, sizeof(controls));

    puts("RS-485 hex dump tests passed");
    return EXIT_SUCCESS;
}
