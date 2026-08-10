#!/usr/bin/env bash
# Source this helper to enter ESP-IDF and switch to the project directory:
#   source ./run-esp.sh

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

if [[ -z "${IDF_PATH:-}" ]]; then
    for candidate in "${ESP_IDF_PATH:-}" "$HOME/esp/esp-idf" "$HOME/esp-idf-5.5"; do
        if [[ -n "$candidate" && -f "$candidate/export.sh" ]]; then
            IDF_PATH="$candidate"
            break
        fi
    done
fi

if [[ -z "${IDF_PATH:-}" || ! -f "$IDF_PATH/export.sh" ]]; then
    echo "ESP-IDF was not found. Set IDF_PATH or ESP_IDF_PATH before sourcing this file."
    return 1 2>/dev/null || exit 1
fi

source "$IDF_PATH/export.sh" || { return 1 2>/dev/null || exit 1; }
export ESPPORT="${ESPPORT:-/dev/ttyACM0}"
cd "$PROJECT_DIR" || { return 1 2>/dev/null || exit 1; }

printf 'ESP-IDF ready | port: %s | project: %s\n' "$ESPPORT" "$PROJECT_DIR"
printf '  idf.py build\n  idf.py flash\n  idf.py monitor\n'
