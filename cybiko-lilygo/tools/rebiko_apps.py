#!/usr/bin/env python3
"""Apps on and off a running Cybiko Classic: ReBiko Desktop or a T-Deck.
The emulator hands each app to CyOS's own console, the way CyberLoad
loaded it, so nothing restarts; CyOS lists new apps on its desktop after
a reboot (the "reboot" command). Files come off by reading the flash.

  tools/rebiko_apps.py [TARGET] ls
  tools/rebiko_apps.py [TARGET] put FILE...      (then: reboot)
  tools/rebiko_apps.py [TARGET] get NAME [OUT]
  tools/rebiko_apps.py [TARGET] rm NAME...
  tools/rebiko_apps.py [TARGET] reboot
  tools/rebiko_apps.py [TARGET] cmd "ver"

TARGET is the Desktop's app link (127.0.0.1:47960, the default) or a
T-Deck's serial port (/dev/ttyACM0, COM5; needs pyserial). The commands
and replies are listed in cybiko-emu/rebiko_apps.h.
"""
import os, socket, sys, time

LINK_START = b"\x19"   # T-Deck: the line that follows is a link command


class Link:
    def __init__(self, target="127.0.0.1:47960"):
        self.buf = b""
        host, _, port = target.rpartition(":")
        if port.isdigit() and not target.startswith(("/", "COM")):
            self.sock = socket.create_connection((host or "127.0.0.1", int(port)), timeout=5)
            self.sock.settimeout(0.05)
            self.ser = None
        else:
            import serial   # the T-Deck; opening without a reset, as tdeck_shot.py
            self.ser = serial.Serial(target, 115200, timeout=0.05)
            self.sock = None

    def _read(self):
        if self.sock:
            try:
                d = self.sock.recv(65536)
                if not d:
                    raise IOError("the emulator closed the link")
                return d
            except socket.timeout:
                return b""
        return self.ser.read(65536)

    def _write(self, b):
        if self.sock:
            self.sock.sendall(b)
        else:
            self.ser.write(b)

    def _fill(self, need=None, secs=30):
        end = time.time() + secs
        while (need is None or len(self.buf) < need) and time.time() < end:
            d = self._read()
            self.buf += d
            if need is None and (b"\n" in self.buf or not d):
                return
        if need is not None and len(self.buf) < need:
            raise IOError("timed out")

    def reply(self, secs=60):
        """The next '@RB ...' line; other lines (T-Deck logs) are skipped."""
        end = time.time() + secs
        while time.time() < end:
            i = self.buf.find(b"\n")
            if i < 0:
                self._fill(secs=0.2)
                continue
            line, self.buf = self.buf[:i], self.buf[i + 1:]
            j = line.find(b"@RB ")
            if j >= 0:
                return line[j + 4:].decode("latin1").rstrip("\r")
        raise IOError("no answer from the emulator")

    def command(self, text, payload=b""):
        self._write(LINK_START + text.encode("latin1") + b"\n" + payload)

    def done(self, on_line=None, secs=60):
        while True:
            r = self.reply(secs)
            if r.startswith("OK"):
                return r
            if r.startswith("ERR"):
                raise IOError(r[4:])
            if on_line:
                on_line(r)

    def ls(self):
        files = []
        self.command("ls")
        r = self.done(lambda l: files.append((l.split()[1], int(l.split()[2])))
                      if l.startswith("FILE ") else None)
        return files, int(r.split()[-1])

    def put(self, name, data, progress=None):
        """The emulator asks for each 1 KB with MORE: the T-Deck's USB
        buffer drops what does not fit."""
        self.command("put %s %d" % (name, len(data)))
        off = 0
        while off < len(data):
            r = self.reply()
            if r.startswith("ERR"):
                raise IOError(r[4:])
            if r == "MORE":
                self._write(data[off:off + 1024])
                off += 1024
        self.done(lambda l: progress and l.startswith("PROGRESS")
                  and progress(*map(int, l.split()[1:3])), secs=120)

    def get(self, name):
        self.command("get " + name)
        r = self.reply()
        if r.startswith("ERR"):
            raise IOError(r[4:])
        n = int(r.split()[1])
        self._fill(n)
        data, self.buf = self.buf[:n], self.buf[n:]
        self.done()
        return data

    def run(self, text):
        out = []
        self.command("cmd " + text)
        self.done(lambda l: out.append(l[4:]) if l.startswith("OUT ") else None)
        return "\n".join(out)


def main():
    args = sys.argv[1:]
    target = "127.0.0.1:47960"
    if args and args[0] not in ("ls", "put", "get", "rm", "reboot", "cmd"):
        target = args.pop(0)
    if not args:
        sys.exit(__doc__)
    op, args = args[0], args[1:]
    link = Link(target)
    if op == "ls":
        files, free = link.ls()
        for name, size in sorted(files, key=lambda f: f[0].lower()):
            print("%8d  %s" % (size, name))
        print("%d files, %d bytes free" % (len(files), free))
    elif op == "put":
        for path in args:
            name = os.path.basename(path)
            data = open(path, "rb").read()
            t0 = time.time()
            link.put(name, data, lambda i, n: print("\r%s: block %d of %d" % (name, i, n),
                                                    end="", flush=True))
            print("\r%s: %d bytes in %.1f s" % (name, len(data), time.time() - t0))
        print("New apps show on the Cybiko's desktop after: rebiko_apps.py reboot")
    elif op == "get":
        data = link.get(args[0])
        out = args[1] if len(args) > 1 else args[0]
        open(out, "wb").write(data)
        print("%s: %d bytes" % (out, len(data)))
    elif op == "rm":
        for name in args:
            print(_rm(link, name))
    elif op == "reboot":
        link.command("reboot")
        link.done()
        print("CyOS is restarting")
    elif op == "cmd":
        print(link.run(" ".join(args)))
    else:
        sys.exit("unknown operation " + op)


def _rm(link, name):
    link.command("rm " + name)
    out = []
    link.done(lambda l: out.append(l[4:]) if l.startswith("OUT ") else None)
    return "\n".join(out)


if __name__ == "__main__":
    main()
