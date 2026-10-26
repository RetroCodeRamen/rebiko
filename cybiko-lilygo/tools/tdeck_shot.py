#!/usr/bin/env python3
# Save the T-Deck's Cybiko screen over USB serial (firmware 0x18): PNG at
# 3x with Pillow, otherwise the 160x100 frame as a PGM.
#   tools/tdeck_shot.py out.png|out.pgm [port]     (needs pyserial)
import sys, time, serial
try:
    from PIL import Image
except ImportError:
    Image = None
port = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyACM0"
s = serial.Serial(port, 115200, timeout=0.2)   # as capture_serial.py: no reset
s.reset_input_buffer()
s.write(b"\x18")
buf, end = b"", time.time() + 5
while time.time() < end:
    buf += s.read(65536)
    i = buf.find(b"SHOT ")
    if i >= 0 and len(buf) >= i + 5 + 32000:
        px = bytes.fromhex(buf[i + 5:i + 5 + 32000].decode())
        if Image and not sys.argv[1].endswith(".pgm"):
            Image.frombytes("L", (160, 100), px).resize((480, 300), Image.NEAREST).save(sys.argv[1])
        else:
            with open(sys.argv[1], "wb") as f:
                f.write(b"P5\n160 100\n255\n" + px)
        print("saved", sys.argv[1])
        sys.exit(0)
sys.exit("no screenshot from " + port)
