#!/usr/bin/env python3
"""fakegh.py: a stand-in for GitHub's `gh` that serves a small board.

Usage: fakegh.py STATE ARGS...

STATE is a JSON file with the board: `login`, the account `gh api user`
names; `origin`, a bare git repository that stands for the repository on
GitHub; `milestones`, each an object with `number`, `title`, `state` and
`description`, whose line `Branch: NAME` names the milestone's branch; and
`issues`, each an object with `number`, `title`, `state`, `labels`,
`assignees`, `status`, the Status on the board, `milestone`, the number of
its milestone or null, `parent`, the number of the task a subtask is under
or null, and `comments`. The issues are on the board in the order STATE
lists them. The worker-session row of the agents suite runs tools/flow/
worker.sh, board.sh among it, with a program `gh` on the path that runs
this with its STATE and the arguments it was given.

ARGS are those of the calls tools/flow/worker.sh and board.sh make for
next, take, mine, release and branch, and of `gh issue comment` and
`gh issue close`. For each the program builds the JSON that GitHub's API
answers and prints what the call's --jq filter makes of it, through the jq
program, as gh does: a string bare, anything else as compact JSON. A call
that changes the board writes STATE again. Each call is appended to
STATE.calls as a JSON array of its arguments. A call this program does not
know is appended to STATE.calls with the prefix `unexpected`, and the
program says so on stderr and exits 1, so that a test which reads that file
finds it.

An issue that is closed, by `gh issue close` or by a commit, is set Done,
as the board's workflow sets a closed item. Before each call the program
does what GitHub does when a commit reaches the default branch: an open
issue that a message on the origin's master closes, with `Closes #N` or
another closing keyword, is closed. A worker closes its task with
`gh issue close`, and the row's commits reach no master, so the rule is
there only for a session that does what it should not.
"""

import fcntl
import json
import os
from pathlib import Path
import re
import subprocess
import sys

PROJECT = "PVT_rach2"
OWNER = "toksaitov"
BOARD = 6
FIELD = "PVTSSF_status"
STATUSES = ("Backlog", "Ready", "In progress", "Done")
CLOSING = re.compile(r"\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?) #([0-9]+)\b", re.I)


class Unexpected(Exception):
    """A call the stand-in does not serve."""


def options(args):
    """Return the flags of ARGS as a dictionary and the words left over.

    A flag that takes a value, as `--jq F` or `-f k=v`, is kept with its
    value; `-f` and `-F` collect theirs in lists. A flag in BARE takes none.
    """
    bare = {"--paginate"}
    flags = {"-f": [], "-F": []}
    words = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg in bare:
            flags[arg] = True
            i += 1
        elif arg.startswith("-") and i + 1 < len(args):
            if arg in ("-f", "-F"):
                flags[arg].append(args[i + 1])
            else:
                flags[arg] = args[i + 1]
            i += 2
        else:
            words.append(arg)
            i += 1
    return flags, words


def fields(flags):
    """Return the `-f` and `-F` fields of FLAGS as a dictionary."""
    return dict(f.split("=", 1) for f in flags["-f"] + flags["-F"])


def item_id(issue):
    """Return the board's item id for ISSUE."""
    return f"PVTI_{issue['number']}"


def option_id(name):
    """Return the id of the Status option NAME."""
    return "PVTSSO_" + name.replace(" ", "_")


def find(board, number):
    """Return the issue NUMBER of BOARD, or raise Unexpected."""
    for issue in board["issues"]:
        if issue["number"] == int(number):
            return issue
    raise Unexpected(f"no issue #{number}")


def milestone(board, issue):
    """Return ISSUE's milestone as an object of STATE, or None."""
    for m in board["milestones"]:
        if m["number"] == issue["milestone"]:
            return m
    if issue["milestone"] is None:
        return None
    raise Unexpected(f"no milestone {issue['milestone']}")


def node(board, issue):
    """Return ISSUE as GitHub's GraphQL Issue node for the board's queries."""
    m = milestone(board, issue)
    return {"number": issue["number"], "title": issue["title"], "state": issue["state"],
            "assignees": {"nodes": [{"login": a} for a in issue["assignees"]]},
            "labels": {"nodes": [{"name": n} for n in issue["labels"]]},
            "parent": None if issue["parent"] is None else {"number": issue["parent"]},
            "milestone": None if m is None else {
                "number": m["number"], "state": m["state"], "description": m["description"]},
            "issueDependenciesSummary": {"blockedBy": 0}}


def graphql(board, flags):
    """Answer one of board.sh's GraphQL queries or mutations."""
    query = fields(flags).get("query", "")
    page = {"hasNextPage": False, "endCursor": None}
    if "items(first: 100" in query:
        nodes = [{"id": item_id(i), "isArchived": False,
                  "fieldValueByName": {"name": i["status"]} if i["status"] else None,
                  "content": node(board, i)} for i in board["issues"]]
        return {"data": {"user": {"projectV2": {"items": {"pageInfo": page, "nodes": nodes}}}}}
    if "issues(first: 100" in query:
        states = re.search(r"states: \[([A-Z, ]*)\]", query).group(1).split(", ")
        nodes = [dict(node(board, i), projectItems={"nodes": [
                     {"id": item_id(i), "project": {"number": BOARD, "owner": {"login": OWNER}},
                      "fieldValueByName": {"name": i["status"]} if i["status"] else None}]})
                 for i in board["issues"] if i["state"] in states]
        return {"data": {"repository": {"issues": {"pageInfo": page, "nodes": nodes}}}}
    if 'field(name: "Status")' in query:
        return {"data": {"user": {"projectV2": {"id": PROJECT, "field": {
            "id": FIELD, "options": [{"id": option_id(s), "name": s} for s in STATUSES]}}}}}
    if "projectItems(first: 10)" in query:
        issue = find(board, fields(flags)["n"])
        return {"data": {"repository": {"issue": {"projectItems": {"nodes": [
            {"id": item_id(issue), "project": {"id": PROJECT}}]}}}}}
    raise Unexpected("a GraphQL query board.sh's next, take, mine and release do not make")


def rest(board, flags, words):
    """Answer `gh api PATH` for the user, an issue, its assignees and timeline."""
    path = words[0]
    method = flags.get("-X", "GET")
    if path == "user" and method == "GET":
        return {"login": board["login"]}
    match = re.fullmatch(r"repos/[^/]+/[^/]+/issues/([0-9]+)(?:/(assignees|timeline))?", path)
    if not match:
        raise Unexpected(f"the REST path {path}")
    issue = find(board, match.group(1))
    if match.group(2) is None and method == "GET":
        m = milestone(board, issue)
        return {"number": issue["number"], "title": issue["title"],
                "state": issue["state"].lower(),
                "assignees": [{"login": a} for a in issue["assignees"]],
                "labels": [{"name": n} for n in issue["labels"]],
                "milestone": None if m is None else {
                    "number": m["number"], "title": m["title"], "state": m["state"].lower(),
                    "description": m["description"]}}
    if match.group(2) == "timeline" and method == "GET":
        return issue.setdefault("timeline", [])
    login = fields(flags).get("assignees[]")
    if match.group(2) == "assignees" and login and method == "POST":
        if login not in issue["assignees"]:
            issue["assignees"].append(login)
        issue.setdefault("timeline", []).append(
            {"event": "assigned", "assignee": {"login": login}})
        return {"number": issue["number"]}
    if match.group(2) == "assignees" and login and method == "DELETE":
        if login in issue["assignees"]:
            issue["assignees"].remove(login)
        issue.setdefault("timeline", []).append(
            {"event": "unassigned", "assignee": {"login": login}})
        return {"number": issue["number"]}
    raise Unexpected(f"{method} {path}")


def view(issue, names):
    """Return the fields NAMES of ISSUE as `gh issue view --json` gives them."""
    known = {"number": issue["number"], "title": issue["title"], "state": issue["state"],
             "assignees": [{"login": a} for a in issue["assignees"]],
             "labels": [{"name": n} for n in issue["labels"]]}
    if any(n not in known for n in names):
        raise Unexpected(f"the issue fields {','.join(names)}")
    return {n: known[n] for n in names}


def close(issue):
    """Close ISSUE and set it Done, as GitHub and the board's workflow do."""
    issue["state"] = "CLOSED"
    issue["status"] = "Done"


def issue_command(board, flags, words):
    """Answer `gh issue view`, `list`, `edit`, `comment` and `close`."""
    verb = words[0]
    if verb == "view":
        return view(find(board, words[1]), flags["--json"].split(","))
    if verb == "list":
        label, state = flags.get("--label"), flags.get("--state", "open")
        return [view(i, flags["--json"].split(",")) for i in board["issues"]
                if (label is None or label in i["labels"]) and
                (state == "all" or i["state"] == state.upper())]
    if verb == "edit":
        issue = find(board, words[1])
        if flags.get("--remove-label") in issue["labels"]:
            issue["labels"].remove(flags["--remove-label"])
        if flags.get("--add-label") and flags["--add-label"] not in issue["labels"]:
            issue["labels"].append(flags["--add-label"])
        if set(flags) - {"-f", "-F", "--repo", "--remove-label", "--add-label"}:
            raise Unexpected("an issue edit other than of its labels")
        return None
    if verb == "comment" and "--body" in flags:
        find(board, words[1])["comments"].append(flags["--body"])
        return None
    if verb == "close" and not set(flags) - {"-f", "-F", "--repo", "--comment"}:
        issue = find(board, words[1])
        if issue["state"] != "OPEN":
            print(f"! Issue #{issue['number']} is already closed", file=sys.stderr)
            return None
        if "--comment" in flags:
            issue["comments"].append(flags["--comment"])
        close(issue)
        return None
    raise Unexpected(f"gh issue {verb}")


def item_edit(board, flags):
    """Answer `gh project item-edit` that sets an item's Status."""
    if flags.get("--project-id") != PROJECT or flags.get("--field-id") != FIELD:
        raise Unexpected("an item edit of another board or field")
    for issue in board["issues"]:
        if item_id(issue) == flags.get("--id"):
            for status in STATUSES:
                if option_id(status) == flags.get("--single-select-option-id"):
                    issue["status"] = status
                    return {"id": item_id(issue)}
    raise Unexpected("an item edit of no item or option")


def close_pushed(board):
    """Close each open issue that a commit message on the origin's master closes."""
    log = subprocess.run(["git", "--git-dir", board["origin"], "log", "--format=%B", "master"],
                         check=True, capture_output=True, text=True).stdout
    for number in CLOSING.findall(log):
        for issue in board["issues"]:
            if issue["number"] == int(number) and issue["state"] == "OPEN":
                close(issue)


def answer(board, args):
    """Return the JSON value the call ARGS answers, changing BOARD as it says."""
    if len(args) >= 2 and args[0] == "api" and args[1] == "graphql":
        return graphql(board, options(args[2:])[0])
    if args and args[0] == "api":
        flags, words = options(args[1:])
        if len(words) != 1:
            raise Unexpected("gh api without one path")
        return rest(board, flags, words)
    if len(args) >= 2 and args[0] == "issue":
        flags, words = options(args[1:])
        return issue_command(board, flags, words)
    if len(args) >= 2 and args[0:2] == ["project", "item-edit"]:
        return item_edit(board, options(args[2:])[0])
    raise Unexpected("a command gh's stand-in does not serve")


def main(args):
    """Serve one call, under a lock on STATE."""
    state = Path(args[0])
    call = args[1:]
    with open(f"{state}.lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        board = json.loads(state.read_text())
        try:
            close_pushed(board)
            value = answer(board, call)
        except Unexpected as error:
            with open(f"{state}.calls", "a") as calls:
                calls.write(json.dumps(["unexpected"] + call) + "\n")
            print(f"fakegh: unexpected: {error}: gh {' '.join(call)}", file=sys.stderr)
            return 1
        with open(f"{state}.calls", "a") as calls:
            calls.write(json.dumps(call) + "\n")
        Path(f"{state}.new").write_text(json.dumps(board, indent=1) + "\n")
        os.replace(f"{state}.new", state)
    filters = options(call)[0].get("--jq")
    if value is None:
        return 0
    if filters is None:
        print(json.dumps(value))
        return 0
    result = subprocess.run(["jq", "-r", "-c", filters], input=json.dumps(value), text=True)
    return 1 if result.returncode else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
