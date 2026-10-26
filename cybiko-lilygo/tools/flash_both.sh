#!/bin/sh
# Build ReBiko Pocket for both models and flash them into the T-Deck's two
# app slots: the Classic in app0 (it boots first) and the Xtreme in app1.
# The menu's System > Machine switches between them.
#   tools/flash_both.sh [port]      (default /dev/ttyACM0)
# A plain `pio run -t upload` afterwards replaces app0 only and boots it.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
PORT=${1:-/dev/ttyACM0}
PIO=${PIO:-pio}
cd "$HERE"
$PIO run -e T-Deck-Cybiko-V1
$PIO run -e T-Deck-Cybiko
PY=${PY:-$HOME/.platformio/penv/bin/python}
ESPTOOL=${ESPTOOL:-$HOME/.platformio/packages/tool-esptoolpy/esptool.py}
BOOT_APP0=${BOOT_APP0:-$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin}
V1=.pio/build/T-Deck-Cybiko-V1
XT=.pio/build/T-Deck-Cybiko
# The menu's switch looks for these tags in the other slot
for pair in "$V1:Classic" "$XT:Xtreme"; do
    if ! grep -aq "REBIKO-MACHINE:${pair#*:}!" "${pair%%:*}/firmware.bin"; then
        echo "${pair%%:*}/firmware.bin has no ${pair#*:} tag; not flashing" >&2
        exit 1
    fi
done
"$PY" "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
    --before default_reset --after hard_reset write_flash -z \
    --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0000 "$V1/bootloader.bin" 0x8000 "$V1/partitions.bin" 0xe000 "$BOOT_APP0" \
    0x10000 "$V1/firmware.bin" 0x650000 "$XT/firmware.bin"
