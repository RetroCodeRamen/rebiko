# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Cybiko handheld emulator on the original LilyGo T-Deck (ESP32-S3, 16MB Flash, 8MB PSRAM, ST7789). Not the T-Deck Pro. Default env is Xtreme; `T-Deck-Cybiko-V1` is Classic. The project has two parts:

- **This repo (cybiko-lilygo):** T-Deck Arduino/PlatformIO firmware — display, keyboard, audio, SD card HAL, SX1262 pump
- **Emulator core** (vendored at `lib/cybiko_core/`): Platform-independent C emulator core — H8S CPU, HD66421 LCD, timers, speaker, CFS/AT45, keyboard matrix. Copied from [cybiko-c-emulator](https://github.com/daberkow/cybiko-c-emulator) at commit `2034984e71fd73e8db32943d0162b52ff8b2af9e`. Radio sources are compiled from `../cybiko-c-emulator/src/radio/`, not copied. Workspace docs: `../ARCHITECTURE.md`, `../PLAN.md`, `../PLAN-V1.md`.

## Build Commands

```bash
# Build (default env: T-Deck-Cybiko)
pio run

# Build + upload to device
pio run -t upload

# Serial monitor (115200 baud)
pio run -t monitor

# Build + upload + monitor
pio run -t upload -t monitor

# Clean build
pio run -t clean
```

## Architecture

### Separation of Concerns

The emulator core (`lib/cybiko_core/src/core/`) is platform-independent C. It communicates with platform-specific code through a HAL interface (`src/hal/hal.h`) with four callbacks: `render_frame`, `audio_output`, `keyboard_poll`, `serial_output`.

`cybiko-emu/main.cpp` implements the T-Deck HAL: TFT_eSPI display with DMA, I2S audio at 48kHz, I2C keyboard polling from the T-Deck's ESP32-C3 keyboard controller (address 0x55), and SD card for ROM loading/NVRAM persistence.

### Core Emulator Modules (in `lib/cybiko_core/src/core/`)

| Module | Role |
|--------|------|
| `h8s_cpu.c` | H8S/2323 CPU instruction decode & execute |
| `address_bus.c` | Memory map and I/O routing |
| `hd66421.c` | HD66421 LCD controller (160x100, 4-shade grayscale) |
| `timer16.c` | 16-bit timer with PWM output compare (drives speaker) |
| `timer8.c` | 8-bit timer |
| `speaker.c` | Audio waveform reconstruction from timer transitions |
| `keyboard.c` | 10-column keyboard matrix scanning via DMA |
| `cfs.c` | Cybiko File System (524,544 byte image in external RAM) |
| `emulator.c` | Frame loop orchestrator |

The core compiles as a unity build (`cybiko_core_unity.c`) for cross-file inlining on ESP32.

### Display Pipeline

Cybiko LCD (160x100 grayscale) → 256-entry grayscale-to-RGB565 LUT → 2x nearest-neighbor upscale to 320x200 → DMA push to ST7789 at 80MHz SPI. Unchanged frames are not redrawn. The RGB565 buffer prefers internal DMA SRAM. Display is centered vertically on the 320x240 panel with 20px borders. Display > Scale 1x pushes the 160x100 frame unscaled, centred in the same area. The menu draws over the whole panel with TFT_eSPI calls (`draw_menu`).

### Keyboard Pipeline

The keyboard controller (ESP32-C3 at I2C 0x55) runs in raw mode (command 0x03, sent every boot because the controller keeps its mode across an ESP32 reset): 5 bytes per poll, one per column, bit n = row n, set while held. `keyboard_task` polls it every 5 ms and queues press/release events. In key mode the Mic key sends nothing at all, which is why raw mode is needed. `tools/keytest` env `keytest-raw` prints the bits.

Input goes through `cybiko-emu/rebiko_input.c`, plain C tested on the host (`tests/run_rebiko.sh`): physical key or trackball event → chord (alone, or after the `$` prefix) → binding lookup (user overrides in `g_cfg.keys`, then `rb_default_action`) → a Cybiko key, a typed character (base or Sym layer, as the controller's own tables), an emulator command, or menu navigation while the menu is open. Typed keys are queued taps: held 12 frames, then all keys up for 3 frames before the next (10 if it is the same key again). CyOS 1.2.46 misses a key held under 10 frames now and then, and a second tap of a key that is still down would only lengthen it ("ll" became "l"). `tools/v1_chat.sh` measured both. A tap whose T-Deck key is still held stays down past its 12 frames. Left and right Shift press the Cybiko's Shift and Fn at once, without holding up the queue.

Classic Backspace is BkSp (A.6 bit 2); `$` Backspace is Del (A.0 bit 2), which deletes forward. The Java emulator sent Backspace to Del.

`$` Mic opens the menu (`rebiko_menu.c`); it cannot be rebound. The menu pauses the guest (no `cybiko_run_frame`; silence keeps the I2S clock pacing the loop) and edits a copy of the settings, applied live and saved to NVS (`rebiko_config.c`, one CRC'd blob) when it closes, if changed. USB serial input still works for scripts: ASCII as the old key mode sent it, 0x11-0x14 trackball, 0x15 Mic, 0x16 click, 0x17 hold.

### SD Card Layout

ROMs must be on the SD card at:
- `/cybiko/boot.bin` — XT boot ROM
- `/cybiko/flash.bin` — XT CyOS
- `/cybiko/save.nvram` — XT RAM image, written via `save.tmp` every 5 minutes
- `/cybiko-v1/boot.bin`, `flash.bin`, `save.flash` — Classic env (`T-Deck-Cybiko-V1`); `save.flash` is written via `save.tmp` 10 s after CyOS's last flash page write

## Critical Technical Notes

### Keyboard Column Fix

The Cybiko XT keyboard has **10 scan columns (0-9 only)**. MAME and the Java emulator incorrectly place keys on columns 13-14, causing DMA aliasing (10-bit word offsets wrap).

### Audio via Timer16 PWM

CyOS generates audio through Timer16 channel 1 hardware output compare, NOT manual CPU port writes. Timer16's TIOR encoding: bit 2 = initial level, bits 1-0 = match action. `set_output_b()` fires a callback to `speaker_set_level()`. On counter clear (TGRA match), output resets to initial level.

### Machine selection

`lib/cybiko_core/src/core/machine.h` selects the machine at compile time: `-DCYBIKO_MACHINE_V1` for the Cybiko Classic, XT by default. Every memory-map and port constant that differs between them lives there, and the bus fast paths depend on them being constants. V1 boots CyOS 1.2.46 to the desktop on the host, and Chat works between two Classics: `tools/v1_chat.sh <boot> <flash> [frames] [screens dir]` walks both into Chat (MAIN), types on each and checks the other received it. CyOS schedules its own radio (random back-off on TMR1's overflow interrupt); `sci0_v1.c` only models the UART, hands received frames to CyOS while its radio is idle, and sets the CyID. Do not add a poll/scan driver back: the Java one existed only because TMR1's OVI never fired. Build it with `pio run -e T-Deck-Cybiko-V1` (stride 12, card folder `/cybiko-v1/`). It holds 60 fps from power-on, so audio never runs dry. Stride 8 needed more guest instructions per frame than the T-Deck runs. `tools/v1_boot.sh <boot> <flash> [frames] [screen.pgm]` prints the Java emulator's STATUS line (frame, PC, halted, CRC-32 of VRAM) every 60 frames for side-by-side comparison. See `../PLAN-V1.md`. After touching `machine.h` or the bus, the XT hashes from `tools/host_bench.sh` must not change.

### Timer and SCI2 batching (performance-critical)

`tick_timers_batch` advances every channel by a whole batch. `timer16_advance` must stay equivalent to calling `timer16_event` once per prescaler tick: it skips ticks that cannot hit TGRA, TGRB, or overflow, and drops whole clear-on-TGRA periods that set no new TSR flag and move no output pin. CyOS leaves channel 3 at φ/4 with TGRA=1; stepping that per tick was ~60% of the core. `cybiko_sci2_advance` (in `../cybiko-c-emulator/src/radio/sci2_xt.c`) is the same idea for SCI2 with a co-processor attached.

After any core change, run `tools/host_bench.sh <boot.bin> <flash.bin> 6000` before and after: the `ram`/`lcd` hashes must match. The baseline changed once, on 23 Oct 2026, with `irq_hold`. The H8S takes no interrupt right after LDC, ANDC, ORC or XORC to CCR; without that, CyOS's scheduler could stop every CyOS timer (see `../PLAN-V1.md`). For V1, `tools/v1_boot.sh` with `PRESS_AT=<frame>` taps the trackball, so a frozen guest shows up as a screen that stops changing. Then flash and read `tools/capture_serial.py`; the `idle` column is the headroom under the 60 Hz limit. With I2S up, the limit is the audio queue itself: `hal_audio_output` blocks until there is room for the frame's 800 samples, and `pace_frame` stands down. A non-blocking write dropped samples whenever the I2S clock and esp_timer drifted apart, which made the sound choppy.

### Interpreter speed on the ESP32-S3 (measured 23 Oct 2026)

- **The cost is fixed per guest instruction, not memory.** A guest NOP costs ~80 host cycles, and a load/add/branch loop ~130. With code and data in internal SRAM it is no faster: PSRAM adds only 10–20 cycles, even when every read misses the cache. Timers cost ~10 cycles per instruction.
- **Build flags.** The Arduino framework compiles everything with `-fno-jump-tables -fno-tree-switch-conversion -fstack-protector`. That turned each decoder `switch` into a compare-and-branch tree. `lib/cybiko_core/library.json` turns jump tables back on and the stack protector off for the core only. Do not remove those flags.
  - Jump tables live in flash rodata. That is safe only because the emulator never runs while the flash cache is off: the only flash writes, NVS for the volume setting, happen on the same loop task.
- **Dispatch.** `decode()` is inlined into `h8s_cpu_run`. Inlining the whole decoder tree made it slower (register spills).
- **Accounting.** `h8s_cpu_run` counts executed instructions in a local variable.
- **Measuring.**
  - Build with `PLATFORMIO_BUILD_FLAGS=-DCYBIKO_PSRAM_BENCH` for host cycles per guest instruction (NOP, MOV, loops; internal vs PSRAM) at boot.
  - Build with `-DCYBIKO_SPLIT_PROFILE` for a timers-vs-CPU split in the 5-second stats.

## PlatformIO Configuration

The active build environment is `T-Deck-Cybiko` in `platformio.ini`. Key flags:
- `-O2` optimization (overrides default `-Os`)
- `EMU_TIMER_STRIDE=20` — guest timer cycles per CPU instruction (a frame is ~15360 instructions)
- `ARDUINO_LOOP_STACK_SIZE=16384`
- TFT_eSPI configured entirely via build flags (not User_Setup.h)

Hardware pin definitions are in `cybiko-emu/tdeck_pins.h`. Custom board definition is in `boards/T-Deck.json`.

