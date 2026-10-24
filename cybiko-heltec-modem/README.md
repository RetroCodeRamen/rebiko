# ReBiko Radio

Turns a Heltec WiFi LoRa 32 V3 into the radio for ReBiko Desktop:

    ReBiko Desktop  →  USB (CP2102, SLIP at 921600)  →  Heltec SX1262  →  LoRa  →  ReBiko Pocket (T-Deck)

The SX1262 driver is `../cybiko-c-emulator/src/radio/sx1262_phy.cpp`, the same one the T-Deck uses. The radio profile is in `../cybiko-c-emulator/src/radio/radio_transport.h`, so both ends share it: 915 MHz, SF7, 500 kHz, CR 4/5, 14 dBm, sync word 0x1424, listen-before-talk. Change it and flash both. The serial protocol is in `../cybiko-c-emulator/src/radio/modem_proto.h`.

The modem does not decide what to send. Every packet comes from the PC.

The OLED shows:

- `EMU ON` or `NO EMU`, depending on whether ReBiko Desktop is linked over USB.
- The frequency and SF.
- RX/TX counts.
- The last packet's RSSI and SNR.

A small box means linked. A larger one flashes on activity. If the chip did not start, the OLED shows `FAIL` and "radio down".

## Flash

    pio run -t upload        # /dev/ttyUSB0

ReBiko Desktop can also do this: right-click → Settings → Flash Heltec (needs PlatformIO).

## Run ReBiko Desktop on it

    ../cybiko-lilygo/tools/v1_emu.sh ../roms/cyrom112.bin ../roms/flash_v1246.bin --nvram ../roms/pc-classic.flash

It finds the CP2102 (`10c4:ea60`) on its own; `--radio /dev/ttyUSB0` picks one. `--cyid` sets the PC's CyID (hex, default `4C000001`). The T-Deck takes its CyID from its MAC, so the two always differ. Only one emulator can hold the modem at a time.

## Check it without CyOS

    cd ../cybiko-c-emulator && make build/modem_tool
    ./build/modem_tool /dev/ttyUSB0 --seconds 20             # listen, print status
    ./build/modem_tool /dev/ttyUSB0 --seconds 20 --ping 1000 # also send PING every second

A T-Deck running ReBiko Pocket counts the pings on its serial log (`radio rx N ... msgs N`).

The Xtreme SDL frontend in `../cybiko-c-emulator` (`make build/cybiko-emu`, `--radio /dev/ttyUSB0`) can use the modem too.
