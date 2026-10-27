#!/usr/bin/env python3
"""Update a running Cybiko Classic from CyOS 1.2.46 to 1.2.57, as Cybiko's
AutoUpdate did, through ReBiko's app link (see rebiko_apps.py).

  tools/rebiko_cyos_update.py [TARGET] Cybiko-AutoUpdate.exe

TARGET is ReBiko Desktop's link (127.0.0.1:47960, the default) or a
T-Deck's serial port (/dev/ttyACM0). Cybiko-AutoUpdate.exe is Cybiko's
System Pack updater (kn100.me keeps a copy); 7z unpacks it.

The update sends the "common" and Classic ("114") System Pack files,
replaces desktop.app last (CyOS cannot overwrite the running one), makes
comp1.2.57r.boot the boot image and restarts CyOS. AutoUpdate's last step,
"avr write avr19c6.bin", is skipped: ReBiko emulates the radio chip, and
avr19c6.bin is the firmware it follows. Every file is checked afterwards
and sent again if it did not arrive whole; once a send was lost without
an error. Back up the save first (save.flash on the T-Deck's card,
roms/pc-classic.flash for the Desktop).
"""
import os, shutil, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rebiko_apps import Link, _rm   # noqa: E402

PACKS = ("common\\System Pack\\", "114\\System Pack\\")
LAST = "desktop.app"
TRIES = 3


def unpack(exe, tmp):
    """{name: bytes} for the Classic's System Pack, and the [114] commands"""
    subprocess.run(["7z", "x", "-y", "-o" + tmp, exe], check=True,
                   stdout=subprocess.DEVNULL)
    res = os.path.join(tmp, ".rsrc", "0", "FILES_TO_LOAD")
    # resource 2000 names the files in order: line i is resource 2001 + i
    names = [n for n in open(os.path.join(res, "2000"), "rb").read()
             .decode("latin1").split("\r\n") if n]
    read = lambda i: open(os.path.join(res, str(2001 + i)), "rb").read()
    files = {p.rsplit("\\", 1)[1]: read(i)
             for i, p in enumerate(names) if p.startswith(PACKS)}
    ini = read(names.index("autoupdate.ini"))
    commands, section = [], None
    for line in ini.decode("latin1").splitlines():
        line = line.strip()
        if line.startswith("["):
            section = line
        elif section == "[114]" and line.startswith("command") and "=" in line:
            commands.append(line.split("=", 1)[1])
    return files, commands


def version(link):
    for line in link.run("ver").splitlines():   # "CyOS v. 1.2.46 built ..."
        if "CyOS v" in line:
            return line.strip()
    return "?"


def send(link, name, data):
    for attempt in range(TRIES):
        try:
            link.put(name, data)
            return True
        except IOError as e:
            print("  %s: %s, again" % (name, e))
    return False


def missing(link, files):
    have = dict(link.ls()[0])
    return [n for n, d in files.items() if have.get(n) != len(d)]


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    target = args.pop(0) if len(args) > 1 else "127.0.0.1:47960"
    exe = args[0]
    tmp = tempfile.mkdtemp(prefix="cyos-update-")
    try:
        files, commands = unpack(exe, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    boot = [c for c in commands if c.startswith("boot ")]
    if LAST not in files or not boot:
        sys.exit("%s does not look like the System Pack 57 updater" % exe)

    link = Link(target)
    before = version(link)
    print("Before:", before)
    if "1.2.57" in before:
        sys.exit("CyOS is already 1.2.57")
    have, free = link.ls()
    have = dict(have)
    need = sum(len(d) - have.get(n, 0) for n, d in files.items())
    if need > free:
        sys.exit("%d bytes more are needed and %d are free" % (need, free))

    for name in sorted(f for f in files if f != LAST):
        if not send(link, name, files[name]):
            sys.exit("could not send %s; CyOS is unchanged until the boot step" % name)
        print("  %-22s %6d bytes" % (name, len(files[name])))
    _rm(link, LAST)
    if not send(link, LAST, files[LAST]):
        sys.exit("could not send %s: send it with rebiko_apps.py before restarting" % LAST)
    print("  %-22s %6d bytes" % (LAST, len(files[LAST])))

    for _ in range(TRIES):
        lost = missing(link, files)
        if not lost:
            break
        print("Not on the Cybiko whole, sending again:", " ".join(lost))
        for name in lost:
            if name == LAST:
                _rm(link, name)
            send(link, name, files[name])
    else:
        if missing(link, files):
            sys.exit("still missing: %s; not switching the boot image"
                     % " ".join(missing(link, files)))

    for c in boot:
        print(">>", c)
        print(link.run(c))
    print("Restarting CyOS")
    link.command("reboot")
    link.done()
    end = time.time() + 180
    while time.time() < end:
        time.sleep(5)
        try:
            after = version(link)
            break
        except IOError:
            continue
    else:
        sys.exit("CyOS did not answer after the restart")
    print("After: ", after)
    if "1.2.57" not in after:
        sys.exit("the update did not take")


if __name__ == "__main__":
    main()
