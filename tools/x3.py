#!/usr/bin/env python3
"""x3 - keep the Xteink X3 in step with a library of lessons and talks (stage 16).

The library is a folder of SYMLINKS (no copies, so nothing goes stale):

  ~/X3/lessons/<series> -> a project's docs folder     -> X3: /books/<series>/<file>.xtc
  ~/X3/talks/<deck>     -> a marimo deck's repo folder -> X3: /books/talks/<deck>.xtc
  ~/X3/x3.toml          optional, per series: [lessons.<series>] include = "lesson-*.md"

Commands:
  x3 add <path> [--name N] [--include GLOB]   link a docs folder (lessons) or a deck folder (talk)
  x3 status                                   library vs the X3 (if plugged in)
  x3 build                                    render what changed (cache: ~/.cache/x3)
  x3 sync [--wait S]                          build, then send what differs (size + CRC32)
  x3 rm <name>...                             delete from the X3 (asks first) - sync NEVER deletes

The X3 is found by its USB id (303a:1001) and its answer to @HELLO, not by a fixed
/dev/ttyACM0 (it re-enumerates as ttyACM1 at times). Opening the port doesn't reset it.
Protocol: src/sync_server.h (version 2: one folder level).
"""
import argparse
import ast
import fcntl
import fnmatch
import glob
import os
import re
import sys
import time
import tomllib
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

LIB = os.path.expanduser(os.environ.get("X3_LIBRARY", "~/X3"))
CACHE = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")), "x3")
USB_ID = (0x303A, 0x1001)  # Espressif USB-Serial/JTAG (any C3/S3 board: @HELLO decides)
CHUNK = 4096
PROTOCOL = 2
# Markdown files in a docs folder that aren't lessons (when a series has no include= glob)
NOT_LESSONS = {"readme", "plan", "changelog", "license", "contributing", "todo"}
DOCS_NAMES = {"docs", "doc", "lessons"}  # "<project>/docs" -> series named after the project


# ------------------------------------------------------------------ library ---

def safe(name):
    """A name the X3 accepts: letters, digits, . _ - (src/sync_server.cpp safeName)."""
    return re.sub(r"[^A-Za-z0-9._-]+", "-", name).strip("-.") or "x"


def config():
    path = os.path.join(LIB, "x3.toml")
    if not os.path.exists(path):
        return {}
    with open(path, "rb") as f:
        return tomllib.load(f)


def deck_files(folder):
    """marimo notebooks in `folder` that have a slides layout (layout_file=... that exists)."""
    decks = []
    for py in sorted(glob.glob(os.path.join(folder, "*.py"))):
        try:
            tree = ast.parse(open(py, encoding="utf-8").read())
        except (SyntaxError, UnicodeDecodeError):
            continue
        for node in ast.walk(tree):
            if isinstance(node, ast.keyword) and node.arg == "layout_file" and isinstance(node.value, ast.Constant):
                layout = os.path.join(folder, node.value.value)
                if str(node.value.value).endswith(".slides.json") and os.path.exists(layout):
                    decks.append((py, layout))
                break
    return decks


class Item:
    """One file on the X3 and how to make it."""

    def __init__(self, kind, device, sources, label):
        self.kind = kind          # "lesson" | "talk"
        self.device = device      # name on the X3, relative to /books: "xteink3/lesson-01.xtc"
        self.sources = sources    # files whose change means re-render (first = the main one)
        self.label = label
        self.xtc = os.path.join(CACHE, "xtc", device)
        self.pages = os.path.join(CACHE, "pages", device[:-4])

    def renderer(self):
        return os.path.join(HERE, "mdpages.py" if self.kind == "lesson" else "notes2xtc.py")

    def stale(self):
        if not os.path.exists(self.xtc):
            return True
        deps = self.sources + [os.path.join(HERE, "mdpages.py"), self.renderer()]
        return os.path.getmtime(self.xtc) < max(os.path.getmtime(p) for p in deps)


def library():
    """Every item in the library, plus problems found (broken links etc.)."""
    items, problems = [], []
    cfg = config().get("lessons", {})
    for kind, sub in (("lesson", "lessons"), ("talk", "talks")):
        root = os.path.join(LIB, sub)
        for entry in sorted(os.listdir(root)) if os.path.isdir(root) else []:
            path = os.path.join(root, entry)
            if entry.startswith("."):
                continue
            if not os.path.isdir(path):
                problems.append(f"{sub}/{entry}: " + ("broken link -> " + os.readlink(path)
                                                      if os.path.islink(path) else "not a folder"))
                continue
            if kind == "lesson":
                include = cfg.get(entry, {}).get("include")
                mds = sorted(glob.glob(os.path.join(path, include or "*.md")))
                if not include:
                    mds = [m for m in mds if os.path.splitext(os.path.basename(m))[0].lower() not in NOT_LESSONS]
                if not mds:
                    problems.append(f"lessons/{entry}: no Markdown files" + (f" match {include}" if include else ""))
                for md in mds:
                    stem = os.path.splitext(os.path.basename(md))[0]
                    items.append(Item("lesson", f"{safe(entry)}/{safe(stem)}.xtc", [md], f"{entry}/{stem}"))
            else:
                decks = deck_files(path)
                if not decks:
                    problems.append(f"talks/{entry}: no marimo notebook with a slides layout")
                for py, layout in decks:
                    name = entry if len(decks) == 1 else f"{entry}-{os.path.splitext(os.path.basename(py))[0]}"
                    items.append(Item("talk", f"talks/{safe(name)}.xtc", [py, layout], name))
    return items, problems


def build(items, quiet=False):
    """Render stale items. Returns (built, failed) lists of items."""
    import mdpages
    import notes2xtc
    built, failed, fonts = [], [], None
    for it in items:
        if not it.stale():
            continue
        fonts = fonts or mdpages.Fonts()
        os.makedirs(os.path.dirname(it.xtc), exist_ok=True)
        print(f"  render {it.device}")
        try:
            if quiet:  # the renderers print a lot; keep warnings only
                import contextlib
                import io
                buf = io.StringIO()
                with contextlib.redirect_stdout(buf):
                    _render(it, fonts, mdpages, notes2xtc)
                for line in buf.getvalue().splitlines():
                    if "WARNING" in line:
                        print("   ", line.strip())
            else:
                _render(it, fonts, mdpages, notes2xtc)
            built.append(it)
        except (SystemExit, Exception) as e:  # one bad file must not stop the rest
            print(f"  FAILED {it.device}: {e}")
            failed.append(it)
    return built, failed


def _render(it, fonts, mdpages, notes2xtc):
    tmp = it.xtc + ".tmp"  # never leave a half-written file in the cache
    if it.kind == "lesson":
        mdpages.build(it.sources[0], it.pages, tmp, F=fonts)
    else:
        notes2xtc.build(it.sources[0], tmp, it.pages, name=it.label, F=fonts)
    os.replace(tmp, it.xtc)


# ------------------------------------------------------------------- device ---

class Link:
    """'@' lines are protocol; anything else from the X3 is log output and ignored."""

    def __init__(self, port):
        import serial
        self.port = port
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

    def hello(self, tries=4):
        for _ in range(tries):  # just plugged in: the X3 may still be booting (~1-4 s)
            self.s.reset_input_buffer()
            self.send("@HELLO")
            try:
                r = self.reply(2.5)
            except TimeoutError:
                continue
            if r.startswith("@OK X3SYNC"):
                return int(r.split()[-1])
        return 0

    def close(self):
        self.s.close()


class Lock:
    """One x3 at a time on the X3 (the auto-sync service and a manual run must not mix)."""

    def __init__(self):
        os.makedirs(CACHE, exist_ok=True)
        self.f = open(os.path.join(CACHE, "device.lock"), "w")

    def acquire(self, wait):
        end = time.time() + wait
        while True:
            try:
                fcntl.flock(self.f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return True
            except BlockingIOError:
                if time.time() >= end:
                    return False
                time.sleep(0.5)


def candidate_ports():
    from serial.tools import list_ports
    return [p.device for p in list_ports.comports() if (p.vid, p.pid) == USB_ID]


def connect(wait):
    """Find the X3 (wait up to `wait` s for it to appear) -> Link, or exit with a message."""
    lock = Lock()
    if not lock.acquire(wait=max(wait, 5)):
        sys.exit("another x3 is talking to the X3 (auto-sync?) - try again in a moment")
    end = time.time() + wait
    said = False
    while True:
        for port in candidate_ports():
            try:
                link = Link(port)
            except OSError:
                continue  # still settling, or another program holds it
            version = link.hello()
            if version >= PROTOCOL:
                link.lock = lock
                return link
            link.close()
            if version:
                sys.exit(f"{port}: X3 firmware speaks sync protocol {version}, x3 needs {PROTOCOL} - "
                         "flash the current firmware (pio run -t upload)")
        if time.time() >= end:
            sys.exit("no X3 found - plug it in (it boots by itself); is the reader firmware on it?")
        if not said:
            print(f"waiting up to {wait:.0f} s for the X3 - plug it in ...")
            said = True
        time.sleep(1.0)


def remote_list(link):
    link.send("@LIST")
    remote = {}
    while (r := link.reply(60)) != "@END":
        _, size, crc, name = r.split(" ", 3)
        remote[name] = (int(size), int(crc, 16))
    return remote


def finish(link):
    link.send("@DONE")
    try:
        link.reply()
    except TimeoutError:
        pass
    link.close()


def put(link, name, data):
    crc = zlib.crc32(data)
    link.send(f"@PUT {name} {len(data)} {crc:08x}")
    if (r := link.reply()) != "@READY":
        return r
    for off in range(0, len(data), CHUNK):
        link.s.write(data[off:off + CHUNK])
        if not (r := link.reply()).startswith("@ACK"):
            return r
    return link.reply(15)


# ----------------------------------------------------------------- commands ---

def cmd_add(a):
    src = os.path.abspath(os.path.expanduser(a.path))
    if os.path.isfile(src) and src.endswith(".py"):
        src = os.path.dirname(src)
    if not os.path.isdir(src):
        sys.exit(f"{a.path}: not a folder")
    if deck_files(src):
        kind, sub = "talk", "talks"
        name = a.name or os.path.basename(src)
    elif glob.glob(os.path.join(src, "*.md")):
        kind, sub = "lessons", "lessons"
        base = os.path.basename(src)
        name = a.name or (os.path.basename(os.path.dirname(src)) if base.lower() in DOCS_NAMES else base)
    else:
        sys.exit(f"{src}: neither a marimo deck (a .py with a *.slides.json layout) nor a folder of .md files")
    if a.include and kind != "lessons":
        sys.exit("--include is for lesson folders")
    name = safe(name)
    link = os.path.join(LIB, sub, name)
    os.makedirs(os.path.dirname(link), exist_ok=True)
    if os.path.lexists(link):
        if os.path.islink(link) and os.path.realpath(link) == os.path.realpath(src):
            print(f"already there: {sub}/{name} -> {src}")
        else:
            sys.exit(f"{sub}/{name} already exists (-> {os.path.realpath(link)}); pick another --name")
    else:
        os.symlink(src, link)
        print(f"added {kind}: {sub}/{name} -> {src}")
    if a.include:
        set_include(name, a.include)
    items = [i for i in library()[0] if i.device.startswith(("talks/" if kind == "talk" else name + "/"))]
    shown = [i.device for i in items if kind != "talk" or i.label.startswith(name)]
    print(f"  on the X3 as: {', '.join(shown) if shown else '(nothing - check the folder)'}")


def set_include(series, pattern):
    """Write [lessons.<series>] include = "<pattern>" into x3.toml (rest of the file kept)."""
    path = os.path.join(LIB, "x3.toml")
    text = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
    block = f'[lessons.{series}]\ninclude = "{pattern}"\n'
    pat = re.compile(rf"^\[lessons\.{re.escape(series)}\]\n(?:[^\[\n].*\n?)*", re.M)
    text = pat.sub(block, text) if pat.search(text) else (text.rstrip("\n") + "\n\n" if text else "") + block
    open(path, "w", encoding="utf-8").write(text)
    print(f"  {series}: only files matching {pattern} (in {path})")


def cmd_build(a):
    items, problems = library()
    for p in problems:
        print("  problem:", p)
    built, failed = build(items, quiet=not a.verbose)
    print(f"build: {len(built)} rendered, {len(items) - len(built) - len(failed)} up to date, {len(failed)} failed")
    return 1 if failed else 0


def compare(items, remote):
    rows = []
    for it in items:
        if not os.path.exists(it.xtc):
            rows.append((it.device, "not rendered"))
            continue
        data = open(it.xtc, "rb").read()
        if it.device not in remote:
            rows.append((it.device, "new - not on the X3"))
        elif remote[it.device] != (len(data), zlib.crc32(data)):
            rows.append((it.device, "changed"))
        else:
            rows.append((it.device, "up to date"))
    return rows


def cmd_status(a):
    items, problems = library()
    print(f"library {LIB}: {len(items)} files")
    for p in problems:
        print("  problem:", p)
    remote = None
    if candidate_ports():
        link = connect(wait=5)
        remote = remote_list(link)
        finish(link)
    else:
        print("  (X3 not plugged in: showing the library only)")
    for it in items:
        if it.stale():
            state = "needs render" + ("" if remote is None else ", then send")
        elif remote is None:
            state = "rendered"
        else:
            state = dict(compare([it], remote))[it.device]
        print(f"  {it.device:48s} {state}")
    if remote is not None:
        mine = {it.device for it in items}
        others = sorted(n for n in remote if n not in mine)
        if others:
            print(f"only on the X3 ({len(others)}) - sync leaves these alone; remove with x3 rm <name>:")
            for n in others:
                print(f"  {n}")
    return 0


def cmd_sync(a):
    items, problems = library()
    for p in problems:
        print("  problem:", p)
    built, failed = build(items, quiet=not a.verbose)
    ready = [it for it in items if os.path.exists(it.xtc) and it not in failed]
    link = connect(wait=a.wait)
    print(f"X3 on {link.port}")
    link.send("@DATE " + time.strftime("%-d %b %H:%M"))
    link.reply()
    t0 = time.time()
    remote = remote_list(link)
    list_s = time.time() - t0

    sent, errors, nbytes, t1 = [], [], 0, time.time()
    for it in ready:
        data = open(it.xtc, "rb").read()
        if remote.get(it.device) == (len(data), zlib.crc32(data)):
            continue
        r = put(link, it.device, data)
        print(f"  {it.device}: {r} ({len(data) // 1024} KB)")
        if r == "@OK":
            sent.append(it.device)
            nbytes += len(data)
        else:
            errors.append(f"{it.device}: {r}")
    put_s = time.time() - t1
    finish(link)

    rate = f", {nbytes / 1024 / put_s:.0f} KB/s" if nbytes else ""
    others = sorted(n for n in remote if n not in {it.device for it in items})
    print(f"sync: {len(sent)} sent, {len(ready) - len(sent) - len(errors)} up to date, "
          f"{len(errors) + len(failed)} failed  (list {list_s:.1f} s, send {put_s:.1f} s{rate})")
    if others:
        print(f"  {len(others)} file(s) only on the X3 - left alone (x3 status lists them, x3 rm removes)")
    for e in errors:
        print("  ERROR", e)
    return 1 if errors or failed else 0


def cmd_rm(a):
    link = connect(wait=a.wait)
    remote = remote_list(link)
    names = []
    for pat in a.names:
        hits = sorted(n for n in remote if fnmatch.fnmatch(n, pat))
        if not hits:
            print(f"  not on the X3: {pat}")
        names += [h for h in hits if h not in names]
    if not names:
        finish(link)
        return 1
    print("delete from the X3:")
    for n in names:
        print(f"  {n}")
    if not a.yes and input(f"delete these {len(names)} file(s)? [y/N] ").strip().lower() != "y":
        print("nothing deleted")
        finish(link)
        return 1
    for n in names:
        link.send(f"@DEL {n}")
        print(f"  {n}: {link.reply()}")
    finish(link)
    return 0


def main():
    ap = argparse.ArgumentParser(prog="x3", description="Keep the Xteink X3 in step with ~/X3 (lessons + talks).")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("add", help="link a lessons folder or a marimo deck folder into the library")
    p.add_argument("path")
    p.add_argument("--name", help="series/deck name (default: from the folder)")
    p.add_argument("--include", help="lessons: only files matching this glob, e.g. 'lesson-*.md'")
    p.set_defaults(fn=cmd_add)
    p = sub.add_parser("status", help="library vs the X3")
    p.set_defaults(fn=cmd_status)
    p = sub.add_parser("build", help="render what changed")
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(fn=cmd_build)
    p = sub.add_parser("sync", help="render, then send what differs (never deletes)")
    p.add_argument("--wait", type=float, default=30, help="seconds to wait for the X3 (default 30)")
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(fn=cmd_sync)
    p = sub.add_parser("rm", help="delete files from the X3 (asks first)")
    p.add_argument("names", nargs="+", help="names as x3 status shows them; wildcards ok (quote them)")
    p.add_argument("--yes", action="store_true", help="don't ask")
    p.add_argument("--wait", type=float, default=10)
    p.set_defaults(fn=cmd_rm)
    a = ap.parse_args()
    sys.exit(a.fn(a) or 0)


if __name__ == "__main__":
    main()
