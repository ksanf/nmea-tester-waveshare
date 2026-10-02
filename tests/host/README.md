# Host tests

These tests compile production C sources with isolated platform substitutes.
They run on Linux or WSL without a board, CAN adapter, Wi-Fi interface, or device
capture files.

## Requirements and execution

Use `make`, a C11 compiler with GNU C support, pthreads, and the math library.
Python 3 is used only to discover an ESP-IDF path from an existing build when
`IDF_PATH` was not provided.

The template suite builds the same cJSON source as the firmware. Activate the
ESP-IDF environment and run:

```sh
make -C tests/host test
```

For a fresh checkout, provide the ESP-IDF directory explicitly:

```sh
make -C tests/host test IDF_PATH=/path/to/esp-idf-v5.5
```

Alternatively, set `CJSON_DIR=/path/to/cJSON` to a directory containing `cJSON.c`
and `cJSON.h`. With neither option, the Makefile attempts to read `idf_path`
from `build/project_description.json`.

Executables are placed in `tests/host/build`. Override `BUILD_DIR` to use a
separate build location, for example:

```sh
make -C tests/host test IDF_PATH=/path/to/esp-idf-v5.5 BUILD_DIR=/tmp/nmea-host-tests
```

## Coverage

| Area | Checks |
|---|---|
| NMEA output | Sentence bodies, XOR checksums, version profiles, wire-length limits, CR/LF, and ROT heading motion |
| RS-485 input | HEX/ASCII formatting, fragmented streams, and byte activity |
| Navigation and AIS | Sentence decoding, independent runtime state, multipart AIS, and concurrent snapshots |
| Presentation helpers | Framebuffer rotation, safe log markup, and numeric input validation |
| Web ownership | Claims, session generations, reconnect deadlines, revocation, and queued commands |
| Templates | Metadata, types/ranges, coordinate/calendar boundaries, atomic rejection, and concurrent edits/saves |
| TT6006 services | Fast Packet pages, NDP connections, retries, dynamic ports, NAME/SA changes, telemetry CRCs, freshness, status labels, and integration |
| CAN task boundary | Extended data frames, RTR/invalid frames, partial TWAI field writes, task creation failure, blocked stop, and safe restarts |
| Device identity | Stable MAC-derived NMEA 2000 NAME/serial and failure to read the MAC |
| Telnet | Fragmentation, immediate CR delivery, IAC negotiation, and bounded in-place output, including CR followed by 128 bytes |
| Wi-Fi persistence | Legacy settings, grouped saves, open/set/commit failures, and RAM/radio behavior |
| RTC fallback | RTC and CPU-only builds, UTC/calendar validation, read failure/recovery, and concurrent time edits |

The template suite uses the production template defaults, snapshot/patch code,
and cJSON. Its stubs supply FreeRTOS locks/timers, NVS, the clock boundary, and
TX notifications. The dedicated clock suites compile the real clock module
against a simulated RTC and CPU clock, without changing the host computer time.

The CAN test double reproduces the ESP-IDF 5.5 message union and partial writes
by `twai_receive_v2()`. It deliberately fills unsupported flag bits with nonzero
values, so relying on a flag that the receive API does not populate is detected.

The Wi-Fi suite compiles the real manager and injects failures at each storage
stage. Its NVS model can persist a write before returning a commit failure;
tests do not assume that failed commit means storage was rolled back.

## Other suites and limits

Browser tests live in `tests/web/test_client.cjs`; setup is documented in the
[main README](../../README.md#tests). The production HTTP output dispatcher has
an additional [sanitizer regression](../web/server_host/README.md).

The portable tests exercise production logic, but their dependency substitutes
do not model electrical CAN behavior, real radio scheduling, power interruption,
or every FreeRTOS/ESP-IDF interleaving. They complement hardware validation;
they are not evidence of certification or industrial reliability.
