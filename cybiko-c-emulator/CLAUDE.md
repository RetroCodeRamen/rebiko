# Cybiko C Emulator

Cybiko XT emulator ported from Java to C, targeting Linux (SDL2) and the original LilyGO T-Deck (ESP32-S3). Radio sources live in `src/radio/` and are shared with the firmware and the Heltec modem. Workspace design is in `../ARCHITECTURE.md`.

## Architecture

- **Core** (`src/core/`): Platform-independent emulator (H8S/2323 CPU, address bus, LCD, timers, RTC, speaker, keyboard)
- **HAL** (`src/hal/`): Hardware Abstraction Layer interface
- **Linux** (`src/linux/`): SDL2 frontend (display, keyboard, audio, `--radio`)
- **Radio** (`src/radio/`): co-processor, link codec, `radio_sim`, SX1262 PHY, Heltec serial PHY, `two_boot`

This is the XT host core. The Classic (V1) builds only from the T-Deck's vendored core (`../cybiko-lilygo/lib/cybiko_core`), and its host tools are `../cybiko-lilygo/tools/v1_*.sh`. The two cores have diverged. A fix to shared hardware (CPU, timers) belongs in both.

## Reference Implementations

- **Java emulator** (`../cybiko-java-emulator-main/`, local only, not in git): Original port source and the radio/SCI reference. Has known keyboard column bug (P on column 13).
- **MAME** (not on this machine): Older, incomplete XT support. Keyboard column definitions for XT are wrong (P on column 13 instead of column 9).

## Keyboard Column Fix (Critical)

The Cybiko XT keyboard matrix has 10 scan columns (0-9). CyOS scans these via DMA using 10-bit word offsets. MAME and the Java emulator incorrectly place P, period, and Help on column 13, which causes column aliasing: since DMA word offsets are only 10 bits, bits 10-14 are always 0, meaning columns 10-14 leak into every DMA scan result.

**Correct mapping (from web emulator):**
- Column 9: Help (0x0001), period (0x0002), semicolon (0x0008), P (0x0010)
- On/Off: NOT in keyboard matrix; read via Port A GPIO (bit 7)

**Wrong mapping (MAME/Java):**
- Column 13: Help (0x0001), period (0x0002), P (0x0010) -- CAUSES ALIASING
- Column 14: On/Off (0x8000) -- CAUSES ALIASING

The `keyboard_read()` function scans only columns 0-9 (`CYBIKO_KEYBOARD_SCAN_COLS = 10`). The array is sized to 15 for HAL communication but only 10 columns are scanned.

## Audio / Speaker Fix (Critical)

The Cybiko XT generates audio using Timer16 channel 1's hardware output compare (TIOCB1 = Port 1 bit 3 = speaker pin). CyOS does NOT manually toggle the speaker via CPU port writes - it configures the timer to produce PWM waveforms automatically.

**Timer configuration observed from CyOS:**
- `TIOR=0x50` → IOB=5 (`0101`): initial output HIGH, go LOW on TGRB match
- `TCR=0x32` → prescaler ÷16, counter clears on TGRA match
- TGRA defines the period, TGRB ≈ TGRA/2 for 50% duty cycle square wave
- Different TGRA values = different musical note frequencies

**PWM waveform generation:**
1. Timer starts / counter clears → output goes to initial level (HIGH)
2. Counter reaches TGRB → output goes LOW (compare match action)
3. Counter reaches TGRA → counter clears → output returns to initial level (HIGH)
4. Repeat → square wave at frequency = clock / prescaler / TGRA

**Implementation details:**
- `timer16.c`: TIOR nibble encoding: bit 2 = initial level, bits 1-0 = match action (01=clear, 10=set, 11=toggle)
- `timer16.c`: `set_output_b()` fires callback on level change; callback wired to `speaker_set_level()` in `emulator.c`
- `timer16.c`: On counter clear (TGRA match with clear_mode=1), output B resets to initial level
- `speaker.c`: Tracks per-cycle transitions within each frame for accurate waveform reconstruction
- `speaker.h`: Sample rate = 48000 Hz (matches web emulator)
- No-transition frames output silence (128) instead of DC offset (96/160)

**Key bug that was fixed:** Output compare pin level was initialized to 0, so "set LOW on match" was a no-op. Combined with missing "reset to initial level on counter clear", no audio was generated.

## App Loading

The `--app` flag injects `.app` files into the CFS filesystem before boot:
```bash
./cybiko-emu boot.bin flash.bin --nvram save.nvram --app game.app --app tool.app
```
- If NVRAM exists, loads it as the CFS base; otherwise formats fresh CFS
- Uses `cfs_add_file()` to inject each app by filename
- CFS image (524,544 bytes) loaded into external RAM at offset 0

## DMA Notes

- CyOS programs DMACR=0x8007: 16-bit mode, both source and destination FIXED
- DMACR FAE mode bit layout (from MAME h8_dma.cpp):
  - High byte: bit 7=MODE_16, bit 6=SRC_DEC, bit 5=SRC_INC_EN (0=fixed)
  - Low byte: bit 6=DST_DEC, bit 5=DST_INC_EN (0=fixed)

## Build

This machine has no cmake; use the Makefile:

```bash
make test                 # unit tests, including test_radio, test_sci0 (the Classic SCI0) and test_lora
make harness              # radio link across a 64-byte PHY MTU
make build/two_boot       # two XT CyOS instances chatting on radio_sim
make build/cybiko-emu     # SDL frontend; --radio /dev/ttyUSB0 for a Heltec
make build/modem_tool     # Heltec checks without CyOS
./build/cybiko-emu <boot.bin> <flash.bin> [--nvram file.nvram] [--app file.app ...] [--trace] [--radio DEV] [--cyid HEX]
```
