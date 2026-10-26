#!/bin/sh
# Every native boot ROM loop (h8s_cpu.c: hle_memset, hle_fill, hle_blit)
# checked against the CPU running the same passes: all registers, PC,
# flags and RAM must match.
#   tests/hle_verify.sh <cyrom112.bin> <flash or save image> [frames]
#   MACHINE=XT tests/hle_verify.sh <cyrom150.bin> <cyos_v1508.bin> [frames]
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
CORE=$HERE/lib/cybiko_core/src
RADIO=$HERE/../cybiko-c-emulator/src/radio
OUT=${OUT:-$HERE/.pio/hle_verify}
mkdir -p "$(dirname "$OUT")"
if [ "${MACHINE:-V1}" = XT ]; then FLAGS="-DEMU_TIMER_STRIDE=20 -DCYBIKO_MACHINE_XT"
else FLAGS="-DEMU_TIMER_STRIDE=12 -DCYBIKO_MACHINE_V1"; fi
gcc -std=c11 -D_DEFAULT_SOURCE $FLAGS -DCYBIKO_HLE_VERIFY -O2 -w \
    -I"$CORE" -I"$CORE/core" -I"$RADIO" -o "$OUT" "$HERE/tests/hle_verify.c" \
    "$CORE/cybiko_core_unity.c" "$RADIO/sci2_xt.c" "$RADIO/sci0_v1.c" \
    "$RADIO/radio_coproc.c" "$RADIO/radio_link.c"
"$OUT" "$1" "$2" "${3:-7200}" 2>&1 | grep -v '^Bus:'
