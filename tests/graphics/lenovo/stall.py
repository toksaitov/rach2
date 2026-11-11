"""Time the hasvk WebGPU picture's first frames on the Lenovo, with each thread's waits.

Run as root: python3 stall.py USER OUT RUNS, on Rach2 or on the Lenovo's
Debian alike. Like hasvk.py it owns the screen: it ends the boot session's
Weston, starts a private DRM Weston through seatd, presents vkcube's 300
frames once, as hasvk.py does before its picture, and then runs Chromium's
WebGPU picture RUNS times as USER, the even runs alone and the odd ones
with Chromium's own trace. threadwatch samples the threads of Chromium,
Weston and Xwayland through each run, with the GPU's clock and the i915
interrupt's count. OUT must not exist; it keeps every run and, in
summary.json and stall.txt, the slowest gap of each run's first frames and
what each thread did in it.
"""
import gzip
import json
import os
from pathlib import Path
import pwd
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from hasvk import GRAPHICS, end_desktop, machine, private_weston, run, user_command
from run import save, stop

# The first frames searched for the slowest gap, and the first frame of the
# steady rate after it.
FIRST = 16
STEADY = 10
# What threadwatch's window around a gap keeps before and after it, in ns.
MARGIN = 20000000


def snapshot(card, into):
    """Copy the kernel's counters that a stall could move into INTO."""
    into.mkdir()
    names = [Path("/proc/interrupts"), Path("/proc/stat"), Path("/proc/meminfo"),
             card / "gt_act_freq_mhz", card / "gt_cur_freq_mhz"]
    for debug in Path("/sys/kernel/debug/dri").glob("*/i915_frequency_info"):
        names += [debug, debug.parent / "i915_gem_objects", debug.parent / "i915_engine_info"]
    for path in names:
        try:
            (into / str(path).strip("/").replace("/", "_")).write_bytes(path.read_bytes())
        except OSError as error:
            (into / (str(path).strip("/").replace("/", "_") + ".error")).write_text(str(error) + "\n")


def frames(picture):
    """Return the slowest gap of the first frames and the rates after it, from the page's own times."""
    times, starts, ends = picture["frameTimes"], picture["frameStarts"], picture["frameEnds"]
    gaps = [times[i + 1] - times[i] for i in range(FIRST - 1)]
    k = max(range(len(gaps)), key=gaps.__getitem__)
    late = sum(1 for i in range(STEADY, len(times) - 1) if times[i + 1] - times[i] > 25)
    return {"gap_ms": round(gaps[k], 1), "after_frame": k,
            "first_gaps_ms": [round(g, 1) for g in gaps],
            # The page's own work around the gap: callback k's return, then
            # callback k+1's entry, against the frame times it was given.
            "callback_end_ms": ends[k], "next_start_ms": starts[k + 1], "next_time_ms": times[k + 1],
            "steady_fps": round((len(times) - 1 - STEADY) * 1000 / (times[-1] - times[STEADY]), 2),
            "late_after_steady": late, "seconds": picture["seconds"], "frames": picture["frames"],
            "phases": picture["phases"]}


def watched(log, low, high):
    """Return threadwatch's lines in [LOW, HIGH] ns of its clock, after each thread's state at LOW."""
    before, inside, header = {}, [], []
    for line in log.read_text().splitlines():
        if line.startswith("#"):
            header.append(line)
            continue
        fields = line.split(" ", 2)
        stamp = int(fields[1].replace(".", ""))
        if stamp < low:
            if fields[0] == "T":
                before[" ".join(line.split(" ", 4)[2:4])] = line
        elif stamp <= high:
            inside.append(line)
    return header, sorted(before.values()), inside


def traced(path, k):
    """Return the trace's marks of frames K and K+1 in us and its long events between them."""
    events = json.loads(gzip.decompress(path.read_bytes()))
    events = events["traceEvents"] if isinstance(events, dict) else events
    names = {(e["pid"], e["tid"]): e["args"]["name"] for e in events
             if e.get("ph") == "M" and e.get("name") == "thread_name"}
    marks = {}
    for e in events:
        if e.get("name", "").startswith("hasvk-frame-") and "ts" in e:
            marks.setdefault(int(e["name"].rsplit("-", 1)[1]), e["ts"])
    if k not in marks or k + 1 not in marks:
        return None, None, ["no marks of frames %d and %d in the trace" % (k, k + 1)]
    low, high = marks[k], marks[k + 1]
    rows = []
    for e in events:
        if e.get("ph") != "X" or "dur" not in e or e["dur"] < 5000:
            continue
        if e["ts"] < high and e["ts"] + e["dur"] > low:
            rows.append((e["dur"], "%9.1f ms at %+9.1f ms %s/%s %s %s" % (
                e["dur"] / 1000, (e["ts"] - low) / 1000, e["pid"], names.get((e["pid"], e["tid"]), e["tid"]),
                e.get("cat", ""), e.get("name", ""))))
    rows.sort(reverse=True)
    return low, high, [row for _, row in rows[:60]]


def main():
    """Run the picture beside the sampler RUNS times, keeping each run's evidence."""
    user, name, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
    assert os.getuid() == 0, "run as root to read every thread's syscall file"
    account = pwd.getpwnam(user)
    assert account.pw_uid != 0, "the programs need an ordinary user"
    out = Path(name).resolve()
    assert not out.exists(), "use a fresh output directory"
    card, icd = machine()
    work = weston = watch = None
    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sig, lambda *_: sys.exit(1))
    try:
        out.mkdir(parents=True)
        os.chown(out, account.pw_uid, account.pw_gid)
        work = Path(tempfile.mkdtemp(prefix="rach2-stall.", dir=os.environ.get("TMPDIR", "/tmp")))
        os.chown(work, account.pw_uid, account.pw_gid)
        sampler = work / "threadwatch"
        subprocess.run(["gcc", "-std=c99", "-O2", "-o", str(sampler), str(HERE / "threadwatch.c")], check=True)
        runtime = work / "run"
        runtime.mkdir(mode=0o700)
        os.chown(runtime, account.pw_uid, account.pw_gid)
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), WAYLAND_DISPLAY="wayland-hasvk",
                   VK_DRIVER_FILES=str(icd), MESA_SHADER_CACHE_DIR=str(work / "cache"),
                   GRAPHICS_CLASS="performance", GRAPHICS_DONE="1", PYTHONDONTWRITEBYTECODE="1")
        save(out / "machine.json", {"card": str(card), "icd": str(icd), "system": os.uname().sysname,
             "cmdline": Path("/proc/cmdline").read_text(), "user": user,
             "kernel": os.uname().release})
        if os.uname().sysname == "Rach2":
            subprocess.run(["mixer", "vol", "1"], check=True)
        end_desktop()
        weston = private_weston(user, out, env)
        setup = ". " + shlex.quote(str(GRAPHICS / "guest-env.sh"))
        run(user_command(user, ["sh", "-c", setup + " && graphics_env"]), out / "environment.log", env)
        run(user_command(user, ["sh", "-c", setup + " && exec vkcube --wsi wayland --c 300"]),
            out / "cube.log", env)
        summary = []
        report = []
        for index in range(count):
            mode = "webgpu-trace" if index % 2 else "webgpu-frames"
            here = out / ("run-%d" % index)
            here.mkdir()
            os.chown(here, account.pw_uid, account.pw_gid)
            snapshot(card, here / "before")
            log = here / "threadwatch.txt"
            watch = subprocess.Popen([str(sampler), "-i", "1000",
                "-f", str(card / "gt_act_freq_mhz"), "-f", "/proc/interrupts@i915",
                str(log), "600", "chromium", "weston", "Xwayland"], start_new_session=True)
            try:
                run(["python3", str(GRAPHICS / "browser/sandbox.py"), user, "chromium",
                     str(here / "browser"), mode], here / "browser.log", env)
            finally:
                watch.send_signal(signal.SIGTERM)
                assert watch.wait() == 0, "threadwatch failed"
                watch = None
            snapshot(card, here / "after")
            picture = json.loads((here / "browser/result.json").read_text())["picture"]
            row = dict(frames(picture), run=index, mode=mode)
            summary.append(row)
            save(out / "summary.json", summary)
            print("stall: run %d: %s: gap %.1f ms after frame %d, steady %.2f FPS, %d late, %.4f s" % (
                index, mode, row["gap_ms"], row["after_frame"], row["steady_fps"],
                row["late_after_steady"], row["seconds"]), flush=True)
            k = row["after_frame"]
            report.append("run %d (%s): %s" % (index, mode, json.dumps(row, sort_keys=True)))
            header, _, _ = watched(log, 0, 0)
            # threadwatch's first line pairs its two clocks; the page's
            # times are on the epoch's through timeOrigin.
            parts = header[0].split()
            offset = int(parts[5].replace(".", "")) - int(parts[3].replace(".", ""))
            origin = (picture["timeOrigin"] + picture["start"]) * 1e6
            low = int(origin + picture["frameEnds"][k] * 1e6) - offset
            high = int(origin + picture["frameStarts"][k + 1] * 1e6) - offset
            if mode == "webgpu-trace":
                mark_low, mark_high, rows = traced(here / "browser/trace.json.gz", k)
                if mark_low is not None:
                    report.append("trace: frames %d and %d marked %.1f ms apart; the epoch's mapping put "
                                  "callback %d's return %+.1f ms from its mark" % (
                                      k, k + 1, (mark_high - mark_low) / 1000, k,
                                      (low / 1000 - mark_low) / 1000))
                    # The trace's clock is the sampler's: CLOCK_MONOTONIC.
                    low, high = int(mark_low * 1000), int(mark_high * 1000)
                report += ["trace: " + line for line in rows]
            header, before, inside = watched(log, low - MARGIN, high + MARGIN)
            report += header
            report.append("threads at %d.%09d, 20 ms before the gap:" % divmod(low - MARGIN, 1000000000))
            report += before
            report.append("changes from 20 ms before to 20 ms after the gap, which ends at %d.%09d:"
                          % divmod(high, 1000000000))
            report += inside
            report.append("")
            (out / "stall.txt").write_text("\n".join(report) + "\n")
        (out / "dmesg.txt").write_text(subprocess.run(["dmesg"], capture_output=True, text=True).stdout)
        print("stall: result: %d runs, gaps %s ms" % (count, [row["gap_ms"] for row in summary]), flush=True)
    finally:
        if watch is not None:
            stop(watch)
        stop(weston)
        if work is not None:
            shutil.rmtree(work)
        if out.exists():
            print("stall: evidence: " + str(out), flush=True)


if __name__ == "__main__":
    main()
