#!/usr/bin/env python3
"""Prepare and verify the sessions of the Claude Code rows on the host.

Usage: claude_session_verify.py --script SOURCE DESTINATION ROW
       claude_session_verify.py --watch RECORD TIMINGS
       claude_session_verify.py ARCHIVE RECORD VERSION

--script writes to DESTINATION the script SOURCE, claude-session.json, with
the repository where the row ROW makes it, and with claude-screen.json's steps
first for claude-screen; for claude-print its last step is held, so that the
row inspects the program while it waits for the last answer. --watch is
muse_verify.py's observer of the record. The third form judges a row's archive
as claude_test.py judges the Linux proof: the file, the commit and every
recorded request, each against its step. It also requires Anthropic's program
before and after the session and Messages requests from Claude Code of
VERSION, and prints the times of the row's steps and the free frames it
sampled.
"""

import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "fakeapi"))

import claude_test  # noqa: E402
from muse_verify import watch  # noqa: E402


def prepare_script(source, destination, row):
    """Render the session's script for the row's repository."""
    if Path(source).resolve() != claude_test.HERE / "claude-session.json":
        raise ValueError(f"{source} is not claude-session.json")
    script = claude_test.render(f"/tmp/agents/work/{row}/session/repo", row == "claude-screen")
    if row == "claude-print":
        script["steps"][-1]["hold"] = True
    Path(destination).write_text(json.dumps(script))


def verify(archive, record, version):
    """Judge the returned session and print what the row measured."""
    with tempfile.TemporaryDirectory(prefix="rach2-claude-session-verify.") as name:
        work = Path(name)
        with tarfile.open(archive) as source:
            source.extractall(work, filter="data")
        mode = (work / "mode").read_text().strip()
        if mode not in ("print", "screen"):
            raise ValueError(f"unknown mode {mode}")
        at = (work / "at").read_text().strip()
        (work / "home").mkdir()
        (work / "script.json").write_text(json.dumps(claude_test.render(at, mode == "screen")))
        claude_test.verify(work, Path(record))
        for binary in ("binary.before", "binary.after"):
            if (work / binary).read_text().split()[0] != claude_test.DIGEST:
                raise ValueError(f"{binary}: not Anthropic's program")
        entries = [json.loads(line) for line in Path(record).read_text().splitlines()]
        for entry in entries:
            headers = {key.lower(): value for key, value in entry["headers"]}
            if entry["path"] == "/v1/messages" and \
                    not headers.get("user-agent", "").startswith(f"claude-cli/{version} "):
                raise ValueError(f"request {entry['n']}: the user agent "
                                 f"{headers.get('user-agent')}")
        for line in (work / "steps").read_text().splitlines():
            seconds, what = line.split(" ", 1)
            print(f"claude-session-verify: {what} after {seconds} s")
        frames = [int(line) for line in (work / "memory").read_text().splitlines()]
        if not frames:
            raise ValueError("no sample of the free frames")
        seconds = int((work / "seconds").read_text())
        print(f"claude-session-verify: measurement: session {seconds} s; free frames {frames[0]} "
              f"at the start, {min(frames)} at the least ({min(frames) * 4} KiB); "
              f"the session took {(frames[0] - min(frames)) * 4} KiB")
    print(f"claude-session-verify: pass: {len(entries)} requests, and the edited fixture committed "
          f"in a clean repository")


def main(args):
    """Dispatch the command line."""
    if args[0] == "--script":
        prepare_script(*args[1:])
    elif args[0] == "--watch":
        watch(*args[1:])
    else:
        verify(*args)


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (OSError, ValueError, KeyError, tarfile.TarError,
            subprocess.CalledProcessError) as error:
        sys.exit(f"claude-session-verify: fail: {error}")
