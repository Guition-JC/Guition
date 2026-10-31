#!/usr/bin/env bash
set -euo pipefail

source_file="$(dirname "$0")/../src/rx8025_registers.c"

if ! grep -q 'RX8025_STOP_TO_START_DELAY_US' "$source_file"; then
    echo "missing RX8025 STOP-to-START delay guard" >&2
    exit 1
fi
if ! grep -q 'esp_rom_delay_us(RX8025_STOP_TO_START_DELAY_US)' "$source_file"; then
    echo "missing RX8025 post-transaction delay" >&2
    exit 1
fi
