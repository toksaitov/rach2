#!/usr/bin/env python3
"""Verify the worker-session row on the host.

Usage: worker_session_verify.py --watch RECORD TIMINGS
       worker_session_verify.py ARCHIVE RECORD VERSION

--watch is muse_verify.py's observer of the record. The second form judges
the archive the row returns. Every recorded request must be the one its
step of worker-session.json accepts, and the first of each session must
carry the prompt of tools/flow/worker.txt for its issue, from Claude Code of
VERSION. The loop's log must show the loop take #901, run one session on it
that ends with status 0, find it closed, delete its merged branch, take
#902, run one session on that, and stop. The board must have #901 closed
and Done and still this account's, #902 open, Ready, unassigned and with
the session's comment, and the phase as it was, and gh must have had no
call that fakegh.py does not serve. The origin's master must have the tree
and then one commit for each issue, the first closing #901 and the second
closing nothing, each adding its file. Each session's transcript must end
with a result that is not an error, and the program must be Anthropic's
before and after the loop.
"""

import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE.parent / "fakeapi"))

import claude_test  # noqa: E402
import fakeapi  # noqa: E402
from muse_verify import watch  # noqa: E402

LOGIN = "rach2-test"
CLOSING = re.compile(r"\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?) #[0-9]+\b", re.I)


def check_record(entries):
    """Require each request to be the one its step accepts, every step once."""
    script = json.loads((HERE / "worker-session.json").read_text())
    steps = script["steps"]
    requests = [e for e in entries if "method" in e]
    if sorted(e.get("step", 0) for e in requests) != list(range(1, len(steps) + 1)):
        raise ValueError(f"the record is not {len(steps)} accepted requests, one for each step")
    for entry in requests:
        step = steps[entry["step"] - 1]
        request = fakeapi.Request(entry["method"], entry["path"] +
                                  ("?" + entry["query"] if entry["query"] else ""),
                                  "HTTP/1.1", entry["headers"], entry["body"].encode())
        reason = fakeapi.protocol(step.get("protocol", script["protocol"])).check(step, request)
        if reason:
            raise ValueError(f"request {entry['n']}: {reason}")
    return requests, steps


def first_text(body):
    """Return the text of the first user message of a Messages body."""
    content = body["messages"][0]["content"]
    if isinstance(content, str):
        return content
    return "\n".join(b.get("text", "") for b in content if b.get("type") == "text")


def check_prompts(requests, steps, version):
    """Require the worker's prompt for each issue at its session's first step."""
    prompt = (REPO / "tools/flow/worker.txt").read_text().strip()
    firsts = [n for n, step in enumerate(steps, 1) if "results" not in step]
    if len(firsts) != 2:
        raise ValueError("worker-session.json does not have two sessions")
    for number, issue in zip(firsts, (901, 902)):
        entry = next(e for e in requests if e["step"] == number)
        if prompt.replace("@ISSUE@", str(issue)) not in first_text(json.loads(entry["body"])):
            raise ValueError(f"request {entry['n']}: the session has no worker's prompt "
                             f"for #{issue}")
    for entry in requests:
        agent = {k.lower(): v for k, v in entry["headers"]}.get("user-agent", "")
        if not agent.startswith(f"claude-cli/{version} "):
            raise ValueError(f"request {entry['n']}: the user agent {agent}")


def check_log(work):
    """Require the loop's log to show what the module's comment says, in order."""
    lines = [line.split(" ", 2)[2] for line in
             (work / "state/loop.log").read_text().splitlines()]
    wanted = [r"loop starts: claude in .*/clone", r"took #901",
              r"session [0-9-]+ on #901 starts", r"session [0-9-]+ ended 0 after [0-9]+s",
              r"#901 is closed or no longer this account's",
              r"tidy: deleted the branch 901-worker-fixture", r"took #902",
              r"session [0-9-]+ on #902 starts", r"session [0-9-]+ ended 0 after [0-9]+s",
              r"stopped"]
    at = 0
    for line in lines:
        if at < len(wanted) and re.fullmatch(wanted[at], line):
            at += 1
    if at < len(wanted):
        raise ValueError(f"the loop's log has no [{wanted[at]}] where it should")
    for what, count in ((r"session .* starts", 2), (r"took #.*", 2)):
        if sum(1 for line in lines if re.fullmatch(what, line)) != count:
            raise ValueError(f"the loop's log has not {count} lines [{what}]")
    for line in lines:
        print(f"worker-session-verify: loop: {line}")


def check_board(work):
    """Require the board the module's comment describes, and no call refused."""
    issues = {i["number"]: i for i in json.loads((work / "board.json").read_text())["issues"]}
    wanted = {900: ("OPEN", "In progress", []), 901: ("CLOSED", "Done", [LOGIN]),
              902: ("OPEN", "Ready", [])}
    for number, (state, status, assignees) in wanted.items():
        issue = issues[number]
        got = (issue["state"], issue["status"], issue["assignees"])
        if got != (state, status, assignees):
            raise ValueError(f"#{number} is {got}, not {(state, status, assignees)}")
    if issues[902]["comments"] != ["The first part is on master; the rest waits for its blocker."]:
        raise ValueError(f"#902 has the comments {issues[902]['comments']}")
    calls = [json.loads(line) for line in (work / "board.json.calls").read_text().splitlines()]
    refused = [c for c in calls if c[0] == "unexpected"]
    if refused:
        raise ValueError(f"gh was called as fakegh.py does not serve: {refused[0][1:]}")
    return len(calls)


def git(work, *args):
    """Return what git prints for ARGS in the returned origin."""
    return subprocess.check_output(["git", "--git-dir", str(work / "origin.git"), *args],
                                   text=True)


def check_origin(work):
    """Require the tree and one commit for each issue on the origin's master."""
    commits = git(work, "rev-list", "--reverse", "master").split()
    if len(commits) != 3:
        raise ValueError(f"the origin's master has {len(commits)} commits, not 3")
    for commit, issue, closes in ((commits[1], 901, ["#901"]), (commits[2], 902, [])):
        message = git(work, "log", "-1", "--format=%B", commit)
        found = [m.split()[-1] for m in CLOSING.findall(message)]
        if found != closes:
            raise ValueError(f"the commit for #{issue} closes {found}, not {closes}")
        files = git(work, "diff-tree", "--no-commit-id", "--name-only", "-r", commit)
        if files != f"worker-{issue}.txt\n" or \
                git(work, "show", f"{commit}:worker-{issue}.txt") != f"worker fixture {issue}\n":
            raise ValueError(f"the commit for #{issue} does not add worker-{issue}.txt")


def check_sessions(work):
    """Require two transcripts, each ending with a result that is no error."""
    transcripts = sorted((work / "state").glob("s-*.jsonl"))
    if len(transcripts) != 2:
        raise ValueError(f"the loop kept {len(transcripts)} transcripts, not 2")
    for path in transcripts:
        last = json.loads(path.read_text().splitlines()[-1])
        if last.get("type") != "result" or last.get("is_error") is not False:
            raise ValueError(f"{path.name} does not end with a result that is no error")
    for binary in ("binary.before", "binary.after"):
        if (work / binary).read_text().split()[0] != claude_test.DIGEST:
            raise ValueError(f"{binary}: not Anthropic's program")


def verify(archive, record, version):
    """Judge the returned archive and the server's record."""
    with tempfile.TemporaryDirectory(prefix="rach2-worker-session-verify.") as name:
        work = Path(name)
        with tarfile.open(archive) as source:
            source.extractall(work, filter="data")
        entries = [json.loads(line) for line in Path(record).read_text().splitlines()]
        requests, steps = check_record(entries)
        check_prompts(requests, steps, version)
        check_log(work)
        calls = check_board(work)
        check_origin(work)
        check_sessions(work)
    print(f"worker-session-verify: pass: {len(requests)} requests in two sessions, {calls} "
          f"calls to gh, #901 closed by its commit and #902 handed back")


def main(args):
    """Dispatch the command line."""
    if args[0] == "--watch":
        watch(*args[1:])
    else:
        verify(*args)


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (OSError, ValueError, KeyError, IndexError, StopIteration, tarfile.TarError,
            subprocess.CalledProcessError) as error:
        sys.exit(f"worker-session-verify: fail: {error}")
