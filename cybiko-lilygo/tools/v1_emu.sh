#!/bin/sh
# ReBiko Desktop. Builds the Classic (v1_emu) and the Xtreme (xt_emu) side
# by side, so Settings > Machine can switch, then runs the Classic.
# Optional Heltec radio.
#   tools/v1_emu.sh <boot.bin> <flash.bin> [--nvram save.flash] [--radio /dev/ttyUSB0] [--cyid HEX]
# Omit --radio to auto-pick the first Heltec CP2102 (10c4:ea60).
# It starts whichever model ran last; the other's ROMs go next to boot.bin.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
CORE=$HERE/lib/cybiko_core/src
RADIO=$HERE/../cybiko-c-emulator/src/radio
OUT=${OUT:-$HERE/.pio/v1_emu}
mkdir -p "$(dirname "$OUT")"
for M in XT V1; do
if [ $M = V1 ]; then BIN=$OUT; ST=${STRIDE:-12}; else BIN=$(dirname "$OUT")/xt_emu; ST=${XT_STRIDE:-20}; fi
gcc -std=c11 -D_DEFAULT_SOURCE -DEMU_TIMER_STRIDE=$ST -DCYBIKO_MACHINE_$M -O2 -Wall -Wextra \
    -I"$CORE" -I"$CORE/core" -I"$RADIO" -I"$HERE/tools" \
    $(pkg-config --cflags sdl2 libpng x11 xext) \
    -I"$HERE/cybiko-emu" \
    -o "$BIN" "$HERE/tools/v1_emu.c" "$HERE/tools/host_serial.c" "$HERE/cybiko-emu/rebiko_apps.c" \
    "$CORE/cybiko_core_unity.c" \
    "$RADIO/sci2_xt.c" "$RADIO/sci0_v1.c" "$RADIO/radio_coproc.c" \
    "$RADIO/radio_link.c" "$RADIO/radio_lora.c" "$RADIO/radio_serial_phy.c" \
    $(pkg-config --libs sdl2 libpng x11 xext) -lm
done
"$OUT" "$@"
