#!/bin/sh
# ReBiko Desktop's Linux release: a .deb for Debian, Ubuntu and their family
# (KDE neon, Mint, Pop!_OS), which installs to /opt/rebiko with a "ReBiko
# Desktop" entry in the launcher and a `rebiko` command, and the unzip-and-
# run zip for everything else.
#   tools/make_deb.sh   -> .pio/rebiko_<VERSION>_amd64.deb
#                          .pio/rebiko-desktop-v<VERSION>-linux-x64.zip
#   sudo apt install ./.pio/rebiko_*.deb
#
# Each user's Cybiko lives in ~/.local/share/rebiko: the first launch makes
# a fresh one there from the Classic ROMs in the package, and its save, the
# Apps/ library and any Xtreme ROMs stay there across upgrades. Like the
# release zip, the package carries only the Classic's ROMs (ROMS=dir to take
# them from elsewhere; default ../roms). Building needs gcc, SDL2, libpng,
# X11 development files and dpkg-deb; Pillow for the icon sizes.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
TOP=$(cd "$HERE/.." && pwd)
VERSION=$(tr -d ' \n' < "$TOP/VERSION")
ROMS=${ROMS:-$TOP/roms}
STAGE=$HERE/.pio/deb/rebiko
OPT=$STAGE/opt/rebiko
DEB=$HERE/.pio/rebiko_${VERSION}_amd64.deb

for f in cyrom112.bin flash_v1246.bin; do
    [ -f "$ROMS/$f" ] || { echo "missing $ROMS/$f" >&2; exit 1; }
done
rm -rf "$STAGE"
mkdir -p "$OPT/roms" "$OPT/img" "$OPT/tools" "$STAGE/usr/bin" \
         "$STAGE/usr/share/applications" "$STAGE/usr/share/pixmaps" \
         "$STAGE/lib/udev/rules.d" "$STAGE/DEBIAN"

# Both models side by side, as the machine switch expects
OUT="$OPT/v1_emu" "$HERE/tools/v1_emu.sh" --version
strip "$OPT/v1_emu" "$OPT/xt_emu"
cp "$ROMS/cyrom112.bin" "$ROMS/flash_v1246.bin" "$OPT/roms/"
cp "$TOP/img/cybiko.png" "$TOP/img/xtremeShell.png" "$TOP/img/CybikoIcon.png" "$OPT/img/"
cp "$HERE/tools/rebiko_apps.py" "$HERE/tools/rebiko_cyos_update.py" "$OPT/tools/"

# Icons for the launcher: hicolor sizes, and pixmaps as a fallback
cp "$TOP/img/CybikoIcon.png" "$STAGE/usr/share/pixmaps/rebiko.png"
python3 - "$TOP/img/CybikoIcon.png" "$STAGE/usr/share/icons/hicolor" <<'EOF' || true
import os, sys
from PIL import Image
src = Image.open(sys.argv[1]).convert("RGBA")
for n in (32, 48, 64, 128, 256, 512):
    d = os.path.join(sys.argv[2], "%dx%d" % (n, n), "apps")
    os.makedirs(d, exist_ok=True)
    src.resize((n, n), Image.LANCZOS).save(os.path.join(d, "rebiko.png"))
EOF

cat > "$STAGE/usr/bin/rebiko" <<'EOF'
#!/bin/sh
# ReBiko Desktop. This user's Cybiko lives in ~/.local/share/rebiko: its
# save, its ROMs, the Apps/ library, and the Xtreme's ROMs (cyrom150.bin,
# cyos_v1508.bin in roms/) if you add them. The first launch makes a fresh
# Cybiko from the ROMs in /opt/rebiko/roms.
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/rebiko"
if [ ! -f "$DATA/roms/cyrom112.bin" ]; then
    mkdir -p "$DATA/roms" "$DATA/Apps"
    cp /opt/rebiko/roms/* "$DATA/roms/"
fi
exec /opt/rebiko/v1_emu "$DATA/roms/cyrom112.bin" "$DATA/roms/flash_v1246.bin" \
    --nvram "$DATA/roms/pc-classic.flash" "$@"
EOF
cat > "$STAGE/usr/bin/rebiko-apps" <<'EOF'
#!/bin/sh
exec python3 /opt/rebiko/tools/rebiko_apps.py "$@"
EOF
chmod 755 "$STAGE/usr/bin/rebiko" "$STAGE/usr/bin/rebiko-apps"

cat > "$STAGE/usr/share/applications/rebiko.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=ReBiko Desktop
GenericName=Cybiko emulator
Comment=The original CyOS on your PC, with a Heltec as its radio
Exec=rebiko
Icon=rebiko
Terminal=false
Categories=Game;Emulator;
Keywords=Cybiko;CyOS;ReBiko;
EOF

# The Heltec (CP2102) and a T-Deck (ESP32-S3 USB) open without the dialout
# group for whoever is at the desktop
cat > "$STAGE/lib/udev/rules.d/70-rebiko.rules" <<'EOF'
SUBSYSTEM=="tty", ATTRS{idVendor}=="10c4", ATTRS{idProduct}=="ea60", TAG+="uaccess"
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", TAG+="uaccess"
EOF

cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
    udevadm trigger --subsystem-match=tty || true
fi
EOF
chmod 755 "$STAGE/DEBIAN/postinst"

MAINT="$(git -C "$TOP" config user.name) <$(git -C "$TOP" config user.email)>"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: rebiko
Version: $VERSION
Architecture: amd64
Maintainer: $MAINT
Installed-Size: $(du -sk "$STAGE/opt" "$STAGE/usr" | awk '{s += $1} END {print s}')
Depends: libsdl2-2.0-0, libpng16-16t64 | libpng16-16, libx11-6, libxext6, python3, python3-serial
Recommends: p7zip-full
Section: games
Priority: optional
Homepage: https://github.com/RetroCodeRamen/rebiko
Description: ReBiko Desktop, a Cybiko on your PC
 The original CyOS on an emulated Cybiko Classic or Xtreme, in a photo of
 the handheld. A Heltec V3 running ReBiko Radio puts it on the air with
 ReBiko Pocket (T-Deck) handhelds. Apps go on and off it from its Apps
 window or rebiko-apps.
EOF

dpkg-deb --root-owner-group --build "$STAGE" "$DEB" >/dev/null
echo "$DEB"

# The zip: unzip anywhere and ./run.sh; the save goes in its roms/
NAME=rebiko-desktop-v$VERSION-linux-x64
Z=$HERE/.pio/zip/$NAME
rm -rf "$HERE/.pio/zip"
mkdir -p "$Z/roms" "$Z/img"
cp "$OPT/v1_emu" "$Z/"
cp "$TOP/img/cybiko.png" "$TOP/img/CybikoIcon.png" "$Z/img/"
cp "$ROMS/cyrom112.bin" "$Z/roms/boot.bin"
cp "$ROMS/flash_v1246.bin" "$Z/roms/flash.bin"
cat > "$Z/run.sh" <<'EOF'
#!/bin/sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BOOT=${1:-"$HERE/roms/boot.bin"}
FLASH=${2:-"$HERE/roms/flash.bin"}
if [ ! -f "$BOOT" ] || [ ! -f "$FLASH" ]; then
  echo "Missing Classic dumps:"
  echo "  $HERE/roms/boot.bin"
  echo "  $HERE/roms/flash.bin"
  exit 1
fi
exec "$HERE/v1_emu" "$BOOT" "$FLASH" --nvram "$HERE/roms/save.flash" --shell "$HERE/img/cybiko.png"
EOF
chmod 755 "$Z/run.sh"
cat > "$Z/roms/README.txt" <<EOF
Classic dumps shipped with ReBiko Desktop v$VERSION.

  boot.bin   Cybiko Classic boot ROM, 32 KB (cyrom112.bin)
  flash.bin  CyOS 1.2.46 (flash_v1246.bin)

save.flash is written here the first time you run. Delete it to
reset the machine.

Xtreme dumps are not in this zip.
EOF
cat > "$Z/README.txt" <<EOF
ReBiko Desktop  v$VERSION (1.0 beta)
Original CyOS. New hardware. Back on the air.

A Cybiko Classic on your PC, running the original CyOS 1.2.46.
Unzip and run. The Classic boot ROM and CyOS are already in roms/.

  roms/boot.bin    32 KB Classic boot ROM
  roms/flash.bin   CyOS 1.2.46

Needs: libsdl2-2.0-0, libpng16, libx11, libxext (Ubuntu/Debian:
sudo apt install libsdl2-2.0-0 libpng16-16 libx11-6 libxext6)
  ./run.sh
On Debian, Ubuntu and KDE neon, the .deb from the same release installs
ReBiko Desktop into the launcher instead.

Radio: plug in a Heltec WiFi LoRa 32 V3 running ReBiko Radio. It is
picked up automatically (Silicon Labs CP2102, 10c4:ea60). CyOS Chat
then works over LoRa with ReBiko Pocket (a LilyGO T-Deck) and other
ReBiko Desktops of this version: the radio link changed in v1.1027.1b,
so T-Decks need firmware from the same source tree.

Right-click the Cybiko for Settings, Status, Apps and Help.
  https://github.com/RetroCodeRamen/rebiko

Xtreme dumps are not included.
EOF
(cd "$HERE/.pio/zip" && rm -f "$HERE/.pio/$NAME.zip" && zip -qr "$HERE/.pio/$NAME.zip" "$NAME")
echo "$HERE/.pio/$NAME.zip"
