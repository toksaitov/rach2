#!/usr/bin/env python3
"""Prepare and verify the sessions of the Codex rows on the host.

Usage: codex_session_verify.py --script SOURCE DESTINATION ROW
       codex_session_verify.py --watch RECORD TIMINGS
       codex_session_verify.py ARCHIVE RECORD VERSION

--script writes to DESTINATION the script SOURCE, codex.json, with
codex-screen.json's steps after it for the row codex-screen. --watch is
muse_verify.py's observer of the record. The third form judges a row's
archive: the edited greeting, committed by the fixture's author in a clean
work tree, and every recorded request against the step it played, each
request of the Responses API with the dummy key and the user agent of Codex
VERSION, codex_exec or codex-tui by the row's mode, and two turns on the
first WebSocket. It also requires that
the program before and after the session is bin/codex of the package the
recipe pins, and prints the times of the row's steps and the free frames
it sampled.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent
FAKEAPI = HERE.parent / "fakeapi"
REPO = HERE.parents[2]
sys.path.insert(0, str(FAKEAPI))

import fakeapi  # noqa: E402
from muse_verify import watch  # noqa: E402

KEY = "sk-fakeapi-dummy"
AGENTS = {"exec": "codex_exec", "screen": "codex-tui"}


def render(screen=False):
    """Return codex.json, with the steps of codex-screen.json for SCREEN."""
    script = json.loads((FAKEAPI / "codex.json").read_text())
    if screen:
        script["steps"] += json.loads((FAKEAPI / "codex-screen.json").read_text())["steps"]
    return script


def prepare_script(source, destination, row):
    """Write the script of the row ROW."""
    if Path(source).resolve() != FAKEAPI / "codex.json":
        raise ValueError(f"{source} is not codex.json")
    Path(destination).write_text(json.dumps(render(row == "codex-screen")))


def program_digest():
    """Return the SHA-256 of bin/codex in the package the recipe pins.

    The package is the download fetch_agents.sh keeps, checked first against
    the recipe's digest.
    """
    recipe = (REPO / "vendor/gnu/agents/packages/codex.sh").read_text()
    package = re.search(r"^PACKAGE=(\S+)$", recipe, re.M).group(1)
    pinned = re.search(r'^FILES="\$PACKAGE \S+ sha256:([0-9a-f]{64})$', recipe, re.M).group(1)
    path = REPO / "vendor/gnu/downloads/agents/codex" / package
    if hashlib.sha256(path.read_bytes()).hexdigest() != pinned:
        raise ValueError(f"{path} is not the package the recipe pins")
    with tarfile.open(path) as source:
        for member in source.getmembers():
            if os.path.normpath(member.name) == "bin/codex" and member.isfile():
                return hashlib.sha256(source.extractfile(member).read()).hexdigest()
    raise ValueError(f"{path} has no bin/codex")


def judge_repository(tree, home):
    """Require the edited greeting, committed alone, in a clean work tree."""
    env = dict(os.environ, HOME=str(home), GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL="/dev/null")

    def git(*args):
        """Read the returned repository without the host's settings."""
        return subprocess.check_output(["git", "-C", str(tree), *args], env=env,
                                       text=True).strip()

    fixture = FAKEAPI / "repo"
    for name in ("check.sh", "notes.txt"):
        if (tree / name).read_bytes() != (fixture / name).read_bytes():
            raise ValueError(f"{name} changed")
    checks = [
        ((tree / "greeting.txt").read_text(), "hello, rach2\n", "the greeting"),
        (git("log", "--format=%s|%an <%ae>"),
         "Greet Rach2|Codex Test <codex@fakeapi.test>\nThe fixture|Codex Test <codex@fakeapi.test>",
         "the history"),
        (git("diff", "--name-only", "HEAD~1", "HEAD"), "greeting.txt", "the committed paths"),
        (git("show", "HEAD:greeting.txt"), "hello, rach2", "the committed greeting"),
        (git("ls-tree", "--name-only", "HEAD"), "check.sh\ngreeting.txt\nnotes.txt", "the tree"),
        (git("status", "--porcelain", "--untracked-files=all"), "", "the work tree"),
    ]
    for got, want, what in checks:
        if got != want:
            raise ValueError(f"{what} is {got!r}, not {want!r}")


def judge_record(record, mode, version):
    """Require each request to be the one its step expects; return their number."""
    script = render(mode == "screen")
    steps = script["steps"]
    entries = [json.loads(line) for line in Path(record).read_text().splitlines()]
    requests = [e for e in entries if "method" in e]
    if sorted(e.get("step", 0) for e in requests) != list(range(1, len(steps) + 1)):
        raise ValueError(f"the record is not {len(steps)} accepted requests, one for each step")
    agent = f"{AGENTS[mode]}/{version} "
    for entry in requests:
        step = steps[entry["step"] - 1]
        name = step.get("protocol", script["protocol"])
        request = fakeapi.Request(entry["method"], entry["path"] +
                                  ("?" + entry["query"] if entry["query"] else ""),
                                  "HTTP/1.1", entry["headers"], entry["body"].encode())
        reason = fakeapi.protocol(name).check(step, request)
        if reason:
            raise ValueError(f"request {entry['n']}: {reason}")
        if name != "responses":
            continue
        headers = {key.lower(): value for key, value in entry["headers"]}
        if headers.get("authorization") != "Bearer " + KEY:
            raise ValueError(f"request {entry['n']} is without the dummy key")
        if not headers.get("user-agent", "").startswith(agent):
            raise ValueError(f"request {entry['n']}: the user agent {headers.get('user-agent')}")
    socket = next(e["n"] for e in requests if e["step"] == 1)
    turns = [json.loads(e["received"]) for e in entries
             if e["n"] == socket and "received" in e]
    if [t.get("generate", True) for t in turns] != [False, True, True]:
        raise ValueError("the WebSocket did not carry a warmup and two turns")
    return len(requests)


def verify(archive, record, version):
    """Judge the returned session and print what the row measured."""
    with tempfile.TemporaryDirectory(prefix="rach2-codex-session-verify.") as name:
        work = Path(name)
        with tarfile.open(archive) as source:
            source.extractall(work, filter="data")
        mode = (work / "mode").read_text().strip()
        if mode not in AGENTS:
            raise ValueError(f"unknown mode {mode}")
        judge_repository(work / "repo", work)
        count = judge_record(record, mode, version)
        digest = program_digest()
        for binary in ("binary.before", "binary.after"):
            if (work / binary).read_text().split()[0] != digest:
                raise ValueError(f"{binary}: not OpenAI's program")
        for line in (work / "steps").read_text().splitlines():
            seconds, what = line.split(" ", 1)
            print(f"codex-session-verify: {what} after {seconds} s")
        frames = [int(line) for line in (work / "memory").read_text().splitlines()]
        if not frames:
            raise ValueError("no sample of the free frames")
        seconds = int((work / "seconds").read_text())
        print(f"codex-session-verify: measurement: session {seconds} s; free frames {frames[0]} "
              f"at the start, {min(frames)} at the least ({min(frames) * 4} KiB); "
              f"the session took {(frames[0] - min(frames)) * 4} KiB")
    print(f"codex-session-verify: pass: {count} requests, and the edited greeting committed "
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
    except (OSError, ValueError, KeyError, AttributeError, StopIteration, tarfile.TarError,
            subprocess.CalledProcessError) as error:
        sys.exit(f"codex-session-verify: fail: {error}")
