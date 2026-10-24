# Plan (Xtreme)

> **This is the Xtreme plan, and it is paused.** Since 23 Oct 2026 the work has been on the Classic. [PLAN-V1.md](PLAN-V1.md) is the live plan, and Chat over LoRa works there. The shared pieces built here carry over to the Classic unchanged: items 3, 5 and 8, the radio boundary, the link codec, the SX1262 driver and the Heltec modem. The radio profile has changed since item 8 was written; it is now 500 kHz and 14 dBm with listen-before-talk (`ARCHITECTURE.md`).

Ordered work for a T-Deck that boots original CyOS and speaks to a second T-Deck over LoRa. Each item has an acceptance check. "Host test" means it was run on this machine. "Hardware" means a T-Deck was flashed and observed. Hardware checks are open until that happens.

## Done in this tree

1. **Source audit and foundation choice.** Compared the Java reference, the C core, and the LilyGO port. Foundation is the C core plus the existing T-Deck HAL, with the Java co-processor as the radio spec. Acceptance: `ARCHITECTURE.md` names what is reused, what is missing, and why Reticulum is not the first transport.

2. **Board identity.** The vendored firmware pinout matches the original T-Deck (ST7789, keyboard 0x55, SX1262 CS 9), not T-Deck Pro. Missing radio pins are now defined, and setup holds the radio in reset-deasserted, chip-deselected idle. Acceptance: `tdeck_pins.h` matches the LilyGO T-Deck map for CS, RST, BUSY, DIO1, SPI, keyboard, trackball, and I2S.

3. **Guest radio boundary, host-tested.** `cybiko_radio_transport_t`, XT/V1 co-processor behavior, and the LoRa link codec. Acceptance: `make test` in `cybiko-c-emulator` passes, including beacon/chat strip and restore, poll-versus-scan, opaque frame types, fragmentation, CRC reject, PHY duplicate suppression, loss, latency, and three-peer delivery. `make harness` exchanges one beacon and one chat payload across a 64-byte PHY MTU.

4. **Keyboard map, host-tested.** The T-Deck ASCII map is a C module shared by the firmware and `test_tdeck_keymap`. Acceptance: letters, Fn-number layer, Shift, and the trackball matrix positions match the firmware that was already running.

5. **Firmware codec link.** The firmware compiles the same link sources and runs `cybiko_link_self_check` before CyOS. Acceptance: the host self-check returns 0, and `pio run` linked `cybiko-lilygo/.pio/build/T-Deck-Cybiko/firmware.bin`. On 22 Oct 2026 that image was uploaded to the attached T-Deck (`/dev/ttyACM0`, ESP32-S3 MAC `14:c1:9f:4a:b3:9c`). Serial printed `Radio codec self-check ok`.

6. **SCI2 DTC on both address buses.** Shared `sci2_xt` serves the host core and the T-Deck core. TX bit 6, RX bit 7, deferred `03` then indicator, and idle `try_async` are in. `cybiko_sci2_advance` skips countdown-only ticks. The frame loop ticks it by guest cycles. Acceptance: `make test` passed, including `test_sci2`. The XT firmware attaches the co-processor after the SX1262 starts.

7. **Desktop two-instance CyOS test.** `cybiko-c-emulator/build/two_boot` patches flash offset `0x7F818` with CRC-32 the way the Java emulator does, drives the first-boot wizard, pokes `settings.dat` so Communications is on, resets, and opens Chat. Acceptance: each side reaches the interactive UI, the CyIDs differ, and a unique chat string is in the peer's external RAM. On 22 Oct 2026 a 7000-frame run exited 0. After A typed `pingdeck` and B sent the stock line (so B's TX cycle drained the queue), B's RAM held `pingdeck` and B's screen showed `deck> pingdeck`. CyIDs `0x10000001` / `0x10000002`. This is not a two-board radio test.

7a. **Guest speed on the T-Deck.** Timer16 and SCI2 batching that are exact per-tick equivalents, a 60 Hz frame limiter, unchanged frames not redrawn, and the display buffer in internal SRAM. Acceptance: the host bench gives identical guest RAM and LCD hashes over 6000 frames before and after, with and without a co-processor; `make test`, `make harness`, and `two_boot --frames 7000` pass with unchanged output. On the T-Deck, 22 Oct 2026: the Loading screen went from 24 fps to a held 60 fps (922 k guest instructions/s, ~10% headroom), and the home screen comes up ~70 s after power-on. A build with the co-processor attached to the SX1262 vtable (TX refused) also holds 60 fps. Stride stays 20. Numbers and method are in `cybiko-lilygo/README.md`.

## Next

8. **SX1262 driver and LoRa link.** Shared by the T-Deck and a Heltec WiFi LoRa 32 V3.
   - **Driver:** `cybiko-c-emulator/src/radio/sx1262_phy.cpp`. Direct SPI, configured by a pin table: DIO2 RF switch, 1.8 V TCXO with recalibration, image calibration for the band, the PA, OCP, the TX-clamp and IQ errata, the private sync word, and continuous RX. TX starts and returns; TxDone and RxDone are read in `poll()` when DIO1 is high.
   - **Transport:** `radio_lora.c` puts `cybiko_radio_transport_t` over any packet PHY (`radio_phy.h`). It fragments with the link codec, keeps a TX queue that is all-or-nothing per message, and rejects sends while transmit is gated off.
   - **No radio task:** the T-Deck pumps it once per frame from the main loop, and only when the display DMA is idle.
   - **Host:** `make test` passed, including `test_lora`: chat crosses, transmit gate, one fragment on the air at a time, full queue, bad packet, both directions.
   - **On the attached T-Deck, 22 Oct 2026:** `SX1262 begin 0 status=0x52 errors=0x0000` (chip mode 5 = RX, and no XOSC flag left after recalibration). It holds 60 fps with the radio pumped, and the counters print every 5 s.
   - **Transmit:** the operator confirmed 915 MHz (US) on 23 Oct 2026. The T-Deck build sets `CYBIKO_RADIO_TX=1`, and both radios run at `CYBIKO_LORA_POWER_DBM=0` for desk range. Without `CYBIKO_RADIO_TX` the T-Deck receives only.
   - **Heltec modem:** `cybiko-heltec-modem/` (PlatformIO, `heltec_wifi_lora_32_V3`). SLIP frames over the CP2102 at 921600 baud (`modem_proto.h`), and the same `sx1262_phy` as the T-Deck. The PC side is `radio_serial_phy.c`, plus `--radio /dev/ttyUSB0 [--cyid HEX]` on the SDL frontend (`make build/cybiko-emu`) and `tools/modem_tool.c` for bench checks without CyOS.
   - **CyID:** the T-Deck patches the guest flash to `0x4C` plus the low 24 bits of its MAC and uses the same value as the link sender id. The PC defaults to `0x4C000001`.
   - **PC to T-Deck over the air, 23 Oct 2026:** `modem_tool --ping 1000` sent 34 link messages through the Heltec. The T-Deck counted rx 34, msgs 34, crc 0, link bad 0, RSSI -21 dBm, SNR 12, at 0 dBm across a desk.
   - **CyOS sends nothing until wireless is on** in `settings.dat` (`two_boot` pokes bytes 12, 13 and 41). An idle T-Deck at the desktop sent 0 packets in 100 s, which is expected.
   - **Shared-bus finding:** right after `tft.initDMA()` the Arduino SPI bus reads back 0 until the first DMA transfer completes, then it works again. The emulator draws a frame before the radio is pumped.
   - **Still open on the XT:** a CyOS frame from the T-Deck received on the PC, then discovery and chat in both directions. Do not mark this done from the raw ping alone.
   - **Done on the Classic, 23 Oct 2026:** Chat both ways, PC ↔ T-Deck, over this driver (PLAN-V1.md step 7).

9. **App loading without a rebuild.** The host half is done: `cfs_read_file` returns the bytes `cfs_add_file` stored, and `test_cfs` round-trips a 400-byte `demo.app` plus a second small app. A bad page CRC fails the read. The firmware scans `/cybiko/apps/*.app` before boot and adds names that are not already in `/cybiko/save.nvram`. The T-Deck now mounts the card and boots CyOS. Still open: watching CyOS list an injected app. A copy of the Cybiko app library lives on this machine as `Apps/` (not in git). **Later, after radio:** a PC loader that picks titles from that library and writes them onto the card or into the guest filesystem over USB, including a V1 AT45 CFS writer. Do not build that until LoRa CyOS frames work.

10. **Settings UI outside CyOS.** A small pre-boot or hotkey screen, drawn by the firmware, not by patching CyOS menus. ROM pick, CyID, keymap tweaks, frequency, SF, bandwidth, power, and the radio counters (CRC drops, duplicates, queue overflows). Acceptance: changing frequency changes the driver register and survives reboot from a file on the card. Hardware required for the visual check.

11. **Two-device CyOS chat.** Original chat.app or the built-in chat, unmodified. Acceptance: two T-Decks on the configured frequency discover each other and display a typed message. Host sim passing is not this check.

12. **Only then, multiplayer and routing.** File transfer and games, timed against the emulated radio rather than patched per title. A Reticulum transport is in scope only if it can carry the same guest payload without adding ACKs CyOS does not expect, and only after a license check. The vtable is already the insertion point.

## Not the success path

- A new chat application, a mock desktop, or a redrawn Cybiko UI.
- Reporting full speed from fps alone. At stride 20 a frame carries ~15360 guest instructions, so 60 fps is ~0.9 guest MIPS, well short of a real XT. Lowering the stride needs about 2x interpreter speed per halving, and the timer and boot tests, not a fps printout.
- Treating a local AVR `03` as proof the other radio heard the packet.
