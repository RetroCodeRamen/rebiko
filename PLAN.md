# ReBiko plan

**Where it stands, 27 Oct 2026.** Release [v1.1026.4b](https://github.com/RetroCodeRamen/rebiko/releases/tag/v1.1026.4b); main has moved on since (below).

The Cybiko Classic is the product. It ships with CyOS 1.2.46 and also runs 1.2.57, the last Classic System Pack, which full Cylandia needs. ReBiko Desktop and one ReBiko Pocket chat over LoRa on either. Apps go on and off a running Classic over USB or from the Desktop. The Xtreme is on the same T-Deck again as of 26 Oct 2026: both firmwares can be flashed, and the menu switches between them. Classic ↔ Xtreme communications are in progress and not done. The internet relay is designed and not started.

This file is the only plan. Design notes stay in [ARCHITECTURE.md](ARCHITECTURE.md). Measured numbers stay in [README.md](README.md) and [cybiko-lilygo/README.md](cybiko-lilygo/README.md).

Classic V2 stays out of scope. Its RF init never finishes in the Java emulator or in MAME, and there is no working reference to port.

## Done

### Classic guest

- Compile-time machine switch in `machine.h` (`CYBIKO_MACHINE_V1`). The XT build kept its RAM and LCD hashes.
- V1 memory map and peripherals: 512 KB RAM, LCD at `0x600000`, no DMA, 3 Timer16 channels, ICR, RTC SDA through Port F DDR, 9-column keyboard with IRQ2.
- AT45 flash on SCI1, with the DTC bulk read. Host boot matches Java through the logo to the idle desktop (`PC=0x219C8E` at stride 1, frame 540).
- The flash chip is the 1 MB AT45DB081, twice the real Classic's AT45DB041: CyOS detects it ("model 4") and has about 560 KB free instead of 43 KB. A 512 KB image or save loads into the first half; the rest is formatted as empty blocks.
- **CyOS 1.2.57**, 26 Oct 2026. `tools/rebiko_cyos_update.py` installs it from Cybiko's own `Cybiko-AutoUpdate.exe` over the app link, checks every file and resends what did not arrive. 1.2.57 polls the keyboard and leaves the IRQ2 hook on the ROM stub, so IRQ2 is raised only once a handler in RAM is installed. Its radio moves frames by DTC, as the Xtreme does; `sci0_v1.c` follows whichever the running CyOS uses and finds each version's radio object and CyID cache by its version string.
- CyOS runs its own radio. TMR1's overflow interrupt is level-sensitive, so the old poll/scan driver is gone. `sci0_v1.c` is the UART, idle RX, and the CyID.
- The scheduler bug is fixed (`irq_hold`): no interrupt in the instruction after a CCR write. Strides 8–16 boot and take keys. The V1 firmware ships stride 12. See [Do not regress](#do-not-regress).

### ReBiko Desktop

A borderless Classic on Linux and Windows. CyOS 1.2.46 is in the release zip. An **Apps** window lists the `Apps/` library beside the Cybiko's files: Send, Restart CyOS, Delete, Copy to PC. `tools/rebiko_apps.py` does the same from a shell, over TCP 47960. A Heltec V3 is found on its own (CP2102 `10c4:ea60`). Right-click opens Settings: shell color, LCD zoom, always-on-top, reboot, power, and flash buttons that still need PlatformIO.

- Saves go to `save.flash` and survive a restart.
- Frames run on a 1/60 s clock. The audio queue is capped at 100 ms, and nothing is queued while muted or powered off.

### ReBiko Pocket

`pio run -e T-Deck-Cybiko-V1`. Card folder `/cybiko-v1/`. Boots CyOS 1.2.46 from the card and holds 60 fps.

- The whole AT45 image is `/cybiko-v1/save.flash`, written 10 s after CyOS's last change. A failed save retries 10 s later. Mute in CyOS Settings survived a reset on hardware, 24 Oct 2026. Deleting the file resets the machine.
- Keys are raw, including Mic and both Shifts. Letters and digits type as themselves. `$` starts a combination (Esc, F-keys, symbols, volume). Backspace is the Cybiko's BkSp. A repeated key is held up long enough that CyOS 1.2.46 sees it.
- **Menu (`$` Mic), shipped 26 Oct 2026.** CyOS pauses. The menu remaps every key and combination, and sets CPU mode (Dynamic, Always high, Always low), brightness, scale, and volume. Settings are one versioned, CRC'd blob in NVS, written when the menu closes or volume mode ends, and only if something changed. The version string comes from the repo `VERSION` file.
- **Battery and sleep.** The backlight steps from 4/4 to 1/4 (default 3/4). After 5 idle minutes CyOS blanks its LCD; the backlight goes off, drawing stops, and Dynamic mode drops the CPU to 160 MHz. A key wakes CyOS and the screen follows. The radio keeps running. The awake desktop stays at 240 MHz.
- The T-Deck's top bar shows the LiPo level. CyOS's own gauge stays off. See [Battery gauge](#battery-gauge).
- **Apps over USB.** `tools/rebiko_apps.py /dev/ttyACM0 ls|put|get|rm|reboot|cmd`, through CyOS's own console as CyberLoad did; files come off by reading the flash image.
- The screen sends only changed rows; the panel is released only after each DMA (releasing early left stale bands), and a rolling refresh resends what the panel should show. Saves rewrite only the flash pages CyOS changed (27 Oct 2026: a 1 MB save used to stop everything for 1.9 s).
- **The clock keeps time** (27 Oct 2026). CyOS resets its clock chip at every cold start; ReBiko puts the time back. The T-Deck's time survives restarts, resumes after a power-off from the last time saved, and is set by CyOS's Settings or by ReBiko Desktop over LoRa. Fixing it found a CPU bug: `MULXU.B`/`MULXS.B` multiplied by the wrong byte, which broke every BCD conversion in CyOS and apps.

### Radio

One profile on every ReBiko Pocket and ReBiko Radio: **915 MHz, SF7, 500 kHz, 22 dBm, listen-before-talk.** Power has been 22 dBm since 24 Oct 2026 (it was 14, and 0 before that). A unit on the old power still hears this one. Reflash both ends to get the range.

- Chat rooms are radio channels. The co-processor drops a frame whose channel is not the one CyOS last tuned. MAIN and Computers stay apart.
- Host: `tools/v1_chat.sh` puts two Classics in Chat MAIN and each sees the other's line. `tools/v1_three.sh` does three, all in range or (`CHAIN=1`) A–B–C with A and C out of each other's range.
- **The co-processor follows the Classic's AVR firmware**, 27 Oct 2026. `avr19c6.bin` ships inside CyOS 1.2.57's `System.a`; disassembled, it is a byte pipe with a channel scanner. Frames keep CyOS's 8-byte address header (destination, sender) on the air (link version 2); the Classic gets no reply to `01` commands, `03` after a frame, and a frame start only when one arrived.
- **Cybiko never relayed.** Neither CyOS (1.2.46 or 1.2.57, the A–B–C test) nor the AVR firmware forwards a frame. Cybiko's 2002 patent application describes mesh routing; the shipped Classic does not do it. Range past one hop has to come from ReBiko (the [relay](#internet-relay)).
- Air, 27 Oct 2026: Desktop and T-Deck both on CyOS 1.2.57 and link version 2, a Chat line each way.
- Air, 23–24 Oct 2026: Desktop on a Heltec and one T-Deck, 3 lines each way, 6 of 6, including both sides sending within 0.1 s.
- `$l` on the T-Deck is a live link readout. `modem_tool --ping` reports loss and SNR both ways. The link layer counts lost messages from gaps in each sender's ids.

### Xtreme foundation

Built before the Classic became the product, and still shared with it:

- The C core plus the T-Deck HAL. Reticulum is not linked in. The transport vtable is the seam for a later path.
- Pinout for the original T-Deck, not the Pro.
- Link codec, fragmentation, duplicate suppression, and `cybiko_link_self_check` in the firmware.
- SCI2 DTC on the XT address bus. `two_boot` chats between two host Xtremes. That is not a radio test.
- On the T-Deck, 22 Oct 2026: Loading holds 60 fps, and the home screen is up about 70 s after power-on. Stride stays 20.
- SX1262 driver (`sx1262_phy.cpp`) and the Heltec SLIP modem. The same driver carries Classic Chat. A CyOS frame from an Xtreme, over the air, has not been shown.

## In progress

Started 26 Oct 2026. None of this is accepted yet.

- **Classic ↔ Xtreme communications.** The Java emulator can chat V1↔XT. ReBiko has not shown that. Done when a Classic and an Xtreme each display the other's line, first on the host, then over LoRa.
- **Both models in one Pocket.** `tools/flash_both.sh` puts the Classic in app0 and the Xtreme in app1. System → Machine saves and restarts into the other. Each model has its own card folder, so they do not share ROMs or saves. The menu says the other model is not installed until both images have been flashed.
- **LLM chat buddy on the Desktop (28 Oct 2026).** Right-click → **LLM**: an Ollama server address, Connect (lists its models), a model, a Chat room, Start. A second, hidden Classic named Tyler (a goofy friend from 2000: N64, Pokemon, Tony Hawk) boots from `bot.flash` beside the Desktop's save; the first time, that is a copy of the Desktop's flash with the nickname changed in `res-finder.dat` (offset 0xB0; `rb_cfs_patch` re-seals the page's CRCs). Once its CyOS console says `Ready >` it starts `chat_visual.app` from the console, steps down to the room (1.2.46 and 1.2.57 list the rooms in different orders), and checks its radio landed on the room's channel. It reads Chat lines off the air (type 0x28, text after 0x25 and a length at frame offset 29), asks the model on a thread (`/api/chat`, short history, one line of at most 60 characters), and types the answer into CyOS's Chat. Typing timings were measured: keys need 12 frames all-up between them and Shift 10 frames either side of the letter, or letters drop and Shift sticks. While Tyler runs, the LLM window stays open and the main window shows his Cybiko (its keyboard types as him); **Show mine** switches back. The Desktop's Cybiko, Tyler's and the Heltec share one in-app air (`rb_air_t`), so Tyler is heard with or without a Heltec. Code: `cybiko-lilygo/tools/rebiko_bot.c`, the LLM window in `v1_emu.c`; prefs `llm_server`, `llm_model`, `llm_room`, `llm_on` (it starts again at launch). Tested 28 Oct: the T-Deck asked in MAIN over LoRa and got Tyler's answers (mistral:7b, ~8 s warm). Not done: the Windows build is untested, and Tyler's CyID is the Desktop's XOR 0x00B07000. The T-Deck's own typing showed the same Shift problem ("JApan" from Shift+j, a): its 3-frame gap is shorter than the bot's measured 12.
- **Shared speed work.** Boot-ROM drawing loops run natively on both models, guarded by the code bytes at those addresses. `tests/hle_verify.sh` checks every native call against the CPU (registers, PC, flags, RAM). The LCD rebuilds only when the picture changes. On the idle Classic desktop, T-Deck busy time went from 2255 ms to 1531 ms per 5 s. The Xtreme's `host_bench` hashes must stay put. The Classic's hashes change with each new native loop, because the guest runs fewer instructions.

## Open

Each item names what done means. Host tests are not a substitute for the air checks.

1. **Range.** The most distance with no lost messages. Power is already 22 dBm. Next is spreading factor, with bandwidth staying at 500 kHz so the channel stays legal under FCC 15.247. SF8 is about +2.5 dB and SF9 about +5 dB. A Chat frame is about 82 ms on the air at SF7, 146 ms at SF8, and 260 ms at SF9. Antennas are the cheap gain after that. Done when a measured link at the new profile loses nothing that SF7 lost, and every flashed unit is on that profile.

2. **More of CyOS over the air.** Desktop ↔ one T-Deck in one room is done. Still open, in this order:
   - private messages, including CyOS's retries (addressed frames now keep their destination, so this is the first real test of that);
   - the people list;
   - two T-Decks, same room, a typed line each way (needs a second T-Deck);
   - multiplayer games: airtime, latency, and frames over 255 bytes.
   - Classic ↔ Xtreme chat is [in progress](#in-progress), not this list. It stays open until both directions are shown.

3. **Apps, without rebuilding firmware.** The Classic is done: live put/get/rm over the app link, and the Desktop's Apps window picks from `Apps/`. Still open:
   - **Xtreme:** the host writer and the card scan exist (`/cybiko/apps/*.app` into `save.nvram`). CyOS does not list an injected app yet, and the Xtreme loaded apps over its USB chip, not the console.
   - A picker for the T-Deck from the PC (today it is `rebiko_apps.py` by name).

4. **Group encryption (decided 28 Oct 2026, not started).** Keeps casual listeners out; not military grade. Open by default: with no key set, packets go out exactly as today. With a group passphrase set (ReBiko menu on the T-Deck, Settings on the Desktop; the Heltec is unchanged because the PC encrypts before the modem), the link layer (`radio_link.c`) encrypts and authenticates the whole packet with ChaCha20-Poly1305 from Monocypher (one public-domain C file, builds everywhere), key from the passphrase through Argon2. Only a 1-byte key id and an 8-byte nonce (per-boot random + counter) stay in the clear; sender, message id, channel and fragment fields go inside, so a wrong-key packet is dropped at once and a forged or tampered one fails the tag. The per-sender counter rejects replays. About 25 bytes more per packet (a 200-byte Chat frame becomes ~241 of 255; the tag can be cut to 8 bytes if airtime matters). Link version 3: units update together. Switching groups is switching keys; Chat rooms still work inside a group, and the internet relay can forward packets it cannot read. Note in the docs: fine on 915 MHz under Part 15, not allowed on amateur bands. Done when the Desktop and a T-Deck chat on one key, a third unit on another key or none hears nothing, and no key behaves as today.

5. **Radio settings on the Pocket.** The `$` Mic menu covers keys, CPU, display, and audio. It does not change frequency, spreading factor, bandwidth, power, or CyID, and it does not pick a ROM. Done when a change hits the SX1262 registers and is still there after reboot.

6. **Desktop leftovers.**
   - A flash pack that does not need PlatformIO installed.
   - Click-through on the shaped window under native Windows. Linux already lets clicks on empty pixels pass through.

7. **Battery passthrough.** Blocked. The details are under [Battery gauge](#battery-gauge). Done when the T-Deck's LiPo drives CyOS's own gauge, CyOS still idles, and the radio still sends.

8. **Xtreme, besides the switch and the cross-chat.** CyOS 1.5.08 is still not the product. The menu switch does not make these done:
   - A CyOS frame from an Xtreme T-Deck received on the PC, then discovery and chat both ways. A raw ping is not this.
   - Two Xtremes chatting over LoRa.
   - The XT battery reading on Port A. The Classic gauge workaround does not cover it.

9. **Internet relay.** On hold. The design is below. Do not start it ahead of two T-Decks in Chat.

## Internet relay

Written from host tests on 24 Oct 2026. Nothing here is built. The aim is Cybikos past radio range: a server joins them into **zones** of up to 30. "Zone" is not "channel." CyOS already uses radio channels for Chat rooms.

Three clients: PC emulators talk to the server directly, T-Decks can talk to it over WiFi, and a Heltec gateway relays everyone in its radio range.

### What the tests established

Headless Classics in one process, stock ROMs, a simulated radio, scripted keys. `v1_many` and `v1_rooms` are still only in the local scratchpad.

| Classics | People on the desktop | Once settled |
|---|---|---|
| 4 | 3 | 60–85% idle |
| 28 | 27 | 50–80% idle |
| 32 | 30 | 30–70% idle |
| 36 | counter gone, keys dropped | flat out for over a minute |
| 60 | counter gone | 0% idle, never recovers |

The limit is the processor. The people list held 77 names and was still filling. Joining a room is the expensive part, because every Classic handles every other Classic's traffic. The manual's "100 units on each of 30 channels" does not hold, and a T-Deck has the same limit. The tests were harsh: every unit heard every channel, and they all joined within seconds. **Zone cap: 30.**

On one SF7 / 500 kHz channel a presence packet is about 28 ms and a Chat packet about 82 ms. Each Classic beacons 14–20 times a minute. At 20 people in Chat the channel was 43% busy (about 8 s average wait). At 40 it was 74% busy (about 49 s). One gateway can carry about 15–20 people actually chatting. The server must not forward everything.

Rooms in 1.2.46 are fixed: Desktop is channel 2, MAIN 4, Romantic 6, Computers 8, Sports 10, Games 52, Music 54. There are no custom rooms. While in a room, a Classic still listens on channel 2 for a few frames about every 5 s. Chat text is type `0x28`, sent three times, in the clear after `25 <len>`.

Channel filtering now ships in `radio_coproc.c` (see [Radio](#radio)). The 30-person cap has not been remeasured with that filter on.

### Shape

```
Cybikos ~ LoRa ~ Heltec gateway ──┐
T-Deck (WiFi) ────────────────────┼── WebSocket ── Server ── control bot
PC emulator ──────────────────────┘
```

Each binary WebSocket message is one link frame, the same bytes as on LoRa. Text beside that: `hello`, `zone`, `stats`. The server is a hub. It keeps members, which gateway they are behind, and a duplicate filter. It does not interpret Cybiko traffic except presence (to count members) and Chat text in the control room.

A direct PC uses a network phy next to `radio_serial_phy.c`, so fragmentation and the duplicate filter stay. A T-Deck would use the same phy over WiFi, chosen in the menu. The Heltec already has the SX1262 path and the SLIP bridge. It would add WiFi and a setup page. Uplink is everything it hears. Downlink is what the server sends, inside an airtime budget.

Computers (channel 8) is the control room, because 1.2.46 has no custom rooms. Any of the five non-MAIN rooms would do. A headless Classic run by the server sits there and speaks for it, so the server never has to build Chat packets. Commands typed in that room: `join`, `list`, `who`, `help`. Control-room traffic is not relayed between zones.

New direct clients start in a lobby that contains only the bot. A full zone refuses the join. Everyone in a gateway's radio range shares the zone set on that gateway. `join` from a Cybiko behind a gateway is refused. Members count by CyID from presence, including Cybikos behind a gateway.

Relay rules, per zone:

1. Drop repeats. CyOS sends each Chat line three times; the server forwards one and lets the gateway decide how many to transmit.
2. Don't echo a frame back to where it came from.
3. Forward each remote member's beacons at most once every 20–30 s.
4. Queue room-join bursts so a zone does not take several joins at once.
5. A gateway transmits at most about 30% of the time. Past that, drop presence first, then extra Chat copies.
6. Keep the radio channel, so MAIN only reaches people in MAIN.

Phases, each with a test before the next:

| Phase | Build | Done when |
|---|---|---|
| 0. Channels | Done in the co-processor. The many-peer tools are not in the tree. | `v1_many` rerun at 24, 30, and 36 with filtering on. |
| 1. Server | A small server and a network phy for Desktop and the headless tools. | Two PCs chat in MAIN through the server. Three air copies become one relayed copy. |
| 2. Zones | Cap 30, the lobby, the bot. | `join`, `list`, and `who` work. A 31st Classic is refused. MAIN stays inside its zone. |
| 3. Gateway | Heltec firmware with the six rules. | A T-Deck by the gateway chats with a PC elsewhere. Over 10 minutes, airtime stays under budget with 20 remote members. |
| 4. T-Deck direct | WiFi, and a choice of LoRa or WiFi. | A T-Deck on WiFi chats with a T-Deck behind a gateway. |
| 5. Load | `v1_many` pointed at the server. | 30 Classics in one zone, across 2 gateways and direct clients, every CyOS still above 20% idle. |

Still unknown, and worth answering before phase 2: where room counts come from (channel 2, or something inside presence); whether private messages survive the relay; whether games survive 50–300 ms extra delay; who issues CyIDs; who hosts the server.

## Battery gauge

CyOS 1.2.46 will not idle on a real on-battery ADC reading, and its radio stops with it. This is in the Java emulator too, with the same values, so it is not a C-port bug.

The battery task (`0x21DED6`) smooths ADC channels 1 and 2. If ch1 − ch2 > 15, CyOS decides a charger is present. Otherwise it is on battery, and from the desktop onward a callback posts message `0x20` forever. The screen stops updating. Levels 36–39 are the exception: CyOS idles and shows the blinking low icon. Java's defaults (ch1 `0x300`, ch2 `0x100`) mean "on the charger."

What ships: the guest always gets those charger values. The firmware patches the two `CMP.W #42` checks (`0x25B760`, `0x25B7CE`) so CyOS draws no battery icon. The T-Deck's own bar shows the LiPo. `cybiko_set_adc()` is the hook for a real passthrough.

If the idle bug is ever fixed: charger values while USB is plugged in. On battery, `ch1 = ch2 = 341 + level`, with the LiPo voltage mapped onto 31–78.

## Do not regress

**Scheduler.** CyOS (`0x204698`) clears the interrupt mask one instruction before it loads the new task's stack pointer. The H8S accepts no interrupt right after `LDC`, `ANDC`, `ORC`, or `XORC` to CCR. Taking one there makes the tick handler check the old stack against the new task, leave "timers in progress" set, and never run a CyOS timer again. The boot stops on the logo, or the desktop freezes. `cpu->irq_hold` is that rule. The XT host-bench hashes changed when it went in. Those hashes are the baseline.

**Radio driver.** Do not put the Java poll/scan layer back. It existed because TMR1's overflow interrupt never fired. CyOS calls its own tick. The only CyOS addresses the radio path touches are each version's radio object and CyID cache, in `V1_LAYOUTS` in `sci0_v1.c` (1.2.46: CyID `0x21F9DC`; 1.2.57: `0x222CF4`). Do not strip the 8-byte address header again: CyOS on each receiver decides by it whether a frame is its own.

**Headless 1.2.57.** A Classic with nothing draining its serial console stops booting after "OS version"; host tools attach a sink (`v1_three.c`). Chat in 1.2.57 opens the recipient list on entering a room, so scripts press Enter once more before typing.

**What this project is not.** A new chat application, a mock desktop, or a redrawn Cybiko UI. A frame rate is not guest speed: at stride 20, 60 fps is about 0.9 guest MIPS, well short of a real Xtreme. A local radio `03` is not proof the other radio heard the packet.
