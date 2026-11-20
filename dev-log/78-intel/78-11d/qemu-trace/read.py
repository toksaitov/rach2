"""Join completed big-lock waits and scheduler delays to Chromium's frame marks."""
from collections import Counter
import gzip
import json
from pathlib import Path
import struct
import sys

HEADER = struct.Struct("<4I6Q")
ROW = struct.Struct("<3QIiIHBB")
WAKE, SELECT, RUN, HOLD = 1, 2, 3, 4
THREADS = {"Compositor", "VizCompositorThread", "CrRendererMain", "CrGpuMain"}


def kernel(path):
    """Read the stopped trace and return its rows, clock conversion and coverage."""
    raw = path.read_bytes()
    version, count, lost, _, lo1, lo2, lo_ns, hi1, hi2, hi_ns = HEADER.unpack_from(raw)
    if version != 1 or len(raw) != HEADER.size + count * ROW.size or not count:
        raise ValueError("empty, unsupported or incomplete kernel trace")
    lo, hi = (lo1 + lo2) // 2, (hi1 + hi2) // 2
    if hi <= lo or hi_ns <= lo_ns or lo2 < lo1 or hi2 < hi1:
        raise ValueError("invalid guest clock anchors")
    us_per_cycle = (hi_ns - lo_ns) / (1000 * (hi - lo))
    rows = sorted(ROW.iter_unpack(memoryview(raw)[HEADER.size:]))
    end = min(hi, rows[-1][0]) if lost else hi
    error = max(lo2 - lo1, hi2 - hi1) * us_per_cycle / 2
    print("kernel: %d records, %d lost, clock bracket at most %.1f us" % (count, lost, error))
    return rows, lambda us: lo + round((us - lo_ns / 1000) / us_per_cycle), us_per_cycle, lo, end, error


def chromium(out):
    """Read callback marks and resolve Chromium's namespace TIDs to kernel TIDs."""
    with gzip.open(out / "trace.json.gz", "rt") as trace:
        events = json.load(trace)["traceEvents"]
    marks = {int(e["name"].rsplit("-", 1)[1]): e for e in events
             if e.get("ph") == "I" and e["name"].startswith("hasvk-frame-")}
    identities = json.loads((out / "tids.json").read_text())
    names = {int((out / "weston.pid").read_text()): "weston"}
    found = set()
    for e in events:
        if e.get("ph") != "M" or e.get("name") != "thread_name":
            continue
        name = e["args"]["name"]
        if name not in THREADS:
            continue
        candidates = [r for r in identities if r["tgid"] == e["pid"] and
                      e["tid"] in (r["tid"], r["nspid"]) and r["name"] == name[:15]]
        if len(candidates) == 1:
            names[candidates[0]["tid"]] = name
            found.add(name)
    if not {"Compositor", "VizCompositorThread"} <= found:
        raise ValueError("could not map Chromium's renderer and Viz compositor TIDs")
    startup = [e["ts"] for e in events if e.get("ph") == "X" and
               e.get("args", {}).get("src_func") == "MaybeSignalStartupComplete"]
    return marks, names, startup


def wake_delays(rows, names):
    """Pair a wake with the next run of that thread, with its selection if any."""
    pending, delays = {}, []
    for at, _, _, tid, _, _, _, cpu, kind in rows:
        if tid not in names:
            continue
        if kind == WAKE:
            pending[tid] = (at, None)
        elif tid in pending and kind == SELECT:
            pending[tid] = (pending[tid][0], at)
        elif tid in pending and kind == RUN:
            wake, selected = pending.pop(tid)
            delays.append((wake, selected, at, cpu, tid))
    return delays


def lock_work(holds, lo, hi):
    """Sum each entry's hold time overlapping a window, in cycles."""
    work = Counter()
    for end, begin, _, tid, nr, arg, vector, cpu, _ in holds:
        overlap = min(end, hi) - max(begin, lo)
        if overlap > 0:
            work[(cpu, tid, nr, vector, arg)] += overlap
    return work


def describe(work, rate, limit=5):
    """Print the longest lock entries in a window."""
    for (cpu, tid, nr, vector, arg), cycles in work.most_common(limit):
        print("    %.3f ms  cpu %d tid %d syscall %d vector %d ioctl 0x%x" %
              (cycles * rate / 1000, cpu, tid, nr, vector, arg))


def frame(index, lo, hi, names, delays, holds, rate):
    """Report wake delays, lock work and holders during compositor lock waits."""
    nearby = [r for r in holds if r[0] >= lo and r[1] - r[2] <= hi]
    longest = sorted((r for r in delays if r[2] >= lo and r[0] <= hi),
                     key=lambda r: r[2] - r[0], reverse=True)
    print("frame %d to %d" % (index, index + 1))
    for wake, selected, run, cpu, tid in longest[:8]:
        before_run = ("%.3f ms from select" % ((run - selected) * rate / 1000)
                      if selected is not None else "no intervening select")
        print("  wake to run %.3f ms, %s, cpu %d tid %d %s" %
              ((run - wake) * rate / 1000, before_run, cpu, tid, names[tid]))
    print("  big-lock holds overlapping the frame interval")
    describe(lock_work(nearby, lo, hi), rate)
    for _, begin, wait, tid, nr, _, _, cpu, _ in nearby:
        if tid not in names or wait * rate < 500 or begin < lo or begin - wait > hi:
            continue
        print("  big-lock wait %.3f ms, cpu %d tid %d %s syscall %d" %
              (wait * rate / 1000, cpu, tid, names[tid], nr))
        describe(lock_work(holds, begin - wait, begin), rate)


def main(run):
    """Read an extracted run directory and show the longest late-frame windows."""
    out = run / "out"
    rows, cycles, rate, start, end, error = kernel(run / "kernel.bin")
    marks, names, startup = chromium(out)
    clock = json.loads((out / "clock.json").read_text())
    picture = json.loads((out / "result.json").read_text())["picture"]
    times, starts = picture["frameTimes"], picture["frameStarts"]
    if set(marks) != set(range(len(times))):
        raise ValueError("Chromium's trace lacks frame marks")
    epoch_offset = (clock["epoch"] - (clock["before"] + clock["after"]) / 2) / 1000
    expected = (picture["timeOrigin"] + picture["start"] + starts[0]) * 1000 - epoch_offset
    skew = marks[0]["ts"] - expected
    print("chromium: first mark differs from the guest clock pair by %.1f us" % skew)
    if abs(skew) > 2000 + error + (clock["after"] - clock["before"]) / 2000:
        raise ValueError("Chromium and the kernel could not be placed on the same clock")
    gaps = [b - a for a, b in zip(times, times[1:])]
    early = max(range(15), key=gaps.__getitem__)
    late = [i for i in range(10, len(gaps)) if gaps[i] > 25]
    print("picture: first gap %.1f ms after frame %d, %d late frames after startup" %
          (gaps[early], early, len(late)))
    holds = [r for r in rows if r[-1] == HOLD]
    delays = wake_delays(rows, names)
    selected = [early] + sorted(late, key=gaps.__getitem__, reverse=True)[:5]
    if startup:
        stamp = min(startup)
        print("chromium: deferred startup begins %+.3f ms from the first callback" %
              ((stamp - marks[0]["ts"]) / 1000))
        for i in range(len(gaps)):
            if marks[i]["ts"] <= stamp < marks[i + 1]["ts"]:
                selected.append(i)
                break
    for i in dict.fromkeys(selected):
        lo = cycles(marks[i]["ts"] - max(0, starts[i] - times[i]) * 1000)
        hi = cycles(marks[i + 1]["ts"])
        if lo < start or hi > end:
            print("frame %d: outside recorded kernel coverage" % i)
            continue
        frame(i, lo, hi, names, delays, holds, rate)


if __name__ == "__main__":
    main(Path(sys.argv[1]))
