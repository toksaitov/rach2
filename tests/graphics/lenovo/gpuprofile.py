"""Read the GPU thread's profile copies that stall.py's bare runs keep on Rach2.

Usage: python3 gpuprofile.py RUN [LOW HIGH OUT]

RUN is a run's directory, such as OUT/run-0, holding bkl_gpu_profile (or
bkl_gpu_profile.gz) and browser/result.json. Each copy in the file is the
kernel's profile of the GPU process's main thread since its start, after a
line with the epoch's time and the thread's utime, stime and minflt. The
script prints the page's first frames' entries and, for each copy, what
the thread gained since the one before: user and system ticks of 10 ms,
minor faults and kernel samples of 1 ms. With LOW and HIGH, seconds after
the first copy, it writes to OUT the samples gained between the copies
nearest them, in bkl/profile's format, which tests/system/games/bklread.sh
names with the build's kernel.debug, and prints their entries.
"""
from collections import Counter
import gzip
import json
from pathlib import Path
import sys


def copies(path):
    """Return the copies in PATH, each a dict of its time, times and samples."""
    opener = gzip.open if path.suffix == ".gz" else open
    found, cur = [], None
    with opener(path, "rt") as text:
        for line in text:
            if line.startswith("# snapshot"):
                w = line.split()
                cur = {"t": float(w[2]), "u": int(w[4]), "s": int(w[6]), "f": int(w[8]),
                       "n": 0, "c": Counter()}
                found.append(cur)
            elif cur is None or line.startswith("#"):
                continue
            elif line.startswith("samples"):
                cur["n"] = int(line.split()[1])
            elif not line.startswith("lost"):
                w = line.split()
                cur["c"][tuple(w[:-1])] += int(w[-1])
    return found


def main():
    """Print the copies' gains and write the window's samples when asked."""
    run = Path(sys.argv[1])
    path = run / "bkl_gpu_profile"
    if not path.exists():
        path = run / "bkl_gpu_profile.gz"
    snaps = copies(path)
    base = snaps[0]["t"]
    picture = json.loads((run / "browser/result.json").read_text())["picture"]
    origin = (picture["timeOrigin"] + picture["start"]) / 1000.0
    print("frames 0 to 7 entered at", [round(origin + s / 1000.0 - base, 3)
                                       for s in picture["frameStarts"][:8]], "s")
    for a, b in zip(snaps, snaps[1:]):
        print("%7.3f s: user %+d system %+d faults %+d samples %+d" % (
            b["t"] - base, b["u"] - a["u"], b["s"] - a["s"], b["f"] - a["f"], b["n"] - a["n"]))
    if len(sys.argv) < 5:
        return
    low, high = float(sys.argv[2]), float(sys.argv[3])
    a = min(snaps, key=lambda s: abs(s["t"] - base - low))
    b = min(snaps, key=lambda s: abs(s["t"] - base - high))
    gained = Counter({k: v - a["c"][k] for k, v in b["c"].items() if v > a["c"][k]})
    with open(sys.argv[4], "w") as out:
        out.write("samples %d\nlost 0\n" % sum(gained.values()))
        for k, v in gained.items():
            out.write(" ".join(k) + " %d\n" % v)
    entries = Counter()
    for k, v in gained.items():
        entries[" ".join(k[:2])] += v
    print("%.3f to %.3f s: %d samples, user %d system %d faults %d" % (
        a["t"] - base, b["t"] - base, sum(gained.values()), b["u"] - a["u"], b["s"] - a["s"],
        b["f"] - a["f"]))
    for name, count in entries.most_common():
        print("%5d %s" % (count, name))


if __name__ == "__main__":
    main()
