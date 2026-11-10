#!/usr/bin/env python3
"""Fold the rows that reference.sh recorded into a reference for run.py.

usage: mkref.py SOURCE OUT.json DIR...

Each DIR is a run of reference.sh; each of its numbered directories is one
row of run.py -R, with its environment.json and results.json. A unit that
several runs recorded keeps every failure count they gave, the fewest checks
and the most skips, as linux.json keeps the discovery's runs. A Wine unit
that ends before its summary keeps the fewest test lines it printed, and its
exit only when every run gave the same one, since a unit that crashes ends
with an exit that varies from run to run. SOURCE is the reference's own
description of where it was taken.
"""
import json
from pathlib import Path
import re
import signal
import sys

STALL = "dxgi.c:8040, d3d10 test 2, unexpected present count 1, expected 2; stop on this record"
SEGV = (-signal.SIGSEGV, 128 + signal.SIGSEGV)
TEST_LINE = r"^\w+\.c:\d+: "


def fail(text):
    """Stop with a diagnostic naming what the records lack."""
    raise SystemExit("mkref: " + text)


def merge_counts(old, failures, checks, skipped):
    """Return a unit's expectation with one more run's counts folded in."""
    if old is None:
        return {"failures": [failures], "min_checks": checks, "max_skipped": skipped}
    return {"failures": sorted(set(old["failures"]) | {failures}),
            "min_checks": min(old["min_checks"], checks),
            "max_skipped": max(old["max_skipped"], skipped)}


def main():
    """Read every recorded row and write the reference whole."""
    if len(sys.argv) < 4:
        fail("usage: mkref.py SOURCE OUT.json DIR...")
    source, output, dirs = sys.argv[1], Path(sys.argv[2]), sys.argv[3:]
    ref = {"source": source, "probe": {}, "wine": {}, "vkd3d": {}, "flakes": []}
    adapters = set()
    for top in dirs:
        for row_dir in sorted(p for p in Path(top).iterdir() if p.is_dir()):
            env = json.loads((row_dir / "environment.json").read_text())
            if env.get("reference") is not None:
                fail("%s was judged against a reference, not recorded" % row_dir)
            adapters.add(env["adapter"])
            results_file = row_dir / "results.json"
            if not results_file.exists():
                fail("%s has no results.json" % row_dir)
            results = json.loads(results_file.read_text())
            family, row = env["family"], env["row"]
            if family == "probe":
                path, _, n = row.rpartition("/")
                result = results[row]
                drew = result["exit"] == 0 and ("D3D%s OK" % n) in result["pixels"].splitlines()
                if not drew:
                    fail("%s: the probe %s did not draw right on Linux" % (row_dir, row))
                old = ref["probe"].get(row)
                if old and old["adapter"] != result["adapter"]:
                    fail("%s: the probe %s named two adapters" % (row_dir, row))
                ref["probe"][row] = {"exit": 0, "adapter": result["adapter"]}
            elif family == "wine":
                path, _, unit = row.partition("/")
                result = results[unit]
                units = ref["wine"].setdefault(path, {})
                old = units.get(unit)
                counts = result["summary"]
                if result["progress"]:
                    new = {"progress": STALL}
                elif counts is None:
                    lines = len(re.findall(TEST_LINE, (row_dir / "unit.log").read_text(errors="replace"), re.M))
                    new = {"exit": result["exit"], "summary": False, "failures": 0, "min_lines": lines}
                else:
                    new = {"exit": result["exit"], "summary": True, "failures": counts[3],
                           "min_checks": counts[0], "max_skipped": counts[4]}
                if old is not None and ("progress" in old) != ("progress" in new):
                    fail("%s: %s stalled in one run and not in another" % (row_dir, row))
                if old is not None and "progress" not in new:
                    if old["summary"] != new["summary"]:
                        fail("%s: %s ended in two ways, %s and %s" % (row_dir, row, old, new))
                    if not new["summary"]:
                        new["min_lines"] = min(old["min_lines"], new["min_lines"])
                        if old["exit"] != new["exit"]:
                            new["exit"] = None
                    elif (old["exit"], old["failures"]) != (new["exit"], new["failures"]):
                        fail("%s: %s ended in two ways, %s and %s" % (row_dir, row, old, new))
                    if new["summary"]:
                        new["min_checks"] = min(old["min_checks"], new["min_checks"])
                        new["max_skipped"] = max(old["max_skipped"], new["max_skipped"])
                units[unit] = new
            elif family == "vkd3d":
                for name, result in results.items():
                    counts = result["summary"]
                    if counts is None or not result["ended"]:
                        if result["exit"] in SEGV and result["begun"]:
                            if name not in ref["flakes"]:
                                ref["flakes"].append(name)
                            continue
                        fail("%s: %s ended with %s and no summary" % (row_dir, name, result["exit"]))
                    ref["vkd3d"][name] = merge_counts(ref["vkd3d"].get(name), counts[1], counts[0], counts[3])
            else:
                fail("%s: unknown family %s" % (row_dir, family))
    if len(adapters) != 1:
        fail("the records name the adapters %s, not one" % sorted(adapters))
    for name in ref["flakes"]:
        # A test that crashed in every run is matched by its crash alone.
        ref["vkd3d"].setdefault(name, {"failures": [], "min_checks": 0, "max_skipped": 0})
    ref["flakes"].sort()
    output.write_text(json.dumps(ref, indent=2) + "\n")
    print("mkref: result: %d probes, %d Wine units, %d vkd3d tests, %d flakes in %s" % (
        len(ref["probe"]), sum(len(u) for u in ref["wine"].values()), len(ref["vkd3d"]),
        len(ref["flakes"]), output))


if __name__ == "__main__":
    main()
