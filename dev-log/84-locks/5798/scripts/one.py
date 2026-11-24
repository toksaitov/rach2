"""Make one fresh prefix under a Weston of its own, at the host's go.
usage: one.py OUT GO
Start Weston as run.py does and print ONE WESTON; wait for the file GO,
which root makes when the host has set the disk's throttle; then make the
prefix as make_prefix does, print ONE END with its status and time, wait for
the Wine server and print ONE DONE."""
import os, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, "/tmp/games")
import run


def say(text):
    print("ONE " + text, flush=True)


out, go = Path(sys.argv[1]), Path(sys.argv[2])
out.mkdir(parents=True)
work = Path(tempfile.mkdtemp(prefix="one.", dir="/tmp"))
r = run.Runner(out, work)
r.start_weston()
say("WESTON %s" % r.env["DISPLAY"])
while not go.exists():
    time.sleep(0.1)
t0 = time.time()
say("START")
proc = r.launch("prefix", ["c:\\windows\\system32\\cmd.exe", "/c", "exit", "0"], wait=False)
status = proc.wait()
took = time.time() - t0
text = (out / "prefix.log").read_text(errors="replace")
made = "rach2-game: prefix: made" in text
say("END status %d made %s in %.1f s" % (status, made, took))
if status != 0 or not made:
    for line in text.splitlines()[-20:]:
        say("LOG " + line)
r.server_wait()
say("DONE in %.1f s" % (time.time() - t0))
r.stop()
