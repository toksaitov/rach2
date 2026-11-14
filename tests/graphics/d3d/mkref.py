#!/usr/bin/env python3
"""Fold the rows that reference.sh recorded into a reference for run.py.

usage: mkref.py [-s UNIT]... [-a TEST=CHECK]... SOURCE OUT.json DIR...

Each DIR is a run of reference.sh; each of its numbered directories is one
row of run.py -R, with its environment.json and results.json. A unit that
several runs recorded keeps every failure count they gave, the fewest checks
and the most skips, as linux.json keeps the discovery's runs. A Wine unit
that ends before its summary keeps the fewest test lines it printed, and its
exit only when every run gave the same one, since a unit that crashes ends
with an exit that varies from run to run. SOURCE is the reference's own
description of where it was taken.

-s names a unit, PATH/EXE:NAME of Wine's or a vkd3d-proton test, that its
author saw stall in Linux's runs, its process alive and idle, and stopped:
the reference stops it where it stalled, at the last test line its log has,
counted as often as the log has it, as linux.json stops WineD3D's DXGI unit.

-a names a vkd3d-proton test whose failure count differs from one of Linux's
runs to the next because its check reads the address at which mmap put the
driver's memory, and the text that each failure of that check prints: the
reference's "placement" keeps it, and run.py accepts any count of failures
that all print it, as test_suballocate_va_alignment's "Expected VA to be
aligned" on lavapipe and Venus.
"""
import getopt
import json
from pathlib import Path
import re
import signal
import sys

STALL = "dxgi.c:8040, d3d10 test 2, unexpected present count 1, expected 2; stop on this record"
SEGV = (-signal.SIGSEGV, 128 + signal.SIGSEGV)
TEST_LINE = r"^\w+\.c:\d+: "
VK_LINE = r"^test_\w+:\d+: "


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


def stall(log, expression):
    """Return a stalled unit's last test line and its count in the log."""
    text = log.read_text(errors="replace")
    lines = [l for l in text.splitlines() if re.match(expression, l)]
    if not lines:
        fail("%s: a stall with no test line to stop at" % log)
    return {"line": lines[-1], "count": lines.count(lines[-1])}


def merge_stall(old, new, unit):
    """Keep one stall line for a unit; every run must have stalled on it."""
    if old is None:
        return new
    if "progress" not in old or old["progress"]["line"] != new["progress"]["line"]:
        fail("%s stalled in one run where another did otherwise" % unit)
    return {"progress": {"line": new["progress"]["line"],
                         "count": min(old["progress"]["count"], new["progress"]["count"])}}


def main():
    """Read every recorded row and write the reference whole."""
    try:
        opts, args = getopt.getopt(sys.argv[1:], "s:a:")
    except getopt.GetoptError:
        opts, args = [], []
    if len(args) < 3:
        fail("usage: mkref.py [-s UNIT]... [-a TEST=CHECK]... SOURCE OUT.json DIR...")
    stalled = {value for opt, value in opts if opt == "-s"}
    placement = {}
    for opt, value in opts:
        if opt == "-a":
            name, _, check = value.partition("=")
            if not name or not check:
                fail("-a takes TEST=CHECK, not %s" % value)
            placement[name] = check
    source, output, dirs = args[0], Path(args[1]), args[2:]
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
                # A probe's row is [PATH/]N[-MODE]; its token names N alone.
                n = row.rpartition("/")[2].partition("-")[0]
                result = results[row]
                drew = result["exit"] == 0 and ("D3D%s OK" % n) in result["pixels"].splitlines()
                if not drew:
                    fail("%s: the probe %s did not draw right on Linux" % (row_dir, row))
                # An i386 probe's process, as run.py read it while it held.
                want = {None: None, "wow64": ("x86_64", 64), "i386": ("i386", 32)}[env.get("mode")]
                if want and (result.get("unix"), result.get("elf")) != want:
                    fail("%s: the probe %s ran in a %s-bit process of the %s Wine, not as its mode asks" % (
                        row_dir, row, result.get("elf"), result.get("unix")))
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
                if row in stalled:
                    units[unit] = merge_stall(old, {"progress": stall(row_dir / "unit.log", TEST_LINE)}, row)
                    continue
                if old is not None and isinstance(old.get("progress"), dict):
                    fail("%s: %s stalled in one run and not in another" % (row_dir, row))
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
                    if name in stalled:
                        ref["vkd3d"][name] = merge_stall(ref["vkd3d"].get(name),
                                                         {"progress": stall(row_dir / (name + ".log"), VK_LINE)}, name)
                        continue
                    if "progress" in ref["vkd3d"].get(name, {}):
                        fail("%s: %s stalled in one run and not in another" % (row_dir, name))
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
    for name in placement:
        if name not in ref["vkd3d"]:
            fail("-a names %s, which the records lack" % name)
    ref["placement"] = dict(sorted(placement.items()))
    output.write_text(json.dumps(ref, indent=2) + "\n")
    print("mkref: result: %d probes, %d Wine units, %d vkd3d tests, %d flakes in %s" % (
        len(ref["probe"]), sum(len(u) for u in ref["wine"].values()), len(ref["vkd3d"]),
        len(ref["flakes"]), output))


if __name__ == "__main__":
    main()
