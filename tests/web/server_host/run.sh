#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/nmea-web-server-host.XXXXXX")
trap 'rm -f -- "$build_dir/test_web_server"; rmdir -- "$build_dir"' EXIT
trap 'exit 1' HUP INT TERM

"${CC:-gcc}" -std=gnu11 -Wall -Wextra -Werror -g -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections -fsanitize=address,undefined \
    -I"$test_dir/stubs" -I"$project_dir/main" \
    "$test_dir/test_web_server.c" -Wl,--gc-sections \
    -o "$build_dir/test_web_server"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}" "$build_dir/test_web_server"
