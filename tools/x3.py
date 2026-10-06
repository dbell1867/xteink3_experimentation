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
  x3 auto on|off|status                       auto-sync on plug-in (tools/autosync/: udev + systemd user unit)

The X3 is found by its USB id (303a:1001) and its answer to @HELLO, not by a fixed
/dev/ttyACM0 (it re-enumerates as ttyACM1 at times). Opening the port doesn't reset it.
Protocol: src/sync_server.h (version 2: one folder level).

Also a library for the marimo manager (~/Work/Python/Marimo/x3_manager): the functions
under "API" print progress and raise X3Error instead of exiting.
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
import subprocess
import tomllib
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

LIB = os.path.expanduser(os.environ.get("X3_LIBRARY", "~/X3"))
CACHE = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")), "x3")
USB_ID = (0x303A, 0x1001)  # Espressif USB-Serial/JTAG (any C3/S3 board: @HELLO decides)
PAUSED = os.path.join(CACHE, "auto-paused")  # x3 auto off: plug-in syncs do nothing (e.g. while flashing)
CHUNK = 4096
PROTOCOL = 2
# Markdown files in a docs folder that aren't lessons (when a series has no include= glob)
NOT_LESSONS = {"readme", "plan", "changelog", "license", "contributing", "todo"}
DOCS_NAMES = {"docs", "doc", "lessons"}  # "<project>/docs" -> series named after the project


class X3Error(Exception):
    """Something the user must fix (no X3, name taken, ...): the message says what."""


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
        if getattr(self, "lock", None):
            self.lock.release()  # a long-running app must not keep the X3 locked


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

    def release(self):
        try:
            fcntl.flock(self.f, fcntl.LOCK_UN)
        finally:
            self.f.close()


def candidate_ports():
    from serial.tools import list_ports
    return [p.device for p in list_ports.comports() if (p.vid, p.pid) == USB_ID]


def connect(wait):
    """Find the X3 (wait up to `wait` s for it to appear) -> Link; X3Error if it can't."""
    lock = Lock()
    if not lock.acquire(wait=max(wait, 5)):
        lock.f.close()
        raise X3Error("another x3 is talking to the X3 (auto-sync?) - try again in a moment")
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
                lock.release()
                raise X3Error(f"{port}: X3 firmware speaks sync protocol {version}, x3 needs {PROTOCOL} - "
                              "flash the current firmware (pio run -t upload)")
        if time.time() >= end:
            lock.release()
            raise X3Error("no X3 found - plug it in (if it still says Sleeping after 5 s, press Power)")
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


# ---------------------------------------------------------------------- API ---

def sources():
    """The library's entries: [{kind, name, target, ok}] (kind 'lessons' or 'talks')."""
    out = []
    for sub in ("lessons", "talks"):
        root = os.path.join(LIB, sub)
        for entry in sorted(os.listdir(root)) if os.path.isdir(root) else []:
            if entry.startswith("."):
                continue
            path = os.path.join(root, entry)
            target = os.readlink(path) if os.path.islink(path) else path
            out.append({"kind": sub, "name": entry, "target": target, "ok": os.path.isdir(path)})
    return out


def add_source(path, name=None, include=None):
    """Link a lessons folder or a marimo deck folder into the library.
    Returns {kind, name, target, devices, already}; X3Error if it can't."""
    src = os.path.abspath(os.path.expanduser(path))
    if os.path.isfile(src) and src.endswith(".py"):
        src = os.path.dirname(src)
    if not os.path.isdir(src):
        raise X3Error(f"{path}: not a folder")
    if deck_files(src):
        sub = "talks"
        name = name or os.path.basename(src)
    elif glob.glob(os.path.join(src, "*.md")):
        sub = "lessons"
        base = os.path.basename(src)
        name = name or (os.path.basename(os.path.dirname(src)) if base.lower() in DOCS_NAMES else base)
    else:
        raise X3Error(f"{src}: neither a marimo deck (a .py with a *.slides.json layout) nor a folder of .md files")
    if include and sub != "lessons":
        raise X3Error("an include pattern is for lesson folders only")
    name = safe(name)
    link = os.path.join(LIB, sub, name)
    already = False
    if os.path.lexists(link):
        if os.path.islink(link) and os.path.realpath(link) == os.path.realpath(src):
            already = True
        else:
            raise X3Error(f"{sub}/{name} already exists (-> {os.path.realpath(link)}); pick another name")
    else:
        os.makedirs(os.path.dirname(link), exist_ok=True)
        os.symlink(src, link)
    if include:
        set_include(name, include)
    prefix = "talks/" if sub == "talks" else name + "/"
    devices = [i.device for i in library()[0]
               if i.device.startswith(prefix) and (sub == "lessons" or i.label.startswith(name))]
    return {"kind": sub, "name": name, "target": src, "devices": devices, "already": already}


def remove_source(kind, name):
    """Take an entry out of the library: removes the SYMLINK only, never the folder it points to.
    Files already on the X3 stay there (x3 rm)."""
    link = os.path.join(LIB, kind, name)
    if not os.path.islink(link):
        raise X3Error(f"{kind}/{name} is not a library link - not touching it")
    os.remove(link)
    if kind == "lessons":
        set_include(name, None)


def set_include(series, pattern):
    """[lessons.<series>] include = "<pattern>" in x3.toml (rest of the file kept); None removes it."""
    path = os.path.join(LIB, "x3.toml")
    text = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
    block = f'[lessons.{series}]\ninclude = "{pattern}"\n' if pattern else ""
    pat = re.compile(rf"^\[lessons\.{re.escape(series)}\]\n(?:[^\[\n].*\n?)*\n?", re.M)
    if pat.search(text):
        text = pat.sub(block + ("\n" if block else ""), text).rstrip("\n") + "\n"
    elif block:
        text = (text.rstrip("\n") + "\n\n" if text.strip() else "") + block
    if text.strip():
        open(path, "w", encoding="utf-8").write(text)
    elif os.path.exists(path):
        os.remove(path)


def device_files(wait=5):
    """{name: (size, crc32)} of everything in /books on the X3 (~1 s per MB: it CRCs each file)."""
    link = connect(wait)
    try:
        return remote_list(link)
    finally:
        finish(link)


def status_rows(items, remote):
    """One row per library item, plus the files only on the X3 (remote=None: X3 not checked)."""
    rows = []
    for it in items:
        if it.stale():
            state = "needs render"
        elif remote is None:
            state = "rendered"
        else:
            state = dict(compare([it], remote))[it.device]
        rows.append({"file": it.device, "kind": it.kind, "state": state, "source": it.sources[0]})
    if remote is not None:
        mine = {it.device for it in items}
        rows += [{"file": n, "kind": "-", "state": "only on the X3", "source": ""}
                 for n in sorted(remote) if n not in mine]
    return rows


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


def sync(wait=30, verbose=False):
    """Render what changed, send what differs, never delete. Returns (exit code, one-line summary)."""
    items, problems = library()
    for p in problems:
        print("  problem:", p)
    built, failed = build(items, quiet=not verbose)
    ready = [it for it in items if os.path.exists(it.xtc) and it not in failed]
    link = connect(wait)
    try:
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
    finally:
        finish(link)

    rate = f", {nbytes / 1024 / put_s:.0f} KB/s" if nbytes else ""
    others = sorted(n for n in remote if n not in {it.device for it in items})
    print(f"sync: {len(sent)} sent, {len(ready) - len(sent) - len(errors)} up to date, "
          f"{len(errors) + len(failed)} failed  (list {list_s:.1f} s, send {put_s:.1f} s{rate})")
    if others:
        print(f"  {len(others)} file(s) only on the X3 - left alone (x3 status lists them, x3 rm removes)")
    for e in errors:
        print("  ERROR", e)
    names = ", ".join(os.path.splitext(os.path.basename(n))[0] for n in sent[:4]) + (" ..." if len(sent) > 4 else "")
    summary = (f"{len(sent)} sent" + (f" ({names})" if sent else "") + f", {len(ready) - len(sent) - len(errors)} up to date"
               + (f", {len(errors) + len(failed)} failed" if errors or failed else ""))
    return (1 if errors or failed else 0), summary


def delete(names, wait=10):
    """Delete exact names from the X3 (the caller has asked the user). Returns {name: reply}."""
    link = connect(wait)
    out = {}
    try:
        for n in names:
            link.send(f"@DEL {n}")
            out[n] = link.reply()
            print(f"  {n}: {out[n]}")
    finally:
        finish(link)
    return out


def notify(title, body, urgent=False):
    """Desktop notification (auto-sync has no terminal). Never fails the sync.
    Omarchy's own sender first: notify-send reached its daemon (exit 0) but never showed."""
    urgency = "critical" if urgent else "normal"
    for cmd in (["omarchy-notification-send", "--app-name", "x3", "-u", urgency, title, body],
                ["notify-send", "-a", "x3", "-u", urgency, title, body]):
        try:
            if subprocess.run(cmd, timeout=5, check=False).returncode == 0:
                return
        except (OSError, subprocess.TimeoutExpired):
            continue


# ---------------------------------------------------------- command line ---

def cmd_add(a):
    r = add_source(a.path, a.name, a.include)
    print(("already there: " if r["already"] else f"added {r['kind']}: ") + f"{r['kind']}/{r['name']} -> {r['target']}")
    if a.include:
        print(f"  {r['name']}: only files matching {a.include} (in {os.path.join(LIB, 'x3.toml')})")
    print(f"  on the X3 as: {', '.join(r['devices']) if r['devices'] else '(nothing - check the folder)'}")


def cmd_build(a):
    items, problems = library()
    for p in problems:
        print("  problem:", p)
    built, failed = build(items, quiet=not a.verbose)
    print(f"build: {len(built)} rendered, {len(items) - len(built) - len(failed)} up to date, {len(failed)} failed")
    return 1 if failed else 0


def cmd_status(a):
    items, problems = library()
    print(f"library {LIB}: {len(items)} files")
    for p in problems:
        print("  problem:", p)
    remote = device_files() if candidate_ports() else None
    if remote is None:
        print("  (X3 not plugged in: showing the library only)")
    rows = status_rows(items, remote)
    for r in rows:
        if r["state"] != "only on the X3":
            print(f"  {r['file']:48s} {r['state']}")
    others = [r["file"] for r in rows if r["state"] == "only on the X3"]
    if others:
        print(f"only on the X3 ({len(others)}) - sync leaves these alone; remove with x3 rm <name>:")
        for n in others:
            print(f"  {n}")
    return 0


def cmd_sync(a):
    if a.auto and os.path.exists(PAUSED):
        print("auto-sync is off (x3 auto on to resume)")
        return 0
    try:
        code, summary = sync(a.wait, a.verbose)
    except X3Error as e:
        if a.notify:
            notify("X3 sync failed", str(e), urgent=True)
        raise
    if a.notify:
        notify("X3 synced" if code == 0 else "X3 sync: problems", summary, urgent=code != 0)
    return code


def cmd_rm(a):
    remote = device_files(a.wait)
    names = []
    for pat in a.names:
        hits = sorted(n for n in remote if fnmatch.fnmatch(n, pat))
        if not hits:
            print(f"  not on the X3: {pat}")
        names += [h for h in hits if h not in names]
    if not names:
        return 1
    print("delete from the X3:")
    for n in names:
        print(f"  {n}")
    if not a.yes and input(f"delete these {len(names)} file(s)? [y/N] ").strip().lower() != "y":
        print("nothing deleted")
        return 1
    delete(names, a.wait)
    return 0


def cmd_auto(a):
    os.makedirs(CACHE, exist_ok=True)
    if a.state == "off":
        open(PAUSED, "w").close()
        print("auto-sync off: plugging in the X3 won't sync (x3 auto on to resume)")
    elif a.state == "on":
        if os.path.exists(PAUSED):
            os.remove(PAUSED)
        print("auto-sync on")
    rule = "/etc/udev/rules.d/70-x3-sync.rules"
    unit = os.path.expanduser("~/.config/systemd/user/x3-sync.service")
    print(f"  switch:    {'OFF' if os.path.exists(PAUSED) else 'on'}")
    print(f"  udev rule: {'installed' if os.path.exists(rule) else 'MISSING'} ({rule})")
    print(f"  user unit: {'installed' if os.path.exists(unit) else 'MISSING'} ({unit})")
    print("  log:       journalctl --user -u x3-sync -n 30")
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
    p.add_argument("--notify", action="store_true", help="desktop notification with the result")
    p.add_argument("--auto", action="store_true", help="started by plugging in: do nothing if x3 auto off")
    p.set_defaults(fn=cmd_sync)
    p = sub.add_parser("auto", help="auto-sync on plug-in: on / off / status")
    p.add_argument("state", nargs="?", choices=["on", "off", "status"], default="status")
    p.set_defaults(fn=cmd_auto)
    p = sub.add_parser("rm", help="delete files from the X3 (asks first)")
    p.add_argument("names", nargs="+", help="names as x3 status shows them; wildcards ok (quote them)")
    p.add_argument("--yes", action="store_true", help="don't ask")
    p.add_argument("--wait", type=float, default=10)
    p.set_defaults(fn=cmd_rm)
    a = ap.parse_args()
    try:
        sys.exit(a.fn(a) or 0)
    except X3Error as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
