"""Beside drive.py, by the manager's order of 15:12 on card 5798: at each
stall, once the driver's captures are done, read Wine's log of the prefix in
the held guest through con.in; with "The explorer process failed to start"
release the guest and remove `release` once the driver has moved on;
without it leave the guest held and say so on the card. Each stall's record
goes in its hang directory and in stalls.txt; the counts are posted after
each tenth boot. Ends when the driver ends.
usage: watcher.py RUNDIR"""
import os, re, subprocess, sys, time
from pathlib import Path

run = Path(sys.argv[1])
log = run / "drive.log"
CARD = "5798"


def now():
    return time.strftime("%H:%M:%S", time.localtime(time.time() + 3600))


def say(text):
    print("watcher: %s %s" % (now(), text), flush=True)


def post(text):
    subprocess.run(["gh", "issue", "comment", CARD, "--repo", "toksaitov/rach2", "--body", text],
                   check=False, stdout=subprocess.DEVNULL)


def lines():
    return log.read_text(errors="replace").splitlines()


def console(n):
    p = run / ("console.b%d" % n)
    return p.read_bytes().replace(b"\r", b"") if p.exists() else b""


def ask(n, command, pattern, timeout=90):
    """Type COMMAND at boot N's console and return what it printed once a
    line matches PATTERN."""
    start = len(console(n))
    tmp = run / "con.in.tmp"
    tmp.write_text(command + "\n")
    os.rename(tmp, run / "con.in")
    end = time.time() + timeout
    while time.time() < end:
        text = console(n)[start:]
        if re.search(pattern.encode(), text, re.M):
            return text.decode(errors="replace")
        time.sleep(1)
    return None


def stop_reads(all_lines, n):
    last = None
    for line in all_lines:
        m = re.search(r"boot %d \S+: .*reads \((\d+), (\d+)\)" % n, line)
        if m:
            last = (int(m.group(1)), int(m.group(2)))
    return last


def release_until_moved(n):
    rel = run / "release"
    rel.touch()
    say("boot %d: released" % n)
    while not any(l.startswith("drive:") and "RESULT boot %d " % n in l for l in lines()):
        time.sleep(2)
    rel.unlink()
    if rel.exists():
        say("boot %d: release could not be removed" % n)
    else:
        say("boot %d: the driver moved on; release removed" % n)


stalls = {}
done_boots = set()
posted = set()
handled = {2}
stalls[2] = {"step": "2MB/s", "why": "",
             "record": "boot 2 step 2MB/s reads-at-stop 9176 ops 492 MB explorer-line present"}
if (run / "release").exists():
    (run / "release").unlink()
say("start")
while True:
    all_lines = lines()
    for line in all_lines:
        m = re.search(r"capture: boot (\d+) step (\S+): (.*) into", line)
        if m:
            stalls.setdefault(int(m.group(1)), {"step": m.group(2), "why": m.group(3)})
        m = re.search(r"RESULT boot (\d+) step (\S+): (\S+)", line)
        if m:
            done_boots.add(int(m.group(1)))
    # A stall is ready once the "capture: done" after its capture line is in the log.
    ready = set()
    current = None
    for line in all_lines:
        m = re.search(r"capture: boot (\d+) ", line)
        if m:
            current = int(m.group(1))
        if "capture: done" in line and current is not None:
            ready.add(current)
    for n in sorted(ready - handled):
        handled.add(n)
        s = stalls[n]
        reads = stop_reads(all_lines, n)
        out = ask(n, "w=$(ls -td /tmp/one.*/ 2>/dev/null | head -n 1); grep -h 'err:' \"$w\"logs/*.log "
                     "2>&1; echo EXPL$(grep -c 'The explorer process failed to start' \"$w\"logs/*.log"
                     " 2>/dev/null)\"\"END", r"EXPL\d*END")
        if out is None:
            present = None
        else:
            m = re.search(r"EXPL(\d*)END", out)
            present = bool(m and m.group(1) and int(m.group(1)) > 0)
        hdir = run / ("hang-b%d" % n)
        hdir.mkdir(exist_ok=True)
        (hdir / "wine-err.txt").write_text(out or "no answer from the guest's shell\n")
        rec = "boot %d step %s reads-at-stop %s explorer-line %s" % (
            n, s["step"], "%d ops %d MB" % (reads[0], reads[1] // 1000000) if reads else "?",
            {True: "present", False: "absent", None: "unknown (no answer)"}[present])
        (hdir / "record").write_text(rec + "\n")
        with open(run / "stalls.txt", "a") as f:
            f.write(rec + "\n")
        s["record"] = rec
        say(rec)
        if present:
            release_until_moved(n)
        else:
            post("Loop s: the stall at boot %d (%s) is held: Wine's log of its prefix %s "
                 "\"The explorer process failed to start\". %s. Captures in "
                 "build/runs/t5798/force/hang-b%d; the driver holds it up to 2 hours." %
                 (n, s["step"], "lacks" if present is False else "could not be read for",
                  rec, n))
    all_lines = lines()
    for n in sorted(done_boots):
        if n % 10 == 0 and n not in posted:
            posted.add(n)
            recs = [stalls[k].get("record", "boot %d step %s (record pending)" % (k, stalls[k]["step"]))
                    for k in sorted(stalls)]
            with_line = sum(1 for r in recs if "present" in r)
            post("Loop s, %s (UTC+6), stalls after %d boots: %d, %d with \"The explorer process "
                 "failed to start\" in Wine's log, %d without.\n\n%s" %
                 (now(), n, len(recs), with_line, len(recs) - with_line,
                  "\n".join("* " + r for r in recs) or "* none"))
    if any(" end: " in l for l in all_lines):
        say("the driver ended; stop")
        break
    time.sleep(3)
