# Architecture

ReBiko runs unmodified CyOS on new hardware:

- **ReBiko Pocket:** the original LilyGO T-Deck (ESP32-S3, ST7789, SX1262).
- **ReBiko Desktop:** a PC, with a Heltec V3 running **ReBiko Radio** as its LoRa radio.

LoRa is a transport under the emulated AVR radio co-processor, not a new Cybiko application. The Cybiko Classic (V1, CyOS 1.2.46, or 1.2.57 after the System Pack update) is the main machine. The Xtreme (XT, CyOS 1.5.08) is the second.

The work order is [PLAN.md](PLAN.md). The latest measured numbers are in [README.md](README.md) and [cybiko-lilygo/README.md](cybiko-lilygo/README.md#performance).

## Decision

Use Dan Berkowitz's C emulator as the guest core, and the existing `cybiko-lilygo` firmware as the T-Deck host. Extend them. Do not replace the H8S interpreter with a new CPU core. Speed work is batching, placement, and the existing decoder, checked against RAM/LCD hashes.

The Java emulator remains the behavioral reference for the radio co-processor, SCI/DTC sequencing, and the bytes CyOS actually exchanges. It is not the runtime. The T-Deck cannot host a JVM, and the finished device must boot CyOS with no PC attached.

The first wireless implementation is a small LoRa link that carries the same guest payload the Java emulator already sends over UDP. Reticulum is not linked in. The transport vtable is the seam where a later gateway, or an on-device stack, can replace the link without another CyOS change.

## Trees

| Path | Role |
|---|---|
| `cybiko-c-emulator` | Host H8S core (XT), unit tests, all radio sources (co-processor, SCI0/SCI2, link codec, `radio_sim`, SX1262 PHY, Heltec serial PHY), `two_boot` |
| `cybiko-lilygo` | ReBiko Pocket firmware: vendored core (XT default, V1 env), display, keyboard, audio, SD, LoRa pump. Also ReBiko Desktop (`tools/v1_emu.c`) and the host V1 checks (`tools/v1_*.sh`), built against the vendored core |
| `cybiko-heltec-modem` | ReBiko Radio: Heltec WiFi LoRa 32 V3, same SX1262 PHY, SLIP to the PC |

The Java emulator (`cybiko-java-emulator-main` on this machine) is a local MIT reference for co-processor, SCI/DTC, and V1/XT/V2 behavior. It is not part of the GitHub repo.

The two C cores have diverged. The Linux tree is what `make test` covers. The T-Deck tree adds PSRAM placement, sleep fast-forward, `EMU_TIMER_STRIDE`, Timer16/SCI2 batching, and a compile-time machine switch (`machine.h`). Radio sources live once under `cybiko-c-emulator/src/radio/` and are compiled into firmware from there.

## Target hardware

Original T-Deck, not T-Deck Pro and not a requirement on T-Deck Plus GPS.

| Function | GPIO |
|---|---|
| ESP32-S3FN16R8, 240 MHz, 16 MB flash, 8 MB OPI PSRAM | — |
| Peripheral power enable | 10, driven high |
| Shared SPI SCK / MISO / MOSI | 40 / 38 / 41 |
| ST7789 CS / DC / backlight | 12 / 11 / 42, no RST pin |
| SX1262 CS / RST / DIO1 / BUSY | 9 / 17 / 45 / 13 |
| microSD CS | 39 |
| Keyboard I2C SDA / SCL, address 0x55, INT | 18 / 8 / 46 |
| Trackball up / down / left / right / click | 3 / 15 / 1 / 2 / 0 |
| Speaker I2S WS / BCK / DOUT | 5 / 7 / 6 |

T-Deck Pro is a different machine: 320×240 e-paper, SPI on 36/33/47, SX1262 on CS 3, and a TCA8418 keyboard at I2C 0x34. This firmware must not be built against that pinout.

The SX1262 shares SPI with the display and the card. Every radio transaction leaves TFT CS and SD CS high. DIO2 is the RF switch and DIO3 is a 1.8 V TCXO; `sx1262_phy.cpp` programs both.

Display path: HD66421 160×100 grayscale, nearest-neighbor 2× to 320×200, centered on the 320×240 panel with 20 px bars. Unchanged frames are not redrawn. The RGB565 buffer is in internal DMA SRAM when a block is free.

Trackball click is CyOS Enter (`col 4`, `0x0008`), not Select. Select does not open an icon or leave the first-run pages.

## Guest radio boundary

CyOS never sees LoRa, UDP, or Reticulum. The boundary is the emulated AVR co-processor.

```
CyOS
  → SCI2 TX / DTC (XT) or SCI0 (V1)
  → emulated AVR co-processor
  → cybiko_radio_transport_t        guest payload + channel
  → link codec                      fragment if the PHY needs it
  → radio_phy (SX1262, serial modem, or radio_sim)
  → peer transport
  → peer co-processor prepare_rx    frame as sent, padded to 50 or 200
  → SCI RX / DTC
  → peer CyOS
```

`cybiko_radio_transport_t` matches the Java `RadioTransport` contract:

- `send_packet(payload, channel)` is non-blocking. It copies bytes and returns.
- A listener later receives `(payload, channel, sender CyID)`.
- `pump()` is how the host moves packets. The CPU frame loop must not sit inside an SPI transfer or a LoRa airtime wait.
- `set_channel()` records the channel CyOS selected. The transport does not filter by channel; the co-processor does (below).

Byte 8 of every frame, the first after the address header, is `0xC0 | channel`. CyOS tunes with co-processor command `0x02`, and from then on `radio_coproc.c` drops frames for any other channel. This mirrors a real radio, and it is what keeps Chat rooms apart:

| Channel | Used for |
|---|---|
| 2 | desktop |
| 4 | Chat MAIN |
| 6, 8, 10 | Romantic, Computers, Sports |
| 52, 54 | Games, Music |

CyOS retunes between these by itself as you move between screens.

A frame goes on the air as CyOS wrote it, address header included (link version 2; a version 1 peer is dropped):

| Offset | Bytes |
|---|---|
| 0–3 | destination CyID, `FF FF FF FF` for everyone |
| 4–7 | sender CyID, big-endian |
| 8+ | channel byte, type, data |

XT frames drop their trailing status byte (51→50, 201→200). V1 frames have none. Anything of 9 bytes or fewer is an AVR command, not a frame, and is not transmitted. On receive, `prepare_rx` hands CyOS the frame unchanged, padded with `FF` to 50 or 200 bytes. The real AVR filters nothing, so a frame addressed to one Cybiko reaches every Cybiko in range, and CyOS on each decides. A null read (Xtreme) is filled with `00`, not `FF`: an all-`FF` buffer passes CyOS's broadcast check and leaks connection objects.

Local `03` bytes come from the emulated AVR when a frame has gone out. They are not a claim that a peer received it. The link layer does not emit its own ACK.

The Classic replies as its AVR firmware does (`avr19c6.bin`, in CyOS 1.2.57's `System.a`): nothing to an `01` command, `03` after a frame, and a `32`/`C8` frame start only when a received frame is waiting. `30` and `CF` start a small (50-byte) or large (200-byte) frame. Neither the firmware nor CyOS relays a frame; `tools/v1_three.sh` with `CHAIN=1` checks that. The Xtreme's AVR firmware is not available, so its replies stay as the Java emulator's: `03` to every command, and after a frame `32` for a small queued frame or an empty queue, `C8` for a large one. After a small frame (`30`) the co-processor does not hand over a waiting frame longer than 50 bytes; it waits for a large one (`CF`) or for the idle delivery.

The Java co-processor waits up to 15 ms inside `complete_tx_dtc`. That wait is not ported. A 200-byte Chat frame plus the 16-byte link header is about 86 ms on the air at SF7/500 kHz/4/5; a 50-byte presence frame is about 31 ms. Frames that arrive later are delivered by `try_async` when the guest radio is idle.

The T-Deck attaches the co-processor after the SX1262 starts. `CYBIKO_RADIO_TX=1` (set in `platformio.ini`) lets it transmit. Without that flag the board receives only. CyOS sends nothing until wireless is on in its settings (`settings.dat`).

### The Classic radio (SCI0)

CyOS schedules its own radio, and the emulator only has to model the hardware it uses (addresses are 1.2.46's):

- **The scheduler.** Posting a job (`0x214A6C`) or finishing one calls the radio tick (`0x214760`) with a random back-off. The tick loads TCNT1 and enables OVIE, and TMR1's overflow interrupt calls the tick again, which starts TXI0.
- **The bug that blocked Chat.** TMR overflow interrupts must be level-sensitive. OVF1 is set early in boot with OVIE off and never cleared, so an edge-only OVI meant CyOS's radio never ran. The Java emulator worked around that with a poll/scan "driver" that guessed at CyOS's RAM. That driver caused lost, late, echoed and junk Chat frames. It is gone, and must not come back.
- **What `sci0_v1.c` does.** It models the UART, hands received frames to CyOS while its radio is idle, and keeps CyOS's CyID cache set to the radio id. 1.2.46 sends a frame byte by byte through TXI0. 1.2.57 moves it by DTC (DTCERE `0xFFFF34`, TX block `0xFFFBC4`, RX block `0xFFFBB8`), as the Xtreme does; `sci0_v1.c` switches to that when CyOS first starts one, and back on reboot. Each version's radio object and CyID cache are in `V1_LAYOUTS`, chosen by the version string in RAM (1.2.46 CyID `0x21F9DC`, 1.2.57 `0x222CF4`). `tests/test_sci0.c` in `cybiko-c-emulator` covers the DTC path.

Frames on the air are CyOS's own:

| Frame | Type byte | Size |
|---|---|---|
| Presence | `0x00` | 50 bytes |
| Chat room joins and messages | `0x20` / `0x28` | 50 or 200 bytes |

Sizes include the 8-byte address header; the type is the byte after the channel byte.

CyOS sends each Chat message 3 times. Nicknames are CyOS's own (set on first boot, or in About Me).

CyOS misses a key held under 10 frames now and then. The T-Deck queues typed keys: 12 frames down, then 3 up, or 10 up before the same key again. ReBiko Desktop holds each key at least 12 frames (`MIN_HOLD`).

Host checks for the Classic, in `cybiko-lilygo/tools/`:

| Script | What it checks |
|---|---|
| `v1_boot.sh` | One Classic boots, compared against Java's STATUS line |
| `v1_two.sh` | Two Classics on `radio_sim` |
| `v1_chat.sh` | Two Classics walk into Chat MAIN and each sees the other's line |
| `v1_three.sh` | Three Classics in Chat MAIN; `CHAIN=1` puts A and C out of each other's range and checks nothing is relayed |

`v1_airchat.c` predates the fix and still looks for the old type `0x01` beacons.

## Link layer and PHY

SX1262 explicit-header mode carries at most 255 bytes. CyOS frames are 50 or 200 bytes. With a 16-byte link header they fit in one PHY packet (66 and 216 bytes). Fragmentation still exists so a smaller MTU does not need a CyOS change.

```
0     magic 0xC7
1     version 1
2     flags (reserved, 0)
3–4   message id, big-endian
5     fragment index
6     fragment count (1–8)
7     channel
8–11  sender CyID, big-endian
12–13 total guest payload length
14–15 CRC-16/CCITT-FALSE over the header (CRC field zero) and this chunk
16+   chunk
```

No transport ACK and no transport retry. CyOS is the protocol that retransmits. PHY duplicates are dropped by message id; CyOS's own 3× retransmit gets a new id each time. Each sender numbers its messages in sequence, so `radio_lora.c` counts a gap as `rx_lost` (a jump of more than 64 is a restart). Range-test pings (`$l`, `modem_tool --ping`) use channel 0, which CyOS never tunes.

`sx1262_phy.cpp` is shared by the T-Deck and the Heltec modem: DIO2 RF switch, 1.8 V TCXO with recalibration, image calibration, PA, OCP, TX-clamp and IQ errata, private sync word `0x1424`, continuous RX. TX starts and returns; TxDone/RxDone are read in `poll()` when DIO1 is high.

`radio_lora.c` is the `cybiko_radio_transport_t` over any `radio_phy`. One fragment on the air at a time. Sends are rejected while transmit is gated off.

Before each send, `sx1262_phy.cpp` listens before talking. It waits a random 0–30 ms. While a preamble or header is latched, it waits one longest packet plus a random amount, and gives up after 2 s. The T-Deck stats line counts these waits as `waited`, and the Heltec status line counts them as `wait`.

There is no dedicated radio task. The T-Deck pumps the radio from the Arduino loop twice per frame, before and after `cybiko_run_frame`. If DIO1 is high (a packet is waiting) and display DMA is busy, it waits for the DMA to finish first. Right after `tft.initDMA()`, the Arduino SPI bus reads back 0 until the first DMA transfer; the emulator draws a frame before the first radio pump.

Default profile:

| Parameter | Value | Notes |
|---|---|---|
| Frequency | 915.0 MHz | US915 starting point. Not legal everywhere |
| Bandwidth | 500 kHz | Chat is ~82 ms on the air |
| Spreading factor | 7 | |
| Coding rate | 4/5 | |
| Power | 22 dBm | `CYBIKO_LORA_POWER_DBM`, the SX1262 maximum; bench builds may pass less |
| Sync word | 0x1424 | Private LoRa, not LoRaWAN |
| Preamble | 8 symbols | |

The profile lives in `cybiko-c-emulator/src/radio/radio_transport.h` and is compiled into both the T-Deck and the Heltec. Change it, and flash both.

The T-Deck CyID is `0x4C` plus the low 24 bits of its MAC. On the XT it is patched into flash, the way the Java emulator patches `0x7F818`. On the Classic, `sci0_v1.c` writes it to RAM at `0x21F9DC`. ReBiko Desktop defaults to `0x4C000001` (`--cyid`).

## Why not Reticulum as the first transport

Reticulum is a real network stack, and a gateway later is attractive. It is the wrong thing to put under CyOS on this chip right now.

- The guest model is broadcast datagrams with the CyID inside the frame. Reticulum data delivery is path-routed and acknowledged.
- A 192-byte guest payload plus a Reticulum header does not reliably fit in a 255-byte LoRa PHY packet.
- Path tables would spend RAM and CPU the interpreter already needs.
- The Python Reticulum stack is not MIT. Nothing is linked here.

`cybiko_radio_transport_t` does not mention LoRa. A later Reticulum wrapper can sit on the same vtable after a license review, without changing CyOS.

## Runtime shape on the ESP32-S3

```
Core 1 (Arduino loop)
  keyboard I2C, trackball
  cybiko_run_frame
  display DMA (skip if the 160×100 frame is unchanged)
  audio write (blocks until the 800-sample slot is free)
  radio pump when DMA is idle

Guest core
  H8S interpreter (IRAM, -O3, jump tables restored for this TU)
  timers (Timer16 skips ticks that cannot match)
  SCI2 (skips countdown-only ticks when a co-processor is attached)
  LCD, speaker, CFS
```

`EMU_TIMER_STRIDE=20` on XT (12 on the V1 env). Each guest instruction advances the timers that many cycles, so an XT frame is about 15360 instructions. 60 fps is about 0.9 guest MIPS, not a real XT. The 60 Hz limiter (`pace_frame`) keeps RTC, key hold, and audio from running fast once the core is ahead of real time.

Memory: emulator state, on-chip guest RAM, and boot ROM in internal SRAM; 2 MB (XT) guest RAM and the flash image in PSRAM. The 128 KB RGB565 buffer prefers internal DMA SRAM.

Saves go through `save.tmp` at 20 MHz SPI:

- **XT:** writes `/cybiko/save.nvram` every 5 minutes, when it has changed. A full 2 MB save took 2.3 s on the attached board.
- **V1:** writes the AT45 chip image to `/cybiko-v1/save.flash` 10 s after CyOS's last flash page write.

Battery (V1): CyOS 1.2.46 stops idling on any real on-battery ADC reading (see [PLAN.md](PLAN.md#battery-gauge)). Its radio stops with it. So the guest always sees Java's charger values. The firmware patches out CyOS's low-battery icon, and shows the LiPo level in the T-Deck's own top bar instead.

## Machines

Compile-time switch in `cybiko-lilygo/lib/cybiko_core/src/core/machine.h`. The XT is the default. V1 is `-DCYBIKO_MACHINE_V1` / PlatformIO env `T-Deck-Cybiko-V1`.

| | XT | V1 Classic (main) |
|---|---|---|
| CPU clock | H8S/2323 @ 18.432 MHz | H8S/2241 @ 11.0592 MHz |
| RAM | 2 MB @ 0x400000 | 512 KB @ 0x200000 |
| Storage | memory-mapped flash + CFS in RAM | AT45DB081 (1 MB; the real Classic has the 512 KB AT45DB041) on SCI1 |
| Radio UART | SCI2 + DTC | SCI0, interrupt-driven |
| Card folder | `/cybiko/` | `/cybiko-v1/` |
| Stride on device | 20 | 12 |
| Hardware | boots CyOS 1.5.08 to home (~70 s); CyOS frames over LoRa not tested | boots CyOS 1.2.46 or 1.2.57 at 60 fps; Chat over LoRa with ReBiko Desktop |

The H8S takes no interrupt immediately after LDC/ANDC/ORC/XORC to CCR (`irq_hold`). CyOS's scheduler relies on that. Without it, boots freeze on the logo at some strides.

## Process and licenses

- Guest core, firmware HAL, and the Java reference: MIT, Copyright 2026 Dan Berkowitz. That notice stays in the copies.
- MAME was a hardware reference for the upstream authors. This project does not vendor MAME sources.
- ROMs and `.app` files are not in the tree.

## What this tree runs today

See [README.md](README.md#status) for the latest run. In short:

- Host tests, `two_boot` and `v1_chat.sh` pass.
- The T-Deck runs the Classic at 60 fps.
- Chat MAIN crosses between ReBiko Desktop (on a Heltec) and a T-Deck over LoRa, 6 of 6 lines both ways, and rooms stay apart.
- Not yet tested over the air: two T-Decks, private messages, the people list, and games.
- Range is being measured with the `$l` link view (22 dBm since 24 Oct).
