#!/usr/bin/env python3
"""Prepare and verify the sessions of the Antigravity CLI rows on the host.

Usage: agy_session_verify.py --script SOURCE DESTINATION ROW
       agy_session_verify.py --watch RECORD TIMINGS
       agy_session_verify.py ARCHIVE RECORD VERSION

--script writes to DESTINATION the script SOURCE, agy-session.json, with the
repository where the row ROW makes it, after the steps of agy-services.json:
the requests the program sends to Google's other services, each answered with
404 as often as it comes; for agy-print the last step of the session is held,
so that the row inspects the program while it waits for the last answer.
--watch is muse_verify.py's observer of the record. The third form judges a
row's archive as agy_test.py judges the Linux proof: the file, the commit and
every recorded request, each against its step. It also requires Google's
program before and after the session, and prints the times of the row's steps,
the free frames it sampled and how often each service was asked.
"""

import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "fakeapi"))

import agy_test  # noqa: E402
from muse_verify import watch  # noqa: E402


def render(repo):
    """Return the services' steps and agy-session.json with its repository at REPO."""
    script = json.loads((agy_test.HERE / "agy-session.json").read_text().replace(
        "@REPO@", json.dumps(str(repo))[1:-1]))
    services = json.loads((agy_test.HERE / "agy-services.json").read_text())
    for step in services["steps"]:
        step["protocol"] = services["protocol"]
    script["steps"] = services["steps"] + script["steps"]
    return script


def prepare_script(source, destination, row):
    """Render the session's script for the row's repository."""
    if Path(source).resolve() != agy_test.HERE / "agy-session.json":
        raise ValueError(f"{source} is not agy-session.json")
    script = render(f"/tmp/agents/work/{row}/session/repo")
    if row == "agy-print":
        script["steps"][-1]["hold"] = True
    Path(destination).write_text(json.dumps(script))


def verify(archive, record, version):
    """Judge the returned session and print what the row measured."""
    with tempfile.TemporaryDirectory(prefix="rach2-agy-session-verify.") as name:
        work = Path(name)
        with tarfile.open(archive) as source:
            source.extractall(work, filter="data")
        mode = (work / "mode").read_text().strip()
        if mode not in ("print", "screen"):
            raise ValueError(f"unknown mode {mode}")
        at = (work / "at").read_text().strip()
        (work / "home").mkdir()
        script = render(at)
        (work / "script.json").write_text(json.dumps(script))
        agy_test.verify(work, Path(record))
        for binary in ("binary.before", "binary.after"):
            if (work / binary).read_text().split()[0] != agy_test.DIGEST:
                raise ValueError(f"{binary}: not Google's program")
        entries = [json.loads(line) for line in Path(record).read_text().splitlines()]
        for entry in entries:
            host = {key.lower(): value for key, value in entry["headers"]}.get("host")
            if "/streamGenerateContent" in entry["path"] and host != "fakeapi.test":
                raise ValueError(f"request {entry['n']}: the Gemini API at {host}")
        for number, step in enumerate(script["steps"], 1):
            if step.get("repeat"):
                asked = sum(entry["step"] == number for entry in entries)
                print(f"agy-session-verify: {step['request']['method']} "
                      f"{step['request']['headers']['Host']}{step['request']['path']}: "
                      f"{asked} requests")
        for line in (work / "steps").read_text().splitlines():
            seconds, what = line.split(" ", 1)
            print(f"agy-session-verify: {what} after {seconds} s")
        frames = [int(line) for line in (work / "memory").read_text().splitlines()]
        if not frames:
            raise ValueError("no sample of the free frames")
        seconds = int((work / "seconds").read_text())
        print(f"agy-session-verify: measurement: session {seconds} s; free frames {frames[0]} "
              f"at the start, {min(frames)} at the least ({min(frames) * 4} KiB); "
              f"the session took {(frames[0] - min(frames)) * 4} KiB")
    print(f"agy-session-verify: pass: Antigravity CLI {version}: {len(entries)} requests, "
          f"and the edited fixture committed in a clean repository")


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
        sys.exit(f"agy-session-verify: fail: {error}")
