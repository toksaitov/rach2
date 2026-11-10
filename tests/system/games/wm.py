#!/usr/bin/env python3
"""Run a row of the games-wm family in the boot session's Weston on kiosk-shell.

Run as root through the guest channel. Each row prints `wm: result:` lines,
`wm: ready: NAME` where the scenario must act on the machine by QMP before
the row goes on, and `GAMESWM OK ROW` when every check holds.
"""
import array
import fcntl
import math
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time

HERE = Path(__file__).resolve().parent
PROBES = HERE / "probes"
ROWS = ("place", "scaled", "wineio")
SERVICES = Path("/var/log/services")
INI = Path("/etc/xdg/weston/weston.ini")
SUPERVISOR = Path("/run/rach2-desktop.pid")
WORK = Path("/tmp/games-wm")
GAME_ID = "2734"
UID = 1001
# CVT's 1366x768 at 60 Hz, which Weston sets when the EDID lists no such mode.
MODELINE = "85.50 1366 1436 1579 1792 768 771 774 798 +hsync +vsync"
# What the scenario sends while xplace waits: ten moves of -200 on x and
# five of +40 on y, then one key.
MOTION = (-2000, 200)
ENV = ("export XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 DISPLAY=:0 "
       "PULSE_SERVER=unix:/run/user/1001/pulse/native XDG_DATA_HOME=%s/data "
       "STEAM_COMPAT_CLIENT_INSTALL_PATH=%s/steam PROTON_LOG_DIR=%s/logs "
       "WINEDEBUG=-all,err+all; " % (WORK, WORK, WORK))
# What the rebuilt Weston of vendor/gnu/games/weston prints for --version.
REBUILT = "(Rach2 14.0.2-1+rach2.1: kiosk-active-tree, xwm-emulated-mode)"
EV_KEY = 1
KEY_ESC = 1
CHILDREN = []
FAILED = []


def say(text):
    """Print one line of the runner's output."""
    print("wm: " + text, flush=True)


def check(name, ok, detail):
    """Print a result line and remember a failed check."""
    say("result: %s: %s %s" % (name, "ok" if ok else "FAILED", detail))
    if not ok:
        FAILED.append(name)


def desktop(line):
    """Form a command under the desktop user's groups and session environment."""
    return ["su", "-s", "/bin/bash", "desktop", "-c", ENV + line]


def log_from(offset):
    """Give the services log from byte OFFSET."""
    with SERVICES.open("rb") as f:
        f.seek(offset)
        return f.read().decode(errors="replace")


def session(mode):
    """
    Give the session's output MODE through weston.ini, start the session
    again and wait until Weston lights the output in that mode and its
    Xwayland listens. Returns the output's name.
    """
    text = log_from(0)
    output = re.findall(r"Output (\S+) \(crtc \d+\) video modes:\n", text)
    assert output, "no DRM output in the services log"
    listed = re.search(r"Output %s \(crtc \d+\) video modes:\n((?:\s+\d+x\d+@.*\n)+)" %
                       re.escape(output[-1]), text)
    wanted = mode if listed and re.search(r"^\s+%s@" % mode, listed.group(1), re.M) else MODELINE
    core = INI.read_text().split("\n[output]")[0].rstrip("\n")
    INI.write_text(core + "\n[output]\nname=%s\nmode=%s\n" % (output[-1], wanted))
    offset = SERVICES.stat().st_size
    os.kill(int(SUPERVISOR.read_text()), signal.SIGHUP)
    while True:
        text = log_from(offset)
        current = re.search(r"Output (\S+) \(crtc \d+\) video modes:\n((?:\s+\S+.*\n)+)", text)
        done = current and re.search(r"^\s+%s@[\d.]+.*\bcurrent\b" % mode, current.group(2), re.M)
        if done and "xserver listening on display :0" in text:
            break
        assert not re.search(r"fatal: |Segmentation fault", text), text[-3000:]
        time.sleep(0.2)
    check("mode", True, "output %s mode %s by %s" % (output[-1], mode,
                                                    "name" if wanted == mode else "modeline"))
    return output[-1]


def keyboard():
    """Open the event device of the PS/2 keyboard for reading."""
    for entry in sorted(Path("/sys/class/input").glob("event*")):
        caps = (entry / "device/capabilities/key").read_text().split()
        # A keyboard has the letters, KEY_Q to KEY_P, in the first word.
        if caps and int(caps[-1], 16) & (0x3ff << 16) == 0x3ff << 16:
            return os.open("/dev/input/" + entry.name, os.O_RDONLY)
    raise AssertionError("no keyboard in /sys/class/input")


def wait_key(fd):
    """Wait until the keyboard reports Escape pressed."""
    size = struct.calcsize("llHHi")
    while True:
        data = os.read(fd, size)
        _, _, kind, code, value = struct.unpack("llHHi", data)
        if kind == EV_KEY and code == KEY_ESC and value == 1:
            return


def start(argv, out=subprocess.PIPE):
    """Start ARGV in a session of its own."""
    proc = subprocess.Popen(argv, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                            text=True, start_new_session=True)
    CHILDREN.append(proc)
    return proc


def xplace(width, height):
    """
    Run xplace at WIDTH by HEIGHT, announce it once it is ready, and return
    its report, which it ends when the scenario's key arrives.
    """
    proc = start(desktop("exec %s %d %d" % (PROBES / "xplace", width, height)))
    lines = []
    for line in proc.stdout:
        lines.append(line.rstrip("\n"))
        print(line, end="", flush=True)
        if line.startswith("xplace: ready"):
            say("ready: xplace-%d" % width)
    status = proc.wait()
    check("xplace-%d-exit" % width, status == 0, "status %d" % status)
    return "\n".join(lines)


def placed(name, report, width, height):
    """Check that REPORT puts its window at 0,0, WIDTH by HEIGHT, unframed and focused."""
    at = re.search(r"window at (-?\d+),(-?\d+) size (\d+)x(\d+)", report)
    check(name + "-place", at is not None and at.groups() == ("0", "0", str(width), str(height)),
          at.group(0) if at else "no place")
    frame = re.search(r"frame (none|at (-?\d+),(-?\d+) size (\d+)x(\d+) child at (-?\d+),(-?\d+))",
                      report)
    unframed = frame is not None and (frame.group(1) == "none" or frame.groups()[1:] == (
        "0", "0", str(width), str(height), "0", "0"))
    check(name + "-unframed", unframed, frame.group(0) if frame else "no frame line")
    check(name + "-focus", "focus yes" in report, "")
    check(name + "-fullscreen", "wm-fullscreen yes" in report, "")


def make_prefix():
    """Make the game's prefix with the launcher, once."""
    if (WORK / "compat" / "pfx" / "user.reg").exists():
        return
    status = subprocess.run(desktop("rach2-game --id %s --prefix %s/compat -- "
                                    "'c:\\windows\\system32\\cmd.exe' /c exit 0" % (GAME_ID, WORK)),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    check("prefix", status.returncode == 0, status.stdout.strip().splitlines()[-1:])


def launch(program, args="", extra=""):
    """Start a probe of PROBES through the launcher; give its process and output file."""
    output = WORK / (program + ".txt")
    if output.exists():
        output.unlink()
    line = "%sPROBE_OUT=Z:%s PROBE_GO=Z:%s/go rach2-game --id %s --prefix %s/compat -- %s %s" % (
        extra, output, WORK, GAME_ID, WORK, PROBES / (program + ".exe"), args)
    proc = start(desktop(line), out=open(WORK / (program + ".log"), "w"))
    return proc, output


def wait_text(path, token, proc):
    """Wait until PATH holds TOKEN; give its text, or None once PROC has ended."""
    while True:
        text = path.read_text(errors="replace") if path.exists() else ""
        if token in text:
            return text
        if proc.poll() is not None:
            return None
        time.sleep(0.05)


def full(width, height):
    """
    Hold Wine's fullscreen Direct3D 9 window at WIDTH by HEIGHT, check its
    place through xplace -q, announce it, and end it on the scenario's key.
    """
    go = WORK / "go"
    if go.exists():
        go.unlink()
    fd = keyboard()
    proc, output = launch("full64", "%d %d" % (width, height))
    try:
        text = wait_text(output, "full: ready", proc)
        check("full-%d-ready" % width, text is not None, (text or "").strip())
        if text is None:
            return
        query = subprocess.run(desktop("exec %s -q fullprobe" % (PROBES / "xplace")),
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(query.stdout, end="", flush=True)
        placed("full-%d" % width, query.stdout, 1366, 768)
        say("ready: full-%d" % width)
        wait_key(fd)
        go.touch()
        status = proc.wait()
        check("full-%d-exit" % width, status == 0, "status %d" % status)
    finally:
        os.close(fd)


def row_place():
    """xplace and Wine's window at the output's 1366x768, and xplace's raw motion."""
    session("1366x768")
    report = xplace(1366, 768)
    placed("xplace-1366", report, 1366, 768)
    check("xplace-1366-grab", "grab 0" in report, "")
    raw = re.findall(r"raw (.*) dx (-?\d+) dy (-?\d+) events (\d+) relative", report)
    sums = [(int(dx), int(dy)) for _, dx, dy, _ in raw]
    check("xplace-1366-motion", tuple(MOTION) in sums, "relative sums %s, sent %s" % (sums, MOTION))
    pointer = re.search(r"pointer (-?\d+),(-?\d+) in window (-?\d+),(-?\d+)", report)
    check("xplace-1366-confined", pointer is not None and pointer.group(3) == "0" and
          0 <= int(pointer.group(4)) < 768, pointer.group(0) if pointer else "no pointer")
    make_prefix()
    full(1366, 768)


def row_scaled():
    """xplace at an emulated 800x600 and Wine's window at 800x600, both scaled."""
    session("1366x768")
    report = xplace(800, 600)
    check("xplace-800-mode", "mode 800x600" in report, "")
    check("xplace-800-emulated", "emulated 0,0 800x600" in report,
          (re.findall(r"emulated .*", report) or ["no emulated line"])[0])
    placed("xplace-800", report, 800, 600)
    make_prefix()
    full(800, 600)


def record(stop, samples, ready):
    """Read mono signed 16-bit frames at 48000 Hz from the loopback until STOP."""
    fd = os.open("/dev/dsp", os.O_RDONLY)
    try:
        for request, value in [(0xc0045005, 16), (0xc0045006, 1), (0xc0045002, 48000)]:
            arg = array.array("i", [value])
            fcntl.ioctl(fd, request, arg, True)
            assert arg[0] == value, (request, arg[0])
        ready.set()
        while not stop.is_set():
            samples.extend(os.read(fd, 4800))
    finally:
        os.close(fd)


def tone_windows(data):
    """Count the 50 ms windows of the loopback that measure 440 Hz, and the others."""
    values = struct.unpack("<%dh" % (len(data) // 2), data)
    good = off = 0
    for at in range(0, len(values) - 2400, 2400):
        block = values[at:at + 2400]
        if math.sqrt(sum(x * x for x in block) / len(block)) < 200:
            continue
        hz = sum(a <= 0 < b for a, b in zip(block, block[1:])) * 20
        if abs(hz - 440) <= 40:
            good += 1
        else:
            off += 1
    return good, off


def row_wineio():
    """Wine's waveOut tone through PulseAudio on OSS, and DirectInput's devices."""
    make_prefix()
    for bits in ("64", "32"):
        stop, ready = threading.Event(), threading.Event()
        samples = bytearray()
        reader = threading.Thread(target=record, args=(stop, samples, ready))
        reader.start()
        try:
            while not ready.wait(0.1):
                assert reader.is_alive(), "the loopback reader failed"
            proc, output = launch("tone" + bits)
            status = proc.wait()
        finally:
            stop.set()
            reader.join()
        text = output.read_text(errors="replace") if output.exists() else ""
        print(text, end="", flush=True)
        good, off = tone_windows(samples)
        check("tone" + bits, status == 0 and "TONE OK" in text and good >= 10 and off * 5 <= good,
              "status %d, %d windows of 440 Hz, %d others" % (status, good, off))
    proc, output = launch("dinput64", extra="WINEDEBUG=-all,err+all,trace+dinput,trace+hid ")
    status = proc.wait()
    text = output.read_text(errors="replace") if output.exists() else ""
    print(text, end="", flush=True)
    log = WORK / "logs" / ("steam-%s.log" % GAME_ID)
    trace = log.read_text(errors="replace") if log.exists() else ""
    events = sorted(set(re.findall(r"/dev/input/event\d+", trace)))
    check("dinput", status == 0 and "keyboard yes, mouse yes" in text,
          "status %d, %s" % (status, ", ".join(re.findall(r"type 0x\w+ subtype 0x\w+ [^\n]*",
                                                           text))))
    check("dinput-sysfs", bool(events), "Wine's log names %s" % (", ".join(events) or "none"))
    shutil.copy(log, WORK / "dinput.proton.log") if log.exists() else None


def main():
    """Run the row named by the first argument and print the row's token."""
    if len(sys.argv) != 2 or sys.argv[1] not in ROWS:
        raise SystemExit("usage: wm.py place|scaled|wineio")
    row = sys.argv[1]
    version = subprocess.run(["weston", "--version"], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, check=False).stdout.strip()
    check("weston", REBUILT in version, version)
    shutil.rmtree(WORK, ignore_errors=True)
    WORK.mkdir(mode=0o755)
    os.chown(WORK, UID, UID)
    try:
        {"place": row_place, "scaled": row_scaled, "wineio": row_wineio}[row]()
    finally:
        for proc in CHILDREN:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
        subprocess.run(desktop("WINEPREFIX=%s/compat/pfx /opt/proton/files/bin/wineserver -k; "
                               "WINEPREFIX=%s/compat/pfx /opt/proton/files/bin/wineserver -w" %
                               (WORK, WORK)), check=False, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        for name in ("full64.log", "dinput.proton.log"):
            if (WORK / name).exists():
                say("log: %s: %s" % (name, " | ".join(
                    (WORK / name).read_text(errors="replace").splitlines()[-5:])))
        shutil.rmtree(WORK, ignore_errors=True)
    if FAILED:
        say("result: %s failed: %s" % (row, ", ".join(FAILED)))
        return 1
    print("GAMESWM OK %s" % row, flush=True)
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(128 + signal.SIGTERM))
    sys.exit(main())
