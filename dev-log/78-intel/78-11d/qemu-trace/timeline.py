"""Join Chromium, Weston and the kernel on guest CLOCK_MONOTONIC."""
import argparse
from decimal import Decimal
import gzip
import json
from pathlib import Path
import re
import struct

HEADER = struct.Struct("<4I2q")
ROW = struct.Struct("<q3Q2I")
KINDS = {
    1: ("surface", "pipe", ("frame", "line", "address")),
    2: ("arm", "pipe", ("frame", "line", "target_after")),
    3: ("irq", "pipe", ("frame", "line", "bits")),
    4: ("deliver", "pipe", ("frame", "line", "user_data")),
    5: ("stamp", "crtc", ("sequence", "sent_ns", "user_data")),
    6: ("fence_wait", "plane_index", ("context", "seqno", "deferred")),
    7: ("fence_signal", "object", ("context", "seqno", "signaled_ns")),
    8: ("request_submit", "engine", ("context", "seqno", "power")),
    9: ("request_done", "engine", ("context", "seqno", "power")),
}


def kernel(path):
    """Decode the recorded prefix and its capture bounds."""
    raw = path.read_bytes()
    version, count, lost, _, start, stop = HEADER.unpack_from(raw)
    if version != 1 or len(raw) != HEADER.size + count * ROW.size or stop <= start:
        raise ValueError("invalid DRM trace header")
    if not count:
        raise ValueError("empty DRM trace")
    rows = []
    for ns, a, b, c, kind, obj in ROW.iter_unpack(memoryview(raw)[HEADER.size:]):
        name, subject, fields = KINDS[kind]
        value = dict(zip(fields, (a, b, c)))
        value[subject] = obj
        if kind in (8, 9):
            value.update(mhz=c >> 32, rc6_ticks=c & 0xffffffff)
            del value["power"]
        rows.append((ns, "kernel", name, value))
    return rows, (start, rows[-1][0] if lost else stop, lost)


def chromium(out):
    """Read BeginFrame, Wayland and frame marks, with an independent clock check."""
    with gzip.open(out / "trace.json.gz", "rt") as source:
        events = json.load(source, parse_float=Decimal)["traceEvents"]
    clock = json.loads((out / "clock.json").read_text())
    picture = json.loads((out / "result.json").read_text())["picture"]
    midpoint = (clock["before"] + clock["after"]) // 2
    estimated = ((picture["timeOrigin"] + picture["start"] + picture["frameStarts"][0]) * 1000000
                 - (clock["epoch"] - midpoint))
    marks, rows = {}, []
    for e in events:
        name = e["name"]
        lower = name.lower()
        if e.get("ph") == "M" or "ts" not in e:
            continue
        ns = int(e["ts"] * 1000)
        if name.startswith("hasvk-frame-") and e.get("ph") == "I":
            marks[int(name.rsplit("-", 1)[1])] = ns
        elif not ("beginframe" in lower.replace("_", "") or "wayland" in e.get("cat", "").lower()
                  or "wayland" in lower or name in ("SubmitCompositorFrame", "SwapBuffers")):
            continue
        details = {"pid": e.get("pid"), "tid": e.get("tid"), "phase": e.get("ph")}
        if "dur" in e:
            details["duration_ns"] = int(e["dur"] * 1000)
        rows.append((ns, "chromium", name, details))
    if set(marks) != set(range(picture["frames"])):
        raise ValueError("Chromium's trace lacks frame marks")
    if abs(marks[0] - estimated) > 2000000 + (clock["after"] - clock["before"]) / 2:
        raise ValueError("Chromium timestamps differ from the guest clock pair")
    return rows, picture, marks


def weston(path, log):
    """Read Weston's monotonic timeline and derive the nominal repaint target."""
    text = log.read_text()
    mode = re.search(r"@([\d.]+),.*\bcurrent\b", text)
    window = re.search(r"Output repaint window is (\d+) ms", text)
    refresh = int(Decimal(1000000000) / Decimal(mode[1])) if mode else None
    lead = int(window[1]) * 1000000 if window else None
    rows = []
    for line in path.read_text().splitlines():
        event = json.loads(line)
        if "T" not in event:
            continue
        sec, ns = event.pop("T")
        name = event.pop("N")
        if "vblank_monotonic" in event:
            vsec, vns = event.pop("vblank_monotonic")
            stamp = vsec * 1000000000 + vns
            event["vblank_ns"] = stamp
            if refresh is not None and lead is not None:
                event["nominal_repaint_ns"] = stamp + refresh - lead
        rows.append((sec * 1000000000 + ns, "weston", name, event))
    if not any(name == "core_repaint_begin" for _, _, name, _ in rows):
        raise ValueError("Weston did not record repaint entry")
    return rows


def main():
    """Print the joined timeline, or the window around one callback gap."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--after-frame", type=int, help="show four refreshes around this gap")
    args = parser.parse_args()
    run = args.capture
    out = run / "out"
    display_log = out / "weston.log"
    if not display_log.exists():
        display_log = run.parent / "display/weston.log"
    stream = out / "timeline" if (out / "timeline").exists() else run / "timeline"
    rows, (start, stop, lost) = kernel(run / "drm.bin")
    browser, picture, marks = chromium(out)
    rows.extend(browser)
    rows.extend(weston(stream, display_log))
    lo, hi = start, stop
    if args.after_frame is not None:
        index = args.after_frame
        if index < 0 or index + 1 >= picture["frames"]:
            parser.error("frame index has no following callback")
        lo, hi = marks[index] - 33334000, marks[index + 1] + 33334000
        gap = picture["frameTimes"][index + 1] - picture["frameTimes"][index]
        print("# animation timestamp gap %.3f ms after frame %d" % (gap, index))
    print("# DRM coverage %d to %d ns, %d records lost" % (start, stop, lost))
    print("monotonic_ns\tsource\tevent\tdetail")
    for ns, source, name, details in sorted(rows, key=lambda row: row[0]):
        if lo <= ns <= hi:
            print(ns, source, name, json.dumps(details, sort_keys=True), sep="\t")


if __name__ == "__main__":
    main()
