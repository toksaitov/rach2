#!/usr/bin/env python3
"""t5804: the final table from each run's results.jsonl line, blk.log and
one.log: time to the prefix's end, reads (where they stopped under the
throttle, and in all), explorer.exe's fate and the explorer line."""
import json
import re
import sys
from pathlib import Path

top = Path(sys.argv[1])
rows = [json.loads(l) for l in (top / "results.jsonl").read_text().splitlines() if l.strip()]
print("| run | step | go (UTC+6) | prefix's end | reads: where they stopped under the throttle | "
      "reads in all | explorer.exe | \"explorer process failed to start\" |")
print("|---|---|---|---|---|---|---|---|")
for r in rows:
    d = top / ("%02d-%s" % (r["run"], r["step"]))
    blk = [tuple(map(int, l.split())) for l in (d / "blk.log").read_text().splitlines()]
    throttled = [b for b in blk if b[0] < r.get("full_at", 1e9) - 5]
    stop = None
    for i, b in enumerate(throttled):
        if all(x[1] == b[1] for x in throttled[i:]) and i + 1 < len(throttled):
            stop = b
            break
    seen = {}
    for line in (d / "one.log").read_text(errors="replace").splitlines():
        m = re.match(r"T (\d+) explorer (\S+)", line)
        if m and m.group(2) != "none":
            for pid in m.group(2).split(","):
                seen.setdefault(pid, [int(m.group(1)), int(m.group(1))])[1] = int(m.group(1))
    if r.get("stall"):
        end = "none: stall (throttle off at %.0f s, no end 120 s later)" % r["full_at"]
        fate = ("; ".join("pid %s seen %d-%d s, then gone" % (p, a, b) for p, (a, b) in seen.items())
                or "never seen") + "; none at the stall"
    else:
        end = "made %s s, exit %s s" % (r["made_s"], r["end_s"])
        fate = "seen %s; alive at end %s" % (r["explorer_seen"], r["explorer_alive_at_end"])
    where = ("%d / %d MB by %d s" % (stop[1], stop[2] / 1e6, stop[0]) if stop else "did not stop")
    print("| %d | %s | %s | %s | %s | %d / %d MB | %s | %s |" % (
        r["run"], {"bps": "2 MB/s", "iops": "50 iops"}[r["step"]], r["go"], end, where,
        r["reads"], r["read_mb"], fate, "yes" if r.get("failmsg") else "no"))
