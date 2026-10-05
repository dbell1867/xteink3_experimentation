#!/usr/bin/env python3
"""Sync lesson pages to the Xteink X3 over the USB cable (firmware: src/sync_server.*).

  1. re-render each docs/lesson-*.md whose .xtc is missing or older than the
     Markdown (or than tools/mdpages.py itself)
  2. wait for the X3's serial port (press Power if it's asleep)
  3. compare with the X3's /books (size + CRC32) and send only what differs
  4. --delete: remove .xtc files on the X3 that no longer exist here
     (never touches .txt books or anything else)

Usage: x3sync.py [--md 'docs/lesson-*.md'] [--out build/xtc] [--port /dev/ttyACM0] [--delete] [--no-build]
Defaults are relative to the project (not the current folder), so it runs from anywhere.
Opening the port does NOT reset the X3 (DTR/RTS are left alone).
"""
import argparse
import glob
import os
import subprocess
import sys
import time
import zlib

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # the project: defaults don't depend on where you run the command
CHUNK = 4096


def build(md_glob, out):
    os.makedirs(out, exist_ok=True)
    tool = os.path.join(HERE, "mdpages.py")
    built = 0
    for md in sorted(glob.glob(md_glob)):
        name = os.path.splitext(os.path.basename(md))[0]
        xtc = os.path.join(out, name + ".xtc")
        newest_src = max(os.path.getmtime(md), os.path.getmtime(tool))
        if os.path.exists(xtc) and os.path.getmtime(xtc) >= newest_src:
            continue
        pages = os.path.join(os.path.dirname(out), "pages", name + "-landscape")
        subprocess.run([sys.executable, tool, md, pages, "--xtc", xtc], check=True, stdout=subprocess.DEVNULL)
        print(f"  rendered {name}.xtc")
        built += 1
    print(f"build: {built} re-rendered, {len(glob.glob(md_glob)) - built} up to date")


class Link:
    """'@' lines are protocol; anything else from the X3 is log output and ignored."""

    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.2)  # default open: no reset
        self.buf = b""

    def send(self, line):
        self.s.write((line + "\n").encode())

    def reply(self, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                line = line.decode(errors="replace").strip()
                if line.startswith("@"):
                    return line
            # Read what has arrived (at least 1 byte). read(256) would wait for 256
            # bytes or the full timeout -- 200 ms per 10-byte @ACK, i.e. 19 KB/s.
            self.buf += self.s.read(max(1, self.s.in_waiting))
        raise TimeoutError("no reply from the X3")


def wait_for_port(port):
    if os.path.exists(port):
        return
    print(f"waiting for {port} - press Power on the X3 to wake it ...")
    while not os.path.exists(port):
        time.sleep(0.5)
    time.sleep(1.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--md", default=os.path.join(ROOT, "docs", "lesson-*.md"),
                    help="Markdown files to render (glob; default: the project's docs/lesson-*.md)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "xtc"),
                    help="folder of .xtc files; everything in it is synced to /books on the X3")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--delete", action="store_true")
    ap.add_argument("--no-build", action="store_true")
    a = ap.parse_args()

    if not a.no_build:
        build(a.md, a.out)
    local = {os.path.basename(p): open(p, "rb").read() for p in sorted(glob.glob(os.path.join(a.out, "*.xtc")))}
    print(f"local: {len(local)} .xtc in {a.out}")
    if a.delete and not local:
        sys.exit("refusing --delete: no local .xtc files (wrong --md/--out?) - it would empty the X3's lessons")

    wait_for_port(a.port)
    link = Link(a.port)
    for attempt in range(3):
        link.s.reset_input_buffer()
        link.send("@HELLO")
        try:
            r = link.reply(3)
            if r.startswith("@OK X3SYNC"):
                break
        except TimeoutError:
            pass
    else:
        sys.exit("the X3 didn't answer @HELLO (is the reader firmware running and awake?)")

    t0 = time.time()
    link.send("@LIST")
    remote = {}
    while (r := link.reply(30)) != "@END":
        _, size, crc, name = r.split(" ", 3)
        remote[name] = (int(size), int(crc, 16))
    list_s = time.time() - t0

    sent = sent_bytes = unchanged = 0
    t1 = time.time()
    for name, data in local.items():
        crc = zlib.crc32(data)
        if remote.get(name) == (len(data), crc):
            unchanged += 1
            continue
        link.send(f"@PUT {name} {len(data)} {crc:08x}")
        if (r := link.reply()) != "@READY":
            sys.exit(f"{name}: {r}")
        for off in range(0, len(data), CHUNK):
            link.s.write(data[off:off + CHUNK])
            if not (r := link.reply()).startswith("@ACK"):
                sys.exit(f"{name}: {r}")
        r = link.reply(10)
        print(f"  {name}: {r} ({len(data) // 1024} KB)")
        if r == "@OK":
            sent += 1
            sent_bytes += len(data)
    put_s = time.time() - t1

    deleted = 0
    if a.delete:
        for name in remote:
            if name.endswith(".xtc") and name not in local:
                link.send(f"@DEL {name}")
                print(f"  deleted {name}: {link.reply()}")
                deleted += 1
    link.send("@DONE")
    link.reply()

    rate = f", {sent_bytes / 1024 / put_s:.0f} KB/s" if sent_bytes else ""
    print(f"sync: {sent} sent, {unchanged} unchanged, {deleted} deleted  "
          f"(list {list_s:.1f} s, transfer {put_s:.1f} s{rate})")
    # CRC cross-check: the X3's CRC32 of files we didn't send must equal zlib's
    same = [n for n in local if n in remote and remote[n] == (len(local[n]), zlib.crc32(local[n]))]
    print(f"crc agreement (X3 rom vs zlib) on {len(same)} unchanged files")


if __name__ == "__main__":
    main()
