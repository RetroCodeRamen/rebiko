# ReBiko Pocket

![CyOS running on a LilyGo T-Deck](tdeck-small.jpg)

ReBiko Pocket is the T-Deck firmware of [ReBiko](../README.md): an original LilyGo T-Deck (ESP32-S3, ST7789, keyboard at I2C 0x55) that runs the original CyOS and is a Cybiko. The T-Deck's SX1262 carries the Cybiko radio over LoRa, so it chats with other ReBiko Pockets and with ReBiko Desktop on a PC with a Heltec.

Two builds:

- **`T-Deck-Cybiko-V1`: the Cybiko Classic**, CyOS 1.2.46. This is the main one. Chat works over the air with ReBiko Desktop.
- **`T-Deck-Cybiko`: the Cybiko Xtreme**, CyOS 1.5.08. This is the PlatformIO default.

The T-Deck Pro is e-paper with a different pinout and is not a target. The pins in `tdeck_pins.h` are the original T-Deck's.

The Cybiko was a wireless handheld from 2000–2001, built around a Hitachi H8S/2323 CPU with a 160x100 grayscale LCD. This firmware emulates that hardware in C and runs CyOS on the ESP32-S3, using the T-Deck's keyboard, display, trackball and speaker.

## Hardware

- **LilyGo T-Deck** (original, ESP32-S3, 16MB Flash, 8MB PSRAM)
- ST7789 320x240 TFT display (Cybiko's 160x100 output is 2x upscaled to 320x200)
- Built-in QWERTY keyboard and trackball
- I2S speaker for audio
- MicroSD card slot for ROM files

## Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- A LilyGo T-Deck (original, not Pro)
- A MicroSD card
- CyOS ROM files for the model you build (below)

## Setup

### 1. Prepare the SD card

ROMs are not in git. The ReBiko Desktop release zip carries the Classic ones in `roms/`; see [GETTING-ROMS.txt](../GETTING-ROMS.txt).

```
SD card root/
  cybiko-v1/          Classic
    boot.bin          cyrom112.bin
    flash.bin         flash_v1246.bin (CyOS 1.2.46)
  cybiko/             Xtreme
    boot.bin
    flash.bin
```

The Xtreme needs the CyOS 1.5.08 dumps:

| SD card name | Original filename | Size | SHA-256 |
|---|---|---|---|
| `boot.bin` | `cyrom150.bin` | 32 KB | `b22f1f30cbf4000d060bacd676ef728e2d3bd7d3eaccbae53fb37cee72604ec1` |
| `flash.bin` | `cyos_v1508.bin` | 512 KB | `2d68c951749102eb310daf3d652e6818ca7e121b9ebd93c5340fe813e8fa11d3` |

Insert the SD card into the T-Deck.

### 2. Build and flash

```bash
# Both models: the Classic in app slot 0 (boots first), the Xtreme in slot 1
tools/flash_both.sh

# Or one model only, into slot 0
pio run -t upload -e T-Deck-Cybiko-V1   # Classic
pio run -t upload                       # Xtreme (the default env)

# Serial log: fps, radio counters, saves
pio run -t monitor

# The Cybiko screen as a picture, over USB
tools/tdeck_shot.py screen.png
```

With both installed, menu > System > Machine switches between them: settings and CyOS's data are saved, then the T-Deck restarts as the other model. Each build carries a tag naming its model, and the switch checks the other slot for it first. `flash_both.sh` refuses an image without its tag.

PlatformIO fetches TFT_eSPI and the other dependencies on the first build. Every ReBiko Pocket and ReBiko Radio has to run the same radio profile; when it changes, flash them all.

## Usage

The T-Deck boots straight into CyOS. On first boot, CyOS asks for a nickname.

**Keyboard:** The keyboard runs in raw mode, so ReBiko sees every key go down and up, Mic and both Shift keys included. Letters type as themselves. Hold **Sym** for the T-Deck's digits and symbols, as printed on the keys. Keys that type are taps, held at least 12 frames, then up for 3 before the next (10 if it is the same key again): CyOS 1.2.46 misses shorter presses. A key you keep holding stays down on the Cybiko until you let go, so games see it held.

**Left Shift** is the Cybiko's Shift key and **Right Shift** its Fn key. Each is held on the Cybiko for as long as you hold it, and CyOS does its own Shift and Fn handling.

**`$` combinations:** The T-Deck has no Esc, function keys or several Cybiko symbols, so `$` starts a combination: tap `$`, then the key. The top line shows a `$` while one is waiting. `$` `$` types a `$`.

| Keys | Does |
|---|---|
| `$` `x` | Esc |
| `$` `q` … `u` | F1 … F7 |
| `$` `a` / `s` | Select / Insert |
| `$` Backspace | Del (Backspace alone is the Cybiko's BkSp) |
| `$` `f` | Fn (a tap of the Cybiko's Fn key) |
| `$` `g` / `h` | Help / Tab |
| `$` `j` `k` `l` `z` `c` `v` `b` `n` | `[` `]` `\` `` ` `` `=` `'` `;` `/` |
| `$` `p` / `m` | Volume up / down |
| `$` Space | Key reference: every combination, as set now |
| `$` Mic | The ReBiko menu |

Every key except `$`, Sym and `$` Mic can be remapped in the menu (below), so this is the default table.

Backspace erases before the cursor (the Cybiko's BkSp). Before raw mode it sent the Cybiko's Del, which deletes forward and did nothing at the end of a line.

Volume has 11 levels, 0 to 10 (100%). After `$p` or `$m`, more `p` and `m` presses keep stepping it for 2 seconds after each press: `$ m m m` is three levels down. Any other key ends volume mode and is typed normally.

**Trackball:** The Cybiko's d-pad. A click is Enter. Holding the click for 600 ms is Esc, held until you let go.

**Menu (`$` Mic):** The emulator pauses while the menu is open, and nothing you press reaches CyOS. Closing it carries on where CyOS was. Move with the ball, click (or Enter) to choose, hold the ball (or Backspace) to go back; on a value, left and right change it. `$` Mic closes it from anywhere.

| Section | Settings |
|---|---|
| Controls & Key Mapping | Every Cybiko key and command the T-Deck reaches by a combination or special key: see its inputs, change it, add another input, clear your own, or restore its default. Restore all key mappings. |
| Performance & CPU | CPU mode: Dynamic (240 MHz, 160 MHz while CyOS sleeps), Always high (240 MHz), or Always low (160 MHz) |
| Display | Brightness 4/4 to 1/4 · Scale 2x or 1x (the Cybiko's own 160×100) · Screen off when CyOS sleeps |
| Audio | Sound on/off · Volume 0–10 (the same level as `$p` / `$m`) |
| System | Version · Machine (Classic or Xtreme) · Hardware info · Key reference · Radio link view · Restore all defaults · Restart ReBiko · Return to emulation |

To change a mapping, choose the key, then Change mapping, and press the new key, `$` combination, or ball move or click. If that input already does something, the menu says what and asks before replacing it. Holding the ball cancels. `$` Mic can't be taken, so the menu is always reachable.

Always low runs at 160 MHz all the time to save battery. The idle desktop and Chat mostly keep up, but busy screens and games fall behind real time now and then: they slow down and the sound breaks up while they do. Dynamic is the default.

**Settings** are kept in the ESP32's NVS as one blob with a version and a CRC (`cybiko-emu/rebiko_config.h`), written when the menu closes, when volume mode ends, and only if something changed. Damaged settings are replaced by defaults; out-of-range values are reset one by one. Restore all defaults resets keys, CPU, display and audio. It leaves CyOS's files and the SD card alone.

**Screen sleep:** After 5 idle minutes CyOS turns its LCD off. The T-Deck then turns off the backlight and stops drawing, and in the Dynamic CPU mode drops the CPU from 240 to 160 MHz. Any key wakes CyOS, and the screen comes back with it (at 240 MHz, unless the mode is Always low). The radio keeps running while it sleeps. The awake desktop stays at 240: at 160 its redraw bursts use nearly every frame's time, and the sound would break up.

**Saves:** The Classic writes CyOS's flash chip to `/cybiko-v1/save.flash` 10 s after CyOS's last write to it. The chip is emulated as the 1 MB AT45DB081, twice the real Classic's 512 KB: CyOS detects it ("flash device model 4") and gets about 560 KB free instead of 43 KB. An old 512 KB save or `flash.bin` loads into the first half and the rest becomes empty blocks; the save is 1 MB from then on. That covers your nickname, settings and files. The Xtreme saves RAM to `/cybiko/save.nvram` every 5 minutes, when it has changed. Both are written to `save.tmp` first and then renamed. SD SPI runs at 20 MHz.

**Radio:** The SX1262 sits under the emulated radio co-processor. It runs at 915 MHz, SF7, 500 kHz, 22 dBm, with listen-before-talk. CyOS's own radio code runs unchanged. Chat rooms are radio channels, and the co-processor hears only the channel CyOS tuned. CyOS transmits nothing until wireless is on in its settings. On the PC side, ReBiko Desktop (`tools/v1_emu.c`) uses a Heltec running ReBiko Radio (`../cybiko-heltec-modem/`).

**Range test:** Radio link view (menu > System) swaps the bottom hint line for a link readout, and the T-Deck sends a ping every second:

    rx 120 lost 2 1.6%  -96dBm snr 4  1s

The fields are messages heard, messages missed, last RSSI, last SNR, and seconds since the last one. Misses are counted from gaps in each sender's message numbers. The line is green while everything is arriving, yellow once something was missed, and red after 5 s of silence.

On the PC, run `../cybiko-c-emulator/build/modem_tool /dev/ttyUSB0 --seconds 600 --ping 1000` (ReBiko Desktop must be closed). It pings back and prints the same counts for the other direction every 5 s. Pings use channel 0, which CyOS never tunes, so CyOS never sees them. At SF7 the receiver stops decoding at about −7.5 dB SNR, so an SNR that gets near that is the edge of range.

**Apps (Classic):** Apps go onto a running Cybiko and come off it over USB, with nothing restarted. ReBiko hands each app to CyOS's own serial console, the way CyberLoad loaded a real Cybiko, and reads files straight off the flash to copy them back. CyOS 1.2.46 lists a new app on its desktop only after it restarts, so send any number of apps, then restart CyOS once.

    tools/rebiko_apps.py /dev/ttyACM0 ls                  # files and free room
    tools/rebiko_apps.py /dev/ttyACM0 put game.app ...    # about 2 KB a second
    tools/rebiko_apps.py /dev/ttyACM0 reboot              # CyOS restarts; new apps show
    tools/rebiko_apps.py /dev/ttyACM0 get notes.dat [out]
    tools/rebiko_apps.py /dev/ttyACM0 rm game.app

The tool needs pyserial. Without a port it talks to ReBiko Desktop instead (`127.0.0.1:47960`). ReBiko Desktop also has an **Apps** window (right-click menu, or Settings > Apps). It lists the `Apps/` library next to `roms/` by folder, filtered as you type, beside the Cybiko's files and free room. Its buttons are Send, Restart CyOS, Delete and Copy to PC (into `Apps/From Cybiko/`). The commands, and the console protocol read from CyOS's code, are in `cybiko-emu/rebiko_apps.h`. The Xtreme is not covered yet: it loaded apps over its USB chip with a different program.

## Project Structure

```
cybiko-lilygo/
  cybiko-emu/
    main.cpp          # T-Deck firmware (display, keyboard, audio, SD, radio pump)
    radio_sx1262.*    # SX1262 transport wrapper
    rebiko_input.*    # Physical keys, $ combinations, bindings, input router
    rebiko_config.*   # Settings struct and its versioned NVS blob
    rebiko_menu.*     # The $ Mic menu: screens, cursor, key remapping
    tdeck_keymap.*    # Cybiko keyboard matrix positions (Classic and Xtreme)
    tdeck_pins.h      # Hardware pin definitions (original T-Deck)
  tests/
    run_rebiko.sh     # Host tests of the rebiko_* modules (gcc, sanitizers)
  tools/
    keytest/          # Keyboard and trackball test (env keytest-raw: raw bits)
    v1_emu.c / .sh    # ReBiko Desktop, the PC Classic (SDL)
    v1_chat.sh        # Two Classics on the host walk into Chat and swap lines
    v1_boot.sh        # V1 host STATUS line vs Java
    host_bench.sh     # Host build + RAM/LCD hash check of the vendored core
    capture_serial.py # Timestamped serial log for a fixed time
  lib/
    cybiko_core/      # Emulator core (vendored from cybiko-c-emulator)
  boards/
    T-Deck.json       # Custom board definition
  platformio.ini      # T-Deck-Cybiko (XT) and T-Deck-Cybiko-V1
```

The emulator core (`lib/cybiko_core/`) is platform-independent C and communicates with the T-Deck firmware through a HAL (Hardware Abstraction Layer) with callbacks for display rendering, audio output, keyboard polling, and serial output. The same core also builds as a standalone Linux/SDL2 emulator from the [cybiko-c-emulator](https://github.com/daberkow/cybiko-c-emulator) repo.

## Performance

These numbers are for the Xtreme build. The Classic holds 60 fps from power-on at stride 12. They were measured on an original T-Deck (ESP32-S3, 240 MHz) on 22 Oct 2026, booting CyOS 1.5.08 from the card. The serial log prints one line every 5 s:

```
60.0 fps (100%)  922 kIPS  232 cyc/insn  sleep 0%  core 4461 ms  render 22 ms  idle 526 ms
```

`kIPS` is guest instructions per second. `cyc/insn` is ESP32 cycles per guest instruction, outside the display callback. `sleep` is the share of guest cycles spent in the SLEEP fast-forward. `idle` is time the frame limiter waited, which is the headroom.

| Phase | Before | After |
|---|---|---|
| CyOS loader, LZSS unpack | 61 fps | 60 fps (capped) |
| "Loading CyOS" (a checksum loop in guest RAM) | 24 fps, 370 kIPS, ~630 cyc/insn | 60 fps (capped), 922 kIPS, ~232 cyc/insn, ~10% idle |
| Power-on to home screen | several minutes | ~70 s |
| CyOS idle at the home screen | — | 60 fps, ~20% of one core |
| Display callback | ~360 ms per 5 s | ~3 ms per 5 s |

### What changed

1. **Timer16 skips ticks that cannot match** (`timer16_advance`). gprof on the host showed ~60% of the time in `timer16_event`: about 5 calls per guest instruction. CyOS leaves channel 3 at φ/4, clear on TGRA, TGRA=1, with TGFA already set, so every tick is a compare match with no effect. The batch now jumps to the next tick that can hit TGRA, TGRB, or overflow. If a whole clear-on-TGRA period sets no new TSR flag and moves no output pin, the remaining whole periods are dropped. Interrupts, flags, and speaker edges are unchanged.
2. **60 Hz frame limiter** (`pace_frame` in `main.cpp`). Once idle CyOS reached ~260 fps, the RTC, key hold, and audio ran 4x fast. Each frame is a fixed slice of guest time, so the loop now waits for the 60 Hz deadline. A frame that runs late is not made up in a burst.
3. **Unchanged frames are not drawn.** The 160×100 frame is compared with the last one sent; CyOS rarely redraws.
4. **The 128 KB RGB565 buffer is in internal DMA SRAM** when a block is free (it is: ~270 KB largest block at boot), so the upscale does not stream through the 32 KB PSRAM data cache. It falls back to PSRAM.
5. **SCI2 skips ticks that only count down** (`cybiko_sci2_advance`, shared with the host core). With a co-processor attached, the old loop ran one full SCI2 tick per guest cycle, 307200 a frame. On the host bench that made the core 6x slower (0.71 s → 4.5 s for 3600 frames). It is 0.77 s now. On the T-Deck the co-processor is attached after the SX1262 starts (`CYBIKO_RADIO_TX=1` in `platformio.ini`) and the 60 fps cap still holds.

Earlier upstream work stays: `IRAM_ATTR` and `-O3` on the interpreter, eight CPU steps per timer batch, boot ROM and on-chip RAM in internal SRAM.

### How the results were checked

- `tools/host_bench.sh <boot.bin> <flash.bin> [frames]` builds this tree's vendored core on the host at stride 20 and prints a hash of guest RAM and the LCD every 250 frames. Before and after each core change, 6000 frames gave identical hashes, with and without `BENCH_RADIO=1`.
- `cybiko-c-emulator`: `make test` and `make harness` pass. `two_boot --frames 7000` exits 0 and puts `pingdeck` in B's RAM, with output identical to the pre-change build apart from timing.
- The c-emulator `test_timer16`, `test_timer8`, `test_h8s_cpu`, `test_hd66421`, `test_memory`, and `test_keyboard` pass against this tree's core. `test_speaker` does not build against it; the two speaker structs have diverged.
- `tools/capture_serial.py <seconds> <log>` records the serial lines after `pio run -t upload`.
- Build with `PLATFORMIO_BUILD_FLAGS=-DCYBIKO_PROFILE` to add a guest-PC histogram to each report. The radio co-processor is always attached when the SX1262 starts; `CYBIKO_RADIO_TX=1` (set in `platformio.ini`) lets it transmit.

### What is still slow

- `EMU_TIMER_STRIDE=20` is a timing approximation: each guest instruction advances the timers 20 cycles, so a frame carries ~15360 instructions. The guest gets ~0.9 MIPS, well short of a real XT. CPU-heavy apps can still run slower than on hardware. Halving the stride needs about 2x interpreter speed for the same 60 fps.
- ~230 ESP32 cycles per guest instruction, against ~60 for the same core on a desktop. The guest's RAM and flash image are in PSRAM behind a 32 KB data cache with 32-byte lines (the Arduino SDK default). A 64 KB data cache needs a custom ESP-IDF sdkconfig.
- Further interpreter work, in rough order of payoff: lazy flags, a fetch window instead of four range compares per fetch, and keeping `pc`/`ccr` in locals in `h8s_cpu_run`.

## Acknowledgments

The emulator core was developed with reference to several existing Cybiko emulation projects:

- [MAME](https://github.com/mamedev/mame) — Cybiko XT machine driver and H8S CPU core (BSD-3-Clause)

## License

MIT — see [LICENSE](LICENSE) for details.

