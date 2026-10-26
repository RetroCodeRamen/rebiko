<p align="center">
  <img src="img/CybikoIcon.png" alt="ReBiko" width="160">
</p>

<h1 align="center">ReBiko</h1>

<p align="center"><em>Original CyOS. New hardware. Back on the air.</em></p>

<p align="center">
  <strong>v1.1026.3b (beta)</strong> · October 2026 ·
  <a href="https://github.com/RetroCodeRamen/rebiko/releases/tag/v1.1026.3b">Linux &amp; Windows builds</a>
</p>

<p align="center">
  <img src="img/pc/shell-blue.png" alt="ReBiko Desktop running CyOS 1.2.46" width="280">
</p>

---

In 2001 you could buy a little blue handheld with a real OS, a tiny keyboard, and a radio that talked to the kid across the cafeteria. No account. No tower. Just CyOS and whoever was in range.

ReBiko does not recreate that vibe in a web app. **It boots the original CyOS.** Hitachi H8S, AT45 flash, the same keyboard matrix, the same chat — on a LilyGO T-Deck in your bag, or as a borderless Classic on your desktop with a Heltec as the radio dongle. LoRa is the transport under the old Cybiko radio, not a new protocol pretending to be one.

Three pieces:

- **ReBiko Desktop** — a Cybiko Classic on your PC (Linux and Windows)
- **ReBiko Pocket** — a LilyGO T-Deck that *is* a Cybiko Classic
- **ReBiko Radio** — a Heltec V3 on USB, the radio for ReBiko Desktop

> Original guest. Modern hardware. Same cafeteria energy, slightly better RSSI.

**1.0 beta.** Testers welcome. The zip now includes Classic CyOS 1.2.46 so you can unzip and run. A git clone still does not — see [GETTING-ROMS.txt](GETTING-ROMS.txt). Xtreme dumps stay off the download.

---

## Get it

**[v1.1026.3b release](https://github.com/RetroCodeRamen/rebiko/releases/tag/v1.1026.3b)** — ReBiko Desktop, CyOS included, no compiler.

| File | What |
| --- | --- |
| `rebiko-desktop-v1.1026.3b-linux-x64.zip` | Linux (`libsdl2`, `libpng`, X11) |
| `rebiko-desktop-v1.1026.3b-windows-x64.zip` | Windows (SDL2 bundled) |

Unzip. `./run.sh` or `run.bat`. Classic boot ROM and CyOS 1.2.46 are already in `roms/`.

Plug in a **Heltec WiFi LoRa 32 V3**. The emulator finds the CP2102 on its own. Right-click the device for Settings — shell color, 4× LCD zoom, always-on-top, reboot/power, and flash buttons for the Heltec radio firmware and a LilyGO T-Deck (those still want [PlatformIO](https://platformio.org/) and this source tree). Full walkthrough below.

### From a git checkout

```bash
# Linux
cybiko-lilygo/tools/v1_emu.sh roms/cyrom112.bin roms/flash_v1246.bin

# ReBiko Pocket (T-Deck)
pio run -t upload --project-dir cybiko-lilygo -e T-Deck-Cybiko-V1

# ReBiko Radio (Heltec)
pio run -t upload --project-dir cybiko-heltec-modem
```

Omit `--radio` and the PC side attaches the first Heltec it sees.

---

## ReBiko Desktop

The PC emulator: a borderless photo of a Cybiko Classic. Inside the LCD, unmodified CyOS 1.2.46 is running. A Heltec V3 on USB is the radio.

<p align="center">
  <img src="img/pc/cyos-loading.png" alt="CyOS 1.2.46 loading" width="240">
  &nbsp;
  <img src="img/pc/shell-blue.png" alt="CyOS Main Desktop" width="240">
  &nbsp;
  <img src="img/pc/cyos-you-and-me.png" alt="You &amp; Me" width="240">
</p>

<p align="center"><sub>Boot splash · Main Desktop · You &amp; Me</sub></p>

### Run it

From the release zip:

```bash
./run.sh          # Linux
run.bat           # Windows
```

From this tree (after you have the ROMs):

```bash
cybiko-lilygo/tools/v1_emu.sh roms/cyrom112.bin roms/flash_v1246.bin \
  --nvram roms/pc-classic.flash \
  --radio /dev/ttyUSB0 --cyid 4C000001
```

`--radio` is optional. If you skip it, the emulator picks the first Heltec CP2102 (`10c4:ea60`). `--nvram` keeps CyOS settings and files across launches.

First boot walks through the CyOS nickname wizard, then lands on the desktop.

### Using the device

| Input | Action |
| --- | --- |
| Type | Letters and numbers go to CyOS |
| Click a key on the photo | Same as pressing that key |
| Arrow keys | D-pad |
| Enter | Opens the highlighted item |
| Esc | Back |
| Drag the plastic | Moves the window |
| Right-click | Application menu |
| Ctrl+Q | Quit (saves NVRAM) |

The window is shaped to the device. Clicks on empty pixels pass through to whatever is behind it.

<p align="center">
  <img src="img/pc/lcd-zoom.png" alt="CyOS Main Desktop close-up" width="400">
</p>

<p align="center"><sub>CyOS 1.2.46 Main Desktop</sub></p>

### Right-click menu

<p align="center">
  <img src="img/pc/menu.png" alt="Right-click menu on ReBiko Desktop" width="280">
</p>

| Item | What it does |
| --- | --- |
| **Settings** | Shell, scale, radio flash, reboot/power |
| **Status** | Heltec, T-Deck USB, CyOS radio counters |
| **Help** | Keyboard map and a short getting-started note |
| **Minimize** | Hides the window |
| **Close** | Quits and writes the NVRAM image |

### Settings

Right-click the Classic → **Settings**.

<p align="center">
  <img src="img/pc/settings.png" alt="ReBiko Settings" width="360">
</p>

| Control | What it does |
| --- | --- |
| **Always on top** | Keeps the Classic above other windows |
| **Mute** | Silences the speaker |
| **Scale − / +** | 50–150%. The window stays proportional to the photo |
| **Shell** | Blue, Neon, or Purple translucent plastic |
| **Body** | The full device (default) |
| **4× zoom** | LCD only, pixel-doubled. Drag to move that window |
| **Reboot** | Cold-boots CyOS. Saved flash stays; RAM does not |
| **Power off / on** | Blanks the LCD and stops the guest; power-on boots again |
| **Flash Heltec** | Uploads ReBiko Radio to the detected CP2102 (needs PlatformIO) |
| **Flash T-Deck** | Uploads ReBiko Pocket to `/dev/ttyACM0` (needs PlatformIO) |

Preferences live in `~/.config/cybiko/v1_emu.conf` (Linux) or `%APPDATA%\cybiko\v1_emu.conf` (Windows).

### Shell colors

The photo is the original blue Classic. Neon and Purple recolor only the dyed plastic — keys, labels, and lighting stay.

<p align="center">
  <img src="img/pc/shells.png" alt="Blue, Neon, and Purple shells" width="720">
</p>

<p align="center"><sub>Blue · Neon · Purple — pick one in Settings → Shell</sub></p>

### Status

Right-click → **Status**. This is the radio health panel, not a CyOS screen.

<p align="center">
  <img src="img/pc/status.png" alt="ReBiko Status" width="360">
</p>

| Block | Meaning |
| --- | --- |
| **Heltec** | USB modem found, ReBiko Radio linked, RSSI/SNR, packet counts |
| **T-Deck** | Whether a T-Deck CDC port is on USB |
| **CyOS radio** | Guest CyID, frames CyOS sent, frames delivered into CyOS |

---

## What you actually get

| Piece | Role |
| --- | --- |
| **ReBiko Desktop** | Borderless photo shell, original CyOS 1.2.46, Heltec as the air interface |
| **ReBiko Pocket** | T-Deck handheld that *is* the Cybiko — keyboard, LCD, trackball, LoRa, battery |
| **ReBiko Radio** | Heltec V3 USB SLIP modem so the PC can share the same 915 MHz channel |

The guest is unmodified CyOS. This is not a Cybiko-style rewrite. Design notes live in [ARCHITECTURE.md](ARCHITECTURE.md); the work order is [PLAN.md](PLAN.md) (Xtreme) and [PLAN-V1.md](PLAN-V1.md) (Classic).

---

## Status

Honest snapshot, 24 Oct 2026.

| Have today | Still cooking |
| --- | --- |
| ReBiko Desktop on Linux and Windows | One-click flash pack with no PlatformIO |
| Heltec auto-detect + ReBiko Radio OLED | CyOS app loader from the PC |
| ReBiko Pocket (T-Deck Classic firmware) | XT battery on Port A |
| PC ↔ T-Deck Chat over LoRa, rooms kept apart | Native Windows click-through window shape |
| T-Deck volume 0–10, settings saved 10 s after a change | Longest range with no lost messages (22 dBm, `$l` link view) |
| | Private messages, the people list and multiplayer games over the air |

Chat over the air works now. The bench test is the PC on a Heltec and a T-Deck in Chat MAIN, 3 lines each way, and it passes 6/6. v1.1026.3b is the first download with it; Chat in v1.1026.2b and earlier doesn't work.

---

## Card layout (ReBiko Pocket)

| Path | Role |
| --- | --- |
| `/cybiko/boot.bin` | XT boot ROM (`cyrom150.bin`, 32 KB) |
| `/cybiko/flash.bin` | CyOS 1.5.08 (`cyos_v1508.bin`, 512 KB) |
| `/cybiko/save.nvram` | Guest RAM image, written back every few minutes |
| `/cybiko/apps/*.app` | Injected into that save before boot if the name is new |
| `/cybiko-v1/boot.bin` | Classic boot ROM (`cyrom112.bin`) |
| `/cybiko-v1/flash.bin` | CyOS 1.2.46 (`flash_v1246.bin`) |
| `/cybiko-v1/save.flash` | AT45DB041 image, written 10 s after CyOS's last change |

Classic dumps ride along in the zip. They stay out of git. XT dumps and `.app` files stay off both.

---

## What's in the tree

| Path | Role |
| --- | --- |
| `cybiko-c-emulator` | H8S core, host tests, radio codec, `two_boot` |
| `cybiko-lilygo` | ReBiko Pocket firmware (XT default, Classic env) and ReBiko Desktop (`tools/v1_emu.c`) |
| `cybiko-heltec-modem` | ReBiko Radio, the Heltec V3 USB LoRa modem |
| `img/pc/` | Screenshots of ReBiko Desktop |

```bash
make -C cybiko-c-emulator test
```

Dan Berkowitz's Java emulator is a local reference for radio and SCI behavior. It is not in this repo.
