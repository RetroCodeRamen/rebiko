#!/bin/sh
# Host tests for the ReBiko input router, settings, menu and app loading
# (cybiko-emu/rebiko_*.c).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$HERE/.pio/test_rebiko}
mkdir -p "$(dirname "$OUT")"
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$HERE/cybiko-emu" -o "$OUT" "$HERE/tests/test_rebiko.c" \
    "$HERE/cybiko-emu/rebiko_input.c" "$HERE/cybiko-emu/rebiko_config.c" \
    "$HERE/cybiko-emu/rebiko_menu.c"
"$OUT"
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$HERE/cybiko-emu" -o "$OUT.apps" "$HERE/tests/test_apps.c" \
    "$HERE/cybiko-emu/rebiko_apps.c"
"$OUT.apps"
