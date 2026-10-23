#!/usr/bin/env python3
"""Report test runs, their admission waits and stalled work, and the claims
of etude's ledger, from a tree on the Linux testing host."""

import datetime
import hashlib
import os
from pathlib import Path
import pwd
import shlex
import sys
import time

ETUDE_STATUS = "etude-status 1"
ETUDE_LEDGER = "etude-ledger 1"


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


def utc(us):
    """Format microseconds since the epoch as UTC, or unknown."""
    try:
        return datetime.datetime.fromtimestamp(int(us) / 1e6,
                                               datetime.timezone.utc).strftime("%F %T UTC")
    except (ValueError, OverflowError, OSError):
        return "unknown"


def secs(us):
    """Give microseconds as whole seconds, rounded up."""
    return (max(0, int(us)) + 999999) // 1000000


def owner(words):
    """Check a process recorded by etude as its PID, start and known flag."""
    if len(words) != 3 or not words[0].isdigit():
        return "?", "unknown"
    if words[2] == "1":
        return words[0], process(words[0], None, words[1])
    state = process(words[0])
    return words[0], "unknown" if state == "alive" else state


def etude_record(path):
    """Read an etude status record, or give None for a file that is not one."""
    try:
        lines = path.read_text().splitlines()
    except FileNotFoundError:
        return None
    if not lines or lines[0] != ETUDE_STATUS:
        return None
    record = {"wait": [], "attempt": []}
    for line in lines[1:]:
        key, _, rest = line.partition(" ")
        if key in ("wait", "attempt"):
            record[key].append(rest.split())
        else:
            record[key] = rest
    return record


def ledger(directory):
    """Read a ledger's claims and requests by identity, changing nothing."""
    try:
        text = (Path(directory) / "claims").read_text()
    except FileNotFoundError:
        return {}
    lines = text.split("\n")
    if lines[0] != ETUDE_LEDGER or lines[-1] != "":
        raise ValueError(f"{directory}/claims is not a whole ledger of {ETUDE_LEDGER}")
    table, claim = {}, None
    for line in lines[1:-1]:
        words = line.split(" ")
        if claim is None and len(words) == 2 and words[0] == "claim":
            claim = {"id": words[1], "resource": [], "process": []}
        elif claim is not None and words == ["end"]:
            table[claim["id"]] = claim
            claim = None
        elif claim is not None and words[0] in ("resource", "process"):
            claim[words[0]].append(words[1:])
        elif claim is not None and len(words) >= 2:
            claim[words[0]] = words[1:] if words[0] == "owner" else words[1]
        else:
            raise ValueError(f"{directory}/claims: a line that does not belong: {line}")
    if claim is not None:
        raise ValueError(f"{directory}/claims: claim {claim['id']} has no end")
    return table


def boot_id():
    """Give this boot's identity, as the ledger records it."""
    try:
        return Path("/proc/sys/kernel/random/boot_id").read_text().strip() or "-"
    except OSError:
        return "-"


def group_runs(pgid):
    """Say whether a process group may have a member."""
    try:
        os.killpg(int(pgid), 0)
    except ProcessLookupError:
        return False
    except (OSError, ValueError):
        return True
    return True


def confirmed(directory, ident):
    """Say whether a claim's keeper wrote that its cleanup was confirmed."""
    name = hashlib.sha256(ident.encode()).hexdigest()[:32]
    try:
        return (Path(directory) / "verdicts" / name).read_bytes() == b"confirmed\n"
    except OSError:
        return False


def judge(claim, directory, boot):
    """Judge a claim as etude's recovery lists it: give owned, live or
    uncertain with the reason, or None for a claim the next admission frees."""
    if claim.get("system") != "linux":
        return "uncertain", "another operating system recorded it"
    if "-" not in (claim.get("boot", "-"), boot) and claim.get("boot") != boot:
        return None
    state = owner(claim.get("owner", []))[1]
    if state == "alive":
        return "owned", ""
    if state == "unknown":
        return "uncertain", "its owner cannot be identified"
    if claim.get("state") in ("reserved", "waiting", "blocked"):
        return None
    states = [("alive" if words[0] == "case" and group_runs(words[1]) else owner(words[1:])[1])
              for words in claim["process"]]
    if "alive" in states:
        return "live", "its owner has ended and a process of it runs"
    if "unknown" in states:
        return "uncertain", "a process of it cannot be identified"
    if claim.get("state") == "uncertain":
        return "uncertain", "its owner found the cleanup unconfirmed"
    if not confirmed(directory, claim["id"]):
        return "uncertain", "its keeper did not confirm the cleanup"
    return None


def asks(claim):
    """Describe the resources a claim or a request asks for."""
    return ", ".join(" ".join(words) for words in claim.get("resource", [])) or "nothing"


def place(case, arch):
    """Name a case on its architecture."""
    return case if arch == "none" else f"{case} on {arch}"


def waiting(words, table, now):
    """Print an attempt that waits for admission, and its place in the ledger."""
    if len(words) != 4 or not words[3].isdigit():
        print(f"    unreadable wait: {' '.join(words)}")
        return
    ident, case, arch, since = words
    claim = table.get(ident)
    protected = next((c["id"] for c in table.values() if c.get("state") == "blocked"), None)
    if claim is None:
        how = "no request of it is in the ledger"
    elif claim.get("state") == "blocked":
        how = f"it is the protected request, for {asks(claim)}"
    elif claim.get("state") == "waiting":
        how = f"behind the protected request {protected}" if protected else "no request is protected"
        how += f", for {asks(claim)}"
    else:
        how = "it was admitted after the record was written"
    print(f"    waits for admission: {place(case, arch)} for {secs(now - int(since))}s, "
          f"since {utc(since)}; {how}")


def attempt(words, now):
    """Print a running attempt as working within its bounds, or stalled."""
    if len(words) != 9 or not words[6].isdigit() or not words[8].isdigit():
        print(f"    unreadable attempt: {' '.join(words)}")
        return
    ident, case, arch, stage, state, kind, progress, deadline, bound = words
    where = f"{place(case, arch)} in {stage}"
    quiet = (f"last progress {secs(now - int(progress))}s ago" if stage == "execute"
             else f"in {stage} for {secs(now - int(progress))}s")
    if state == "expired":
        print(f"    stalled: {where}: {quiet}; its {kind} bound of {secs(bound)}s ran out, "
              f"and attempt {ident} is being ended")
    elif not deadline.isdigit():
        print(f"    unreadable attempt: {' '.join(words)}")
    elif now <= int(deadline):
        print(f"    working: {where}: {quiet}; its {kind} bound of {secs(bound)}s "
              f"runs out in {secs(int(deadline) - now)}s")
    else:
        print(f"    stalled: {where}: {quiet}; its {kind} bound of {secs(bound)}s ran out "
              f"{secs(now - int(deadline))}s ago; fault: the run has not ended attempt {ident}")


def etude(record, ledgers, now):
    """Print an etude run: its owner, and what each attempt waits for or does."""
    pid, state = owner(record.get("owner", "").split())
    written = record.get("written", "")
    age = f"{secs(now - int(written))}s ago" if written.isdigit() else "at an unknown time"
    fault = "; fault: stale run record, inspect its ledger" if state.startswith("dead") else ""
    print(f"  etude run {pid} {state}: {record.get('args', '?')} "
          f"(run {record.get('run', '?')}, status written {age}){fault}")
    directory = record.get("ledger")
    try:
        table = ledger(directory) if directory else {}
    except (OSError, ValueError):
        table = {}
    if directory and directory not in ledgers:
        ledgers.append(directory)
    for words in record["wait"]:
        waiting(words, table, now)
    for words in record["attempt"]:
        attempt(words, now)
    if not record["wait"] and not record["attempt"]:
        print("    no attempt waits or runs")


def machine_ledger(repo):
    """Give the ledger that the machine record of the tree's owner names."""
    try:
        account = pwd.getpwuid(Path(repo).stat().st_uid)
    except (KeyError, OSError):
        return None
    state = os.environ.get("XDG_STATE_HOME", "") if account.pw_uid == os.getuid() else ""
    base = Path(state) if state.startswith("/") else Path(account.pw_dir) / ".local" / "state"
    return value(fields(base / "etude" / "machine"), "ledger", "") or None


def claims(directory):
    """Print a ledger's requests, and its claims that no running owner keeps."""
    try:
        table = ledger(directory)
    except (OSError, ValueError) as error:
        print(f"  ledger {directory}: unreadable: {error}; fault: no admission can be judged")
        return
    boot, owned, rows = boot_id(), 0, []
    for claim in table.values():
        verdict = judge(claim, directory, boot)
        request = claim.get("state") in ("waiting", "blocked")
        if verdict is None:
            continue
        if verdict[0] == "owned" and not request:
            owned += 1
        elif verdict[0] == "owned":
            role = "protected" if claim.get("state") == "blocked" else "waiting"
            rows.append(f"    request {claim['id']} of {claim.get('account', '?')}: {role}, "
                        f"for {asks(claim)}")
        else:
            drop = (f"build/tools/etude/etude release {shlex.quote(directory)} "
                    f"{shlex.quote(claim['id'])}")
            fault = ("end its processes" if verdict[0] == "live"
                     else "a person must establish that nothing of it runs and drop it "
                          f"with {drop}")
            rows.append(f"    claim {claim['id']}: {verdict[0]}: {verdict[1]}; "
                        f"holds {asks(claim)}; fault: {fault}")
    print(f"  ledger {directory}: {owned} claims owned by running workers")
    for row in rows:
        print(row)


def report(repo):
    """Report each recorded run and any tree lock whose owner has ended."""
    if not Path("/proc/locks").exists():
        print("  test status unavailable: Linux testing host required")
        return
    build = Path(repo) / "build"
    found = False
    ledgers = []
    now = int(time.time() * 1e6)
    for path in sorted(build.glob(".run.status.*")):
        if path.name.endswith(".new"):
            continue
        record = etude_record(path)
        if record is not None:
            found = True
            etude(record, ledgers, now)
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
        if value(owner, "identity", None) is not None:
            pid, state = identity(value(owner, "identity").split())
        else:
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
    machine = machine_ledger(repo)
    if machine and machine not in ledgers:
        ledgers.append(machine)
    for directory in ledgers:
        claims(directory)


if __name__ == "__main__":
    try:
        report(sys.argv[1])
    except (OSError, ValueError) as error:
        print(f"  test status unavailable: {error}")
        sys.exit(1)
