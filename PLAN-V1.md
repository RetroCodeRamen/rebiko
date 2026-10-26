# Plan: Cybiko Classic (V1) on the T-Deck

**Where it stands, 24 Oct 2026:**

- Steps 1–4, 6 and 7 are done.
- From step 5, persistence works on the T-Deck. `.app` injection is still open.
- Chat runs over LoRa between ReBiko Desktop and a T-Deck.

What is left is under [Open](#open). The plan below keeps its original text and adds the results.

Written 22 Oct 2026 from the Java reference (`cybiko-java-emulator-main`), the C core (`cybiko-c-emulator`, `cybiko-lilygo/lib/cybiko_core`), and one headless Java V1 run on this machine.

## Why V1, not V2

The owner's own Cybiko was the original, not the Xtreme. Of the two original revisions, only V1 has a working reference to port from.

| | V1 (Classic) | V2 |
|---|---|---|
| ROMs on this machine | `cyrom112.bin` + `flash_v1246.bin` (CyOS 1.2.46), in `cybikov1.zip` | `cyrom117.bin` + CyOS 1.3.55/57/58, in `cybikov1.zip` under `cybikov2/` |
| Java reference | Boots to the UI. V1↔V1 and V1↔XT chat work (Java README) | Stalls at the animated logo; RF init never completes. MAME also fails |
| CPU | H8S/2241 @ 11.0592 MHz | H8S/2246 @ 11.0592 MHz |
| Guest RAM | 512 KB @ 0x200000 | 256 KB @ 0x200000 |
| CyOS storage | AT45DB041 SPI DataFlash (528 KB), read through SCI1 + DTC | 256 KB memory-mapped flash + the same SPI flash |

Checked here: `java com.github.daberkow.CybikoEmulator --machine v1 --headless` (compiled into the scratchpad, not in the tree) reached CyOS's idle loop (`PC=0x219C8E`, halted) by frame ~540, about 9 s of guest time. The last frame is a CyOS icon screen. Java runs one instruction per cycle. The T-Deck V1 env uses stride 12; on-device boot time is unknown until the first UI is observed.

V2 stays out of scope until someone finds why its RF init never completes. The machine config below leaves room for it.

## Why V1 could suit the T-Deck better than the XT

- 11.06 MHz is 184,320 cycles per frame, against the XT's 307,200. At stride 20 that is ~9,200 guest instructions per frame instead of ~15,400. The T-Deck core now does ~920k guest instructions/s on the XT, so V1 has room either for headroom or for a lower, more faithful stride (about 12 for the same host load). Not measured.
- CyOS runs from guest RAM in PSRAM, as on the XT, so cycles per instruction should be similar (~230). 512 KB does not fit internal SRAM next to the display buffer (~298 KB free at boot without it).

## What exists already

- **C radio co-processor.** `cybiko_radio_coproc_set_v1()` and `cybiko_radio_coproc_v1_tx_complete()` are ported: V1 frames have no trailing status byte, and an empty V1 exchange uses indicator `C8`.
- **Java V1 reference.** `MachineConfig.java` (memory map, pins, ports), `AT45DB041Flash.java` (203 lines), the SCI1/DTC SPI path and SCI0 radio path in `AddressBus.java`, the V1 keyboard matrix in `CLAUDE.md`, and the V1 bug list in `docs/research/bugs-fixed.md` (#9, #10, #12, #13, #14, #15).
- **Shared pieces that should carry over unchanged:** the H8S CPU core (same H8S/2000 instruction set), HD66421 LCD (only its address moves), Timer8, Timer16 (V1 has 3 channels), the PCF8593 RTC, the speaker, the frame limiter, and the host bench and hash method.

## What the C core hardcodes as XT

Each item is a constant or branch in `address_bus.c`, `emulator.c`, `keyboard.c`, or `cfs.c` today:

| Area | XT (now) | V1 (needed) | Java reference |
|---|---|---|---|
| External RAM | 2 MB @ 0x400000 | 512 KB @ 0x200000 | `MachineConfig` |
| On-chip RAM / I/O | 0xFFDC00, 0x2400 bytes | 0xFFEC00, 0x1400 bytes | `MachineConfig` |
| LCD | 0x100000 | 0x600000 | `MachineConfig` |
| Memory-mapped flash | 512 KB @ 0x600000 | none | `MachineConfig` |
| Boot ROM mirror | to 0x03FFFF | 32 KB only | `MachineConfig` |
| Flash storage | in the address map | AT45DB041 on SCI1, CS = Port 3 DR bit 4 (0xFFFF62), DTC block at 0xFFFBD0, DTCER 0xFFFF34 bit 1 | `AT45DB041Flash`, `AddressBus` DTC section |
| DMA | 4 channels (keyboard scan) | none; keyboard read directly | `AddressBus` |
| Keyboard | 10 scan columns, 16-bit, Fn+letter digits | 9 columns, 8-bit, dedicated digits, IRQ2 (vector 18) on key press | `CLAUDE.md` V1 matrix |
| Power status | Port A = 0xC0 | Port 1 input (0xFFFF50) bit 3 | `MachineConfig` |
| RTC I2C SDA | Port F DR bit 6, inverted | Port F DDR bit 0, open-drain (bug #10) | `MachineConfig`, `AddressBus` |
| ADC | fixed values | ch1 = 0x0300, ch2 = 0x0100, or CyOS's battery check fails (bug #14) | `AddressBus` ADC read |
| ICR registers | absent | present (H8S/2241 interrupt controller) | `AddressBus` ICR |
| Timer16 | 6 channels | 3 channels | `MachineConfig` |
| Radio UART | SCI2 + DTC | SCI0, interrupt-driven (TXI0 82, RXI0 81) | `AddressBus` SCI0 section, `RadioCoProcessor` v1 mode |
| Filesystem | CFS in RAM, 258-byte pages, CRC16 | CFS in the SPI flash, 264-byte pages, CRC32 + write count | `manager` `FlashGeometry.AT45DB041`, `CfsPage` |
| Save file | 2 MB RAM image (`save.nvram`) | the 528 KB SPI flash image (programmed through command 0x82) | — |

## Work, in order

Sizes are rough: S is under a day, M is 1–3 days, and L is a week or more of iteration.

1. **Machine selection in the core (M). Done 22 Oct 2026.** A compile-time `CYBIKO_MACHINE_V1` / `_XT` switch, not a runtime struct. The bus fast paths in `h8s_cpu.c` compare against constant ranges, and the ESP32 speed depends on that. The host builds both.
   *Acceptance:* the XT build gives identical `tools/host_bench.sh` hashes over 6000 frames before and after, and `two_boot --frames 7000` passes.
   *Result:*
   - `cybiko-lilygo/lib/cybiko_core/src/core/machine.h` holds the per-machine constants: clock, RAM, on-chip base, LCD, boot mirror, flash map, USB, DMA, Timer16 channel count, keyboard columns, power port, and RTC SDA bit. `address_bus.c`, `h8s_cpu.c`, and `emulator.c` use them. The XT is the default.
   - XT hashes are identical over 6000 frames, with and without `BENCH_RADIO=1`, and the firmware is byte-for-byte the same size.
   - The c-emulator `test_timer16`, `test_timer8`, `test_h8s_cpu`, `test_hd66421`, `test_memory`, and `test_keyboard` pass against this core.
   - Both machines compile with `-Wall -Wextra` and no warnings. `MACHINE=V1 tools/host_bench.sh` runs the V1 boot ROM without crashing; it busy-loops with no SPI flash, as expected.
   - `two_boot` lives in `cybiko-c-emulator` on the other core, which this step did not touch, so it was not rerun.
   - On V1 the XT's USB stub range (0x200000–0x200003) would shadow the start of RAM. It is compiled out there.

2. **V1 memory map and peripherals (M). Done 23 Oct 2026.** Everything in the table above except SPI flash, CFS, and radio: RAM, on-chip base, LCD address, no DMA, 3 Timer16 channels, ICR, power port, RTC DDR wiring, ADC values, 9-column 8-bit keyboard with IRQ2.
   *Acceptance:* the host V1 build runs the boot ROM to the same first VRAM hash as Java (`CC3A6F3D`, frames ~1–60).
   *Result:* all under `CYBIKO_MACHINE_V1` in the vendored core:
   - ICR at 0xFFFEC0-2, and Port F SDA through the DDR (`CYBIKO_RTC_SDA_VIA_DDR`).
   - The ADC returns 10-bit left-aligned results from `bus->adc[]` (Java's defaults), raises ADI (vector 28), and clears ADF the MAME way.
   - A key press raises IRQ2 (vector 18).
   - The XT RAM and LCD hashes are identical to the pre-V1 core at all 24 checkpoints over 6000 frames.

3. **AT45DB041 + SCI1 SPI + DTC bulk read (M). Done 23 Oct 2026.** Port `AT45DB041Flash.java`. Model the DTC bulk transfer the way Java does: run the whole transfer when SCR is written with DTC enabled.
   *Acceptance:* host V1 passes `4ACC524E` (SPI loading) and `5580FBFF` (decompression).
   *Result:*
   - `core/at45db041.c` ports the Java model without allocating per read.
   - SCI1 TDR clocks one byte, and an SCI1 SCR write runs the DTC block at 0xFFFBD0.
   - Completion is vector 85, deferred one CPU batch (Java waits 5 instructions).
   - Port 3 DR bit 4 drives CS when Port 3 DDR bit 4 is set.

4. **Host boot to the CyOS UI (M, open-ended). Done 23 Oct 2026.** Debug against Java frame by frame. For the comparison runs, build the host core at `EMU_TIMER_STRIDE=1`, which is Java's timing. Expect C-port gaps that only V1 CyOS exercises: opcodes, port defaults, interrupt priorities.
   *Acceptance:* the VRAM hash sequence matches Java through `C0DBEF72` (logo) to the idle UI at `PC=0x219C8E`. Then confirm at stride 20 that it still reaches the UI, and record the frame count.
   *Result:* `tools/v1_boot.sh` prints Java's STATUS line (frame, PC, halted, CRC-32 of VRAM) every 60 frames.
   - **Stride 1:** matches Java at every checkpoint (`CC3A6F3D`, `4ACC524E`, `5580FBFF`, `C0DBEF72`, `C8D41E13`), and is idle at `0x219C8E` by frame 540, as Java is.
     - Its desktop hash differs from Java's only because of the clock: the C RTC starts at 12:00 AM, while Java reads the host time. The two screenshots are otherwise identical.
     - This worked on the first attempt; no C-port gaps showed up.
   - **Other strides:**

     | Stride | Desktop reached |
     |---|---|
     | 8 | frame ~2580 |
     | 12 | frame ~3780 |
     | 20 | never: still on the logo (`C0DBEF72`, halted) at frame 9000 |

     Boot is instruction-bound at roughly 55–60 M guest instructions whatever the stride, so on the T-Deck expect about a minute, like the XT. The V1 env uses stride 12. Why 20 hangs is not investigated.

5. **Persistence and apps (M). Persistence done on the T-Deck; app injection open.** Write the SPI flash image back to the card when it changes, the way `save.nvram` works now. Add an AT45 CFS writer (264-byte pages, CRC32) so `/cybiko/apps/*.app` can be injected for V1. The XT CFS writer stays.
   *Acceptance:* the host round-trips a 400-byte `.app` through the V1 writer. A setting changed in CyOS survives a host restart.
   *Status:*
   - The T-Deck saves the whole 540672-byte chip as `/cybiko-v1/save.flash`, and boots from it in place of `flash.bin`. Deleting the file resets the machine.
   - The save happens 10 s after the last change to `at45db041_t.writes`, and a failed save retries 10 s later.
   - Verified on hardware, 24 Oct 2026: Mute changed in CyOS Settings was saved 11 s later and survived a reset.
   - The AT45 CFS writer for `.app` injection is not written.

6. **T-Deck V1 firmware (S–M). Done 23 Oct 2026.** A second PlatformIO env, `T-Deck-Cybiko-V1`, with card paths under `/cybiko-v1/` and a V1 keymap. Digits map directly, since V1 has number keys.
   *Acceptance on hardware:* boots to the UI from the card, the serial log shows fps, kIPS, and `idle`, and power-on to UI time is recorded.
   *Status:*
   - `pio run -e T-Deck-Cybiko-V1`: `CYBIKO_MACHINE_V1`, stride 12, card folder `/cybiko-v1/`. Stride 8 fell to ~50 fps under load; 12 holds 60 fps on the host arithmetic and is what `platformio.ini` ships.
   - Leaves out the XT CyID patch, `.app` injection, and the SCI2 co-processor.
   - Keymap: `tdeck_keymap.c` has a V1 table from the Java V1 matrix, plain keys and digits only. Backspace is the Del position, as in Java.
   - On hardware: boots CyOS 1.2.46 from the card and holds 60 fps.
   - `$` commands give Esc, Help, F1–F7 and volume (see `cybiko-lilygo/README.md`).
   - Typed keys are queued taps that CyOS cannot miss.

7. **V1 radio (L, highest risk). Done 23–24 Oct 2026.** The plan was to port the SCI0 interrupt-driven path, then the Java V1 driver layer. That layer was a set of workarounds tied to CyOS 1.2.46 RAM addresses:
   - It bootstrapped `connObj` by injecting `0x03`.
   - It drove poll and scan every 15 frames, because "CyOS never calls the tick function."
   - It restored `connObj` each cycle and patched the CyID at `0x21F9DC`.
   - It resolved the RF service at `0x223186`.

   *Acceptance:* two host V1 instances chat (a V1 `two_boot`), then V1↔XT on the sim bus, then over LoRa with the SX1262 driver from `PLAN.md` item 8.

   *Result:*
   - **The driver layer was not needed.** CyOS does call its tick, from TMR1's overflow interrupt. The C and Java `timer8` raised OVI only on a 0→1 edge of OVF, and OVF1 is already set when CyOS enables it.
   - **The fix.** With a level-sensitive OVI, CyOS runs its own radio. `sci0_v1.c` keeps only the UART, async RX while CyOS's radio is idle, and the CyID. `ARCHITECTURE.md` has the details.
   - **Host test.** `tools/v1_chat.sh` passes: two Classics in Chat MAIN, each sees the other's line.
   - **Over LoRa.** ReBiko Desktop on a Heltec ↔ T-Deck passes 6 of 6, 3 lines each way, including both sides sending within 0.1 s.
   - **Rooms.** Rooms are radio channels, and the co-processor drops frames off the tuned channel, so rooms stay apart.
   - **Not tested:** V1↔XT, two T-Decks over the air, private messages and games.

## Battery gauge (investigated 23 Oct 2026)

The owner asked for the T-Deck battery to drive the Classic's gauge.

**How CyOS 1.2.46 uses the battery channels** (from disassembly; addresses are in decompressed CyOS):
- **ADC ISR** (`0x21DBA4`): stores channels 0–2 in the table at `[0x227FCC]+4`.
- **Battery task** (`0x21DED6`): about every 80 frames it smooths ch1 and ch2 (7/8 old + 1/8 new) into the battery object at `[0x227FD0]` (+0x1C, +0x1E). When two raw readings in a row are within 9, it evaluates them (`0x21DD5A`):
  - `v = 2*avg2 - avg1`.
  - **ch1 − ch2 > 15:** charger present. Level = v − 349, and the gauge shows the charging animation.
  - **Otherwise:** on battery. Level = v − 341, clamped to 31–78.
  - Port `0xFFFF64` bit 3 is set or cleared as a charge control.
- **Boot check** (`0x21DF96`): passes if ch1 − ch2 > 15 or level > 31.
- **Desktop status bar** (`0x25B6AA`):
  - On battery, the icon shows only when level ≤ 42, and it blinks.
  - Above 42 the icon is blanked.
  - Bar width is (level − 31) × 21 / 47 pixels.
- **Java's defaults** (ch1 0x300, ch2 0x100) therefore mean "on the charger": the icon is the charging animation and never shows a level.

**Open problem:** with on-battery values the machine never idles.
- This covers ch1 ≈ ch2, or ch1 = 0, at any level of about 43 or more, and also 30 or less.
- From the desktop onward the CPU never sleeps. A callback (`0x25DF54`, table `0x25E87E`) posts message 0x20 to `desktop` continuously, and the screen stops updating.
- The Java emulator does the same with the same values (`-Dadc1=396 -Dadc2=396` on a patched copy), so it is a gap common to both emulators, not a C-port bug.
- Levels 36–39 idle normally with the blinking low icon.
- Port 1 (0xFFFF50) and ADC channel 0 have no effect on it.
- **What ships (24 Oct 2026):** the guest always gets Java's charger values. It has to: with on-battery values CyOS also stops sending on the radio.
  - The firmware patches CyOS's two `CMP.W #42` low-icon checks (`0x25B760`, `0x25B7CE`) to 0, so CyOS's battery icon stays off.
  - The T-Deck's own top bar shows the LiPo level.
  - `cybiko_set_adc()` is there for a real passthrough if the idle bug is ever found.
- **The passthrough plan,** if that bug is fixed: charger mode while the T-Deck is on USB. On battery, `ch1 = ch2 = 341 + level`, with level from the LiPo voltage mapped onto 31–78.

## Risks and open questions

- **Hangs at some strides: root-caused and fixed 23 Oct 2026.**
  - **Mechanism:** CyOS's scheduler (`0x204698`) re-enables interrupts with `ANDC #0x7F,CCR` one instruction before it loads the new task's SP (`0x20469A`). The H8S accepts no interrupt right after an instruction that writes CCR, and CyOS relies on that.
  - **What the emulators did:** both the Java and the C emulator took a pending interrupt there. The tick handler (`0x20736A`) then ran its stack check (`0x205594`) with the old SP against the new task, took the overflow path, and never cleared its "timers in progress" flag `[0x21F06E]`. Every later tick counted a missed tick (`[0x21F084]`), and no CyOS timer fired again.
  - **Symptoms:** a boot that stops on the logo, a desktop that freezes and ignores keys, and the stride sensitivity.
  - **Fix:** `cpu->irq_hold` in `h8s_cpu.c`. It was also applied to `cybiko-c-emulator`, where `make test` and `two_boot --frames 7000` still pass.
  - **After the fix:** strides 8–16 all boot and answer key presses, and stride 12 does in 8 of 8 timing variants.
  - **Effect on the XT:** it boots to its welcome screen, but its host-bench hashes changed; the new values are the baseline.

- **The V1 radio driver was a workaround layer: resolved 23 Oct 2026.** The missing tick was TMR1's overflow interrupt (step 7). Nothing in the radio path depends on CyOS 1.2.46 RAM addresses now except the CyID at `0x21F9DC`.
- **C-port gaps.** None showed up in step 4.
- **The XT stays.** Its radio work, `two_boot`, and the hash checks remain the regression net for every core change.

## Open

- **Range.** The goal is the most distance with no lost messages. Since 24 Oct the power is 22 dBm (+8 dB), and `$l` with `modem_tool --ping` measures loss and SNR in both directions.
  - **Next lever: SF.** The bandwidth stays at 500 kHz, because that keeps the fixed channel under FCC 15.247. SF8 gains about 2.5 dB and SF9 about 5 dB. A Chat frame takes about 82 ms on air at SF7, 146 ms at SF8 and 260 ms at SF9.
  - **Antennas** are the cheapest gain after that.
- **More of CyOS over the air:**
  - two T-Decks;
  - private messages;
  - the people list;
  - multiplayer games (airtime, latency, frames over 255 bytes).
- **`.app` loading** for the Classic: the AT45 CFS writer (step 5), then a PC loader.
- **Internet relay** (`PLAN-RELAY.md`, not in git). This is on hold.
- **Battery passthrough:** blocked on the idle bug above.
