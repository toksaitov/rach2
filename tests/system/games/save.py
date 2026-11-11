#!/usr/bin/env python3
"""
Write a save and a prefix's registry value on the data partition through
Wine, or, in a later boot of a new root, load them through that prefix and
through a new one: the save-update family's two halves.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import sys
import tempfile

from run import Runner, interrupted, say, show_logs

# The data partition's directory the root's administrator gives the account,
# as /data/crysis is given for Crysis, and what the rows keep there.
BASE = Path("/data/games")
SAVE = BASE / "saves" / "slot1.sav"
STATE = BASE / "state.json"
OLD = BASE / "compat"
NEW = BASE / "compat-new"
KEY = "HKCU\\Software\\Rach2"
SAVE_BYTES = 4 << 20
REG = "c:\\windows\\system32\\reg.exe"
CMD = "c:\\windows\\system32\\cmd.exe"


def dos(path):
    """Return PATH, an absolute path, as Wine's drive Z: names it."""
    return "Z:" + str(path).replace("/", "\\")


def payload():
    """Return the save's bytes: SAVE_BYTES of a SHA-256 chain from a fixed
    seed, so that every run writes the same save."""
    out, block = bytearray(), hashlib.sha256(b"rach2 save-update").digest()
    while len(out) < SAVE_BYTES:
        block = hashlib.sha256(block).digest()
        out += block
    return bytes(out[:SAVE_BYTES])


def digest(path):
    """Return the SHA-256 of the file at PATH in hex."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def marked(runner, name, value):
    """Return whether KEY's value Save in the runner's prefix is VALUE, as
    NAME, through reg.exe's export to a file: what reg.exe prints does not
    reach the launcher's output through Proton's steam.exe. The export is
    UTF-16 with a byte-order mark; a key that is missing exports nothing."""
    export = runner.work / (name + ".reg")
    status = runner.launch(name, [REG, "export", KEY, dos(export), "/y"])
    text = export.read_bytes().decode("utf-16", errors="replace") if export.exists() else ""
    (runner.out / (name + ".reg.txt")).write_text(text)
    have = '"Save"="%s"' % value in text
    say("%s: reg export exit %d value %s" % (name, status, have))
    return status == 0 and have


def loaded(runner, name, want):
    """Copy the save out of the data partition through Wine's cmd in the
    runner's prefix as NAME; return whether the copy's SHA-256 is WANT."""
    back = runner.work / (name + ".sav")
    status = runner.launch(name, [CMD, "/c", "copy", "/b", dos(SAVE), dos(back)])
    have = digest(back) if back.exists() else None
    say("%s: copy exit %d sha256 %s want %s" % (name, status, have, want))
    return status == 0 and have == want


def row_save(runner):
    """Make the prefix on the data partition, write the save through Wine,
    and record its SHA-256 in KEY of the prefix and in STATE."""
    data = payload()
    want = hashlib.sha256(data).hexdigest()
    source = runner.work / "save.src"
    source.write_bytes(data)
    SAVE.parent.mkdir(parents=True)
    runner.prefix = OLD
    runner.make_prefix()
    status = runner.launch("write", [CMD, "/c", "copy", "/b", dos(source), dos(SAVE)])
    have = digest(SAVE) if SAVE.exists() else None
    say("write: copy exit %d sha256 %s want %s" % (status, have, want))
    mark = runner.launch("mark", [REG, "add", KEY, "/v", "Save", "/t", "REG_SZ", "/d", want,
                                  "/f"])
    runner.server_wait()
    say("mark: reg add exit %d" % mark)
    STATE.write_text(json.dumps({"sha256": want}) + "\n")
    os.sync()
    return status == 0 and have == want and mark == 0 and marked(runner, "check", want)


def row_load(runner):
    """In a new root, load the save through the existing prefix, which the
    launcher must not make again and whose KEY must still hold the save's
    SHA-256, and through a new prefix, which the launcher makes and whose
    KEY must not have it. Each launch waits for its Wine server to end
    (Runner.launch), so the next prefix's server starts alone."""
    want = json.loads(STATE.read_text())["sha256"]
    say("load: state sha256 %s, save %s bytes" % (want, SAVE.stat().st_size))
    runner.prefix = OLD
    old_reg = marked(runner, "old-reg", want)
    again = "rach2-game: prefix: made" in (runner.out / "old-reg.log").read_text(errors="replace")
    say("old-reg: prefix made again %s" % again)
    old_save = loaded(runner, "old-load", want)
    runner.prefix = NEW
    runner.make_prefix()
    new_save = loaded(runner, "new-load", want)
    new_reg = marked(runner, "new-reg", want)
    say("load: old prefix value %s save %s; new prefix save %s value %s" % (
        old_reg, old_save, new_save, new_reg))
    return old_reg and not again and old_save and new_save and not new_reg


def main():
    """Run the row save or load with its logs in OUT."""
    if len(sys.argv) != 3 or sys.argv[1] not in ("save", "load"):
        raise SystemExit("usage: save.py save|load OUT")
    row, output = sys.argv[1:]
    if os.geteuid() == 0:
        raise SystemExit("games: run as an ordinary user")
    out = Path(output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    work = Path(tempfile.mkdtemp(prefix="rach2-save.", dir=os.environ.get("TMPDIR", "/tmp")))
    runner = None
    try:
        runner = Runner(out, work)
        runner.start_weston()
        ok = row_save(runner) if row == "save" else row_load(runner)
        if not ok:
            show_logs(out)
            say("result: %s differs; see %s" % (row, out))
            return 1
        print("SAVEROW OK %s" % row, flush=True)
        return 0
    except BaseException:
        show_logs(out)
        raise
    finally:
        if runner is not None:
            runner.stop()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    for stop_signal in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(stop_signal, interrupted)
    sys.exit(main())
