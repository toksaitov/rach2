#!/usr/bin/env python3
"""Report test admission waits from a tree on the Linux testing host."""

import datetime
import os
from pathlib import Path
import sys
import time


def fields(path):
    """Read an atomic key/value record, preserving repeated keys."""
    try:
        return [line.split(" ", 1) for line in path.read_text().splitlines() if " " in line]
    except FileNotFoundError:
        return []


def value(rows, key, default="?"):
    """Return the first value of key, or default when it is absent."""
    return next((v for k, v in rows if k == key), default)


def process(pid, group=None, start=None):
    """Describe liveness, checking the group and start time when recorded."""
    try:
        if group is not None and not str(group).isdigit():
            return "unknown"
        text = Path(f"/proc/{int(pid)}/stat").read_text()
        words = text[text.rindex(")") + 2:].split()
        if words[0] in ("Z", "X"):
            return "dead"
        if group is not None and words[2] != str(group):
            return "dead (pid reused)"
        if start is not None and words[19] != str(start):
            return "dead (pid reused)"
        return "alive"
    except FileNotFoundError:
        return "dead"
    except (OSError, ValueError, IndexError):
        return "unknown"


def identity(words):
    """Check a recorded pid, group, start time and fractional start time."""
    if len(words) != 4:
        return "?", "unknown"
    return words[0], process(*words[:3])


def since(rows):
    """Format the wait's recorded start as UTC, independent of host timezone."""
    try:
        return datetime.datetime.fromtimestamp(int(value(rows, "since")),
                                               datetime.timezone.utc).strftime("%F %T UTC")
    except (ValueError, OverflowError, OSError):
        return "unknown"


def locks():
    """Read current POSIX lock holders, omitting queued lock requests."""
    result = []
    for line in Path("/proc/locks").read_text().splitlines():
        words = line.split()
        if len(words) >= 8 and words[1] == "POSIX":
            major, minor, inode = words[5].split(":")
            result.append(((int(major, 16), int(minor, 16), int(inode)),
                           words[3], words[4]))
    return result


def claim(rows):
    """Print the actual holders of every file blocking a host or port claim."""
    print(f"    waits for {value(rows, 'reason')} since {since(rows)}")
    try:
        held = locks()
    except OSError as error:
        print(f"    holders unknown: {error}")
        return
    for key, text in rows:
        if key != "lock":
            continue
        mode, path = text.split(" ", 1)
        try:
            st = os.stat(path)
        except FileNotFoundError:
            print(f"    {path}: no holder (claim may retry)")
            continue
        file_id = (os.major(st.st_dev), os.minor(st.st_dev), st.st_ino)
        owners = [(pid, process(pid)) for file, kind, pid in held
                  if file == file_id and (mode == "WRITE" or kind == "WRITE")]
        if not owners:
            print(f"    {path}: no conflicting holder (claim may retry)")
        for pid, state in owners:
            fault = "; fault: holder ended during inspection" if state.startswith("dead") else ""
            print(f"    {path}: holder {pid} {state}{fault}")


def pool(rows):
    """Print pending suites and the jobs occupying their admission budget."""
    print(f"    waits for {value(rows, 'reason')} since {since(rows)}; "
          f"budget {value(rows, 'budget')} MiB")
    for key, text in rows:
        words = text.split()
        if key == "pending" and len(words) == 2:
            print(f"    pending {words[0]}: {words[1]} MiB")
        elif key == "holder" and len(words) >= 2:
            pid, state = identity(words[2:])
            fault = "; fault: collect the ended job" if state.startswith("dead") else ""
            print(f"    holder {words[0]} pid {pid} {state}: {words[1]} MiB{fault}")


def report(repo):
    """Report each recorded run and any tree lock whose owner has ended."""
    if not Path("/proc/locks").exists():
        print("  test status unavailable: Linux testing host required")
        return
    build = Path(repo) / "build"
    found = False
    for path in sorted(build.glob(".run.status.*")):
        if path.name.endswith(".new"):
            continue
        rows = fields(path)
        if not rows:
            continue
        found = True
        pid, state = identity(value(rows, "identity").split())
        fault = "; fault: stale run record, inspect its work" if state.startswith("dead") else ""
        print(f"  run {pid} {state}: {value(rows, 'args')}{fault}")
        work = Path(value(rows, "work"))
        waiting = fields(work / "claim.wait")
        queued = fields(work / "pool.wait")
        if waiting:
            claim(waiting)
        if queued:
            pool(queued)
        if not waiting and not queued:
            print("    no recorded admission wait")
    owner = fields(build / ".run.lock" / "owner")
    if owner:
        pid = value(owner, "pid")
        state = process(pid, value(owner, "pgid"))
        fault = "; fault: inspect the work and clear the stale tree lock" if state.startswith("dead") else ""
        print(f"  tree lock: holder {pid} {state}, since {value(owner, 'since')}, "
              f"{value(owner, 'args')}{fault}")
    elif (build / ".run.lock").exists():
        age = max(0, int(time.time() - (build / ".run.lock").stat().st_mtime))
        fault = "; fault: stale tree lock without an owner" if age >= 10 else ""
        print(f"  tree lock: holder unknown (owner record absent, age {age}s){fault}")
    if not found:
        print("  no recorded test run")


if __name__ == "__main__":
    try:
        report(sys.argv[1])
    except (OSError, ValueError) as error:
        print(f"  test status unavailable: {error}")
        sys.exit(1)
