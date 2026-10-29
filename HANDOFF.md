# Handoff: Claude → Cursor (27 Oct 2026)

Read this, then [PLAN.md](PLAN.md) (the work order) and [ARCHITECTURE.md](ARCHITECTURE.md) (how it works). Delete this file once you have taken it in, or fold what is still useful into PLAN.md.

## Where things stand

- **Release [v1.1027.1b](https://github.com/RetroCodeRamen/rebiko/releases/tag/v1.1027.1b)** is out: a Linux `.deb` and a Linux zip. The Windows build was **not** rebuilt (no MinGW on this machine); v1.1026.4b for Windows uses the old radio link and cannot talk to this release.
- Everything is committed and pushed to `main` (last code commit `24526a0`, tag `v1.1027.1b`).
- The user's T-Deck runs current firmware in both slots (Classic app0, Xtreme app1). Their T-Deck save and the Desktop save (`roms/pc-classic.flash`) run **CyOS 1.2.57** with full Cylandia. `roms/pc-classic.flash.cyos1246-backup` is the pre-update save.

## What changed in the last two days (oldest first)

| Commit | What |
|---|---|
| `f0a5368` | Desktop Xtreme switch; live app loading on the Classic (app link, Apps window, `tools/rebiko_apps.py`) |
| `17933fe` | 1 MB flash chip (AT45DB081); CyOS 1.2.57 key IRQ fix |
| `961b88e` | Classic radio under CyOS 1.2.57 (DTC path in `sci0_v1.c`) |
| `f4c5634` | Radio co-processor follows the real AVR firmware: 8-byte address header kept on the air, **link version 2** (incompatible with older builds); Classic replies as `avr19c6.bin` |
| `a0251bd` | T-Deck: panel DMA finished before chip select is released (stale screen areas) |
| `5f10b41` | App link: uploads no longer lost (stale reply buffer); `tools/rebiko_cyos_update.py` |
| `34d06e7` | Radio tidy (v2 beacon removed, poll/scan → small/large frames), `tests/test_sci0.c`, docs |
| `4bb4459` | T-Deck: page-by-page saves (no 1.9 s freeze), faster drawing, rolling screen refresh, Sym ordering fix, Classic shifted symbols |
| `a227a46` | **CPU fix: `MULXU.B`/`MULXS.B` multiplied by Rd's high byte** (broke all BCD in CyOS); clock keeps time (CyOS boot reset undone, T-Deck NVS clock, Desktop `TIME` broadcast on LoRa channel 0); real month lengths |
| `24526a0` | `tools/make_deb.sh` (.deb + zip), version from `VERSION` |

## How to work on it (what the user expects)

- **Commit straight to `main`**, no branches or PRs. Commit/push only when the user asks or approves.
- **Flash and test T-Deck changes on the device before committing.** Flashing needs no permission; it is a dev board. `cybiko-lilygo/tools/flash_both.sh [port]` builds and flashes both slots. The T-Deck is `/dev/ttyACM0`, sometimes `/dev/ttyACM1` after a restart; the Heltec is `/dev/ttyUSB0`.
- The user cares about small visual details (the boot logo, ripples, the time) and about accuracy to the real Cybiko.
- Host checks after core or radio changes: `cybiko-c-emulator: make test`; `cybiko-lilygo: sh tests/run_rebiko.sh`, `tools/v1_three.sh <cyrom112> <flash>` (and `CHAIN=1`), `tools/v1_chat.sh`, `tests/hle_verify.sh` (Classic and `MACHINE=XT`), `tools/host_bench.sh` XT hashes before/after.
- T-Deck over USB: `tools/tdeck_shot.py` (screenshot of what the T-Deck *believes* it shows: the panel cannot be read back, its MISO is not wired), `tools/rebiko_apps.py /dev/ttyACMx ls|put|get|rm|cmd ver`, the 5 s stats line on serial (fps, render ms, idle ms).
- Desktop headless tests: `SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy CYBIKO_SHOT=prefix CYBIKO_SHOT_EVERY=n CYBIKO_QUIT_AT=n CYBIKO_KEYS=frame:key,...`.

## Gotchas learned the hard way

- A headless CyOS 1.2.57 stops booting after "OS version" unless its serial console is drained (`cybiko_attach_serial` with a sink).
- CyOS 1.2.57 Chat opens the recipient list on entering a room: press Enter once more before typing.
- `pgrep -f`/`pkill -f` match their own shell; check `/proc/PID/cmdline` instead.
- Opening and closing the T-Deck's USB port repeatedly while it prints its boot log can leave it not reading USB input (it still prints). A reset clears it. One long-lived connection never hit it. Not fixed yet.
- Sending only the changed *columns* to the panel drew the boot logo wrong (cause unknown, logic verified on the host); updates stay whole rows.

## Open, in the order I would take them

1. **Private messages and the people list, Desktop ↔ T-Deck over LoRa.** First real test of addressed frames now that the destination is kept. Then multiplayer games.
2. **Classic ↔ Xtreme chat** (PLAN "In progress"). Recheck after the header change.
3. **Windows release** of v1.1027.1b (needs a Windows machine or MinGW; `tools/v1_emu.bat`).
4. **T-Deck USB input stall** (above): likely in the T-Deck's USB-serial handling when its output backs up.
5. **Cylandia runs at 54–58 fps** on the T-Deck: CyOS alone uses 4.5–4.85 s of every 5 s there. Needs core speed (profile Cylandia, run hot loops natively as `hle_v1_*` does).
6. **Group encryption** (the user decided the design on 28 Oct: see PLAN.md Open item 4). Open by default; with a group passphrase, the whole link packet is encrypted except a key id and nonce.
7. **LLM buddy (Tyler) on the Desktop** is built and tested over LoRa with the T-Deck (PLAN "In progress"). Untested on Windows. The T-Deck's own typing loses Shift the same way the bot's did ("JApan"); the bot's measured timings (12 frames up between keys, Shift 10 frames either side) are the starting point for `rebiko_input.c`.
8. Smaller: Classic `~` has no mapping (CyOS types Shift+` as `); WiFi time for T-Decks away from the Desktop; radio settings in the Pocket menu; range at SF8/SF9.

## Where things live

- T-Deck firmware: `cybiko-lilygo/cybiko-emu/main.cpp` (display, keyboard, saves, clock, USB link), core `cybiko-lilygo/lib/cybiko_core/src/core/` (vendored; the PC-side XT core in `cybiko-c-emulator/src/core/` is a separate copy: CPU and RTC fixes go in both).
- Radio: `cybiko-c-emulator/src/radio/` (`radio_coproc.c`, `sci0_v1.c`, `sci2_xt.c`, `radio_link.c`, `radio_lora.c`), shared by T-Deck, Desktop and Heltec.
- Desktop: `cybiko-lilygo/tools/v1_emu.c` (builds `v1_emu` and `xt_emu`); packaging `tools/make_deb.sh`.
- The Classic's AVR firmware analysis and the "Cybiko never relayed" result are summarised in ARCHITECTURE.md (radio section) and PLAN.md (Radio).
