#!/usr/bin/env python3
"""Report unfinished phase work and delays that need the manager's attention."""
import datetime
import json
from pathlib import Path
import subprocess
import sys

REPO = "toksaitov/rach2"


def command(argv):
    """Read one command's output; an incomplete read fails the report."""
    return subprocess.check_output(argv, text=True, timeout=120)


def pages(path):
    """Read every page of a GitHub list, without the search index's delay."""
    data = json.loads(command(["gh", "api", "--paginate", "--slurp", path]))
    return [item for page in data for item in page]


def stamp(value):
    """Convert a GitHub timestamp to seconds since the epoch."""
    return datetime.datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()


def unassigned_since(issue):
    """Find when a currently unassigned issue last lost an assignee."""
    events = pages(f"repos/{REPO}/issues/{issue['number']}/timeline?per_page=100")
    return max([stamp(issue["created_at"])] + [stamp(e["created_at"])
               for e in events if e.get("event") == "unassigned"])


def assigned_since(issue):
    """Find the latest assignment to an account that still owns the issue."""
    owners = {a["login"] for a in issue["assignees"]}
    events = pages(f"repos/{REPO}/issues/{issue['number']}/timeline?per_page=100")
    return max([stamp(issue["created_at"])] + [stamp(e["created_at"])
               for e in events if e.get("event") == "assigned" and
               (e.get("assignee") or {}).get("login") in owners])


def report(now):
    """Print phase progress and incidents, including failures outside the phase."""
    board = ["sh", str(Path(__file__).with_name("board.sh"))]
    phase = command(board + ["current"]).strip()
    opened = pages(f"repos/{REPO}/issues?state=open&per_page=100")
    opened = {i["number"]: i for i in opened if "pull_request" not in i}
    incidents = []
    unknown = None
    if phase:
        members = [line.split("\t", 4) for line in
                   command(board + ["members", phase]).splitlines() if line]
        comments = pages(f"repos/{REPO}/issues/{phase}/comments?per_page=100")
        starts = [stamp(c["created_at"]) for c in comments
                  if any(line.startswith("phase: started at ")
                         for line in c["body"].splitlines())]
        start = min(starts) if starts else None
        work = [m for m in members if m[1] == "OPEN" and
                not set(m[3].split(",")) & {"intermittent", "incident", "day-report"}]
        if start is None:
            unknown = f"phase #{phase} has no start record"
        else:
            closed = [stamp(m[2]) for m in members if m[1] == "CLOSED"]
            last = max([start] + closed)
            last_member = next(("#" + m[0] for m in members
                                if m[1] == "CLOSED" and stamp(m[2]) == last), "none")
            age = max(0, int(now - last))
            print(f"progress: phase #{phase}: {len(work)} unfinished members; "
                  f"last closed {last_member}; {age}s since a member closed or the phase started")
            if work and age >= 21600:
                incidents.append(f"phase #{phase} has closed no member for six hours")
        critical = []
        for member in work:
            issue = opened.get(int(member[0]))
            if issue is None:
                raise ValueError(f"open member #{member[0]} changed during the read; retry")
            blocks = issue["issue_dependencies_summary"]["blocking"]
            if blocks > 0:
                critical.append((blocks, issue))
        for blocks, issue in sorted(critical, key=lambda p:
                                    (p[1]["issue_dependencies_summary"]["blocked_by"] > 0,
                                     -p[0], p[1]["number"])):
            number = issue["number"]
            owners = ",".join(a["login"] for a in issue["assignees"]) or "unassigned"
            blocked = issue["issue_dependencies_summary"]["blocked_by"]
            if blocked:
                print(f"progress: member #{number}: blocks {blocks} open issues; "
                      f"{owners}; waits on {blocked} open prerequisites; {issue['title']}")
                continue
            if not issue["assignees"]:
                print(f"progress: member #{number}: blocks {blocks} open issues; "
                      f"unassigned; no open prerequisite; {issue['title']}")
                continue
            age = max(0, int(now - max(start or now, assigned_since(issue))))
            print(f"progress: member #{number}: blocks {blocks} open issues; "
                  f"{owners}; {age}s since assignment or phase start; {issue['title']}")
            if start is not None and age >= 21600:
                incidents.append(f"member #{number} blocks {blocks} open issues "
                                 "and has not landed for six hours")
    else:
        print("progress: no phase is In progress")
    for number, issue in sorted(opened.items()):
        labels = {label["name"] for label in issue["labels"]}
        if "ci" in labels and "intermittent" not in labels and not issue["assignees"]:
            age = max(0, int(now - unassigned_since(issue)))
            print(f"progress: failure #{number}: unassigned for {age}s; {issue['title']}")
            if age >= 1800:
                incidents.append(f"failure #{number} has had no assignee for thirty minutes")
    for reason in incidents:
        print(f"progress: incident: {reason}")
    if unknown:
        raise ValueError(unknown)


def main():
    """Exit unsuccessfully when evidence is missing, rather than report no delay."""
    try:
        report(datetime.datetime.now(datetime.timezone.utc).timestamp())
    except (OSError, subprocess.SubprocessError, ValueError, KeyError, TypeError) as error:
        print(f"progress: unknown: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
