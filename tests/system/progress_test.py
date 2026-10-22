#!/usr/bin/env python3
"""Check the manager's progress report against complete and failed command reads."""
import contextlib
import copy
import importlib.util
import io
import json
import subprocess
import sys
from unittest.mock import patch


def issue(number, age, blocks=0, labels=(), assigned=True):
    """Make an open issue with a fixed age and dependency count."""
    return {"number": number, "title": f"task {number}",
            "created_at": f"2026-10-21T{12 - age:02d}:00:00Z",
            "labels": [{"name": name} for name in labels],
            "assignees": [{"login": "worker"}] if assigned else [],
            "issue_dependencies_summary": {"blocking": blocks, "blocked_by": 0}}


def main():
    """Exercise time boundaries, membership, ordering, hand-back and failed reads."""
    path = sys.argv[1] if len(sys.argv) > 1 else "tools/flow/progress.py"
    spec = importlib.util.spec_from_file_location("progress", path)
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    now = reader.stamp("2026-10-21T12:00:00Z")
    base = {
        "phase": "589\n",
        "members": "1\tOPEN\t-\tbug\ttask 1\n2\tOPEN\t-\tbug\ttask 2\n",
        "opened": [issue(1, 8, 2), issue(2, 7, 5)],
        "comments": [{"body": "phase: started at " + "a" * 40,
                      "created_at": "2026-10-21T06:00:00Z"}],
        "timeline": [],
    }
    failed = 0

    def run(name, change, required=(), absent=(), error=False):
        """Run one report with exact command fixtures and check its evidence."""
        nonlocal failed
        data = copy.deepcopy(base)
        data.update(change)

        def command(argv, **kwargs):
            """Emulate the board and paginated REST lists; reject other requests."""
            if argv[0] == "sh":
                if argv[2:] == ["current"]:
                    return data["phase"]
                if argv[2:] == ["members", "589"]:
                    return data["members"]
            assert argv[:4] == ["gh", "api", "--paginate", "--slurp"], argv
            resource = argv[4]
            if resource == "repos/toksaitov/rach2/issues?state=open&per_page=100":
                value = data["opened"]
            elif resource == "repos/toksaitov/rach2/issues/589/comments?per_page=100":
                value = data["comments"]
            elif resource.endswith("/timeline?per_page=100"):
                value = data["timeline"]
            else:
                raise AssertionError(argv)
            if value is None:
                raise subprocess.CalledProcessError(1, argv)
            # Two pages require the reader to flatten the whole response.
            return json.dumps([value[:1], value[1:]])

        output = io.StringIO()
        caught = False
        with patch.object(reader.subprocess, "check_output", side_effect=command):
            with contextlib.redirect_stdout(output):
                try:
                    reader.report(now)
                except (subprocess.SubprocessError, ValueError, KeyError, TypeError):
                    caught = True
        text = output.getvalue()
        if caught != error or any(s not in text for s in required) or any(s in text for s in absent):
            print(f"progress-test: failed: {name}: {text!r}; error={caught}")
            failed = 1

    run("phase-idle", {}, required=("incident: phase #589",))
    run("dependency", {}, required=("incident: member #1", "incident: member #2"))
    # The report must put the member that blocks more work first.
    run("order", {}, required=("member #2: blocks 5 open issues; worker; 21600s since "
                              "assignment or phase start; task 2\nprogress: member #1",))
    waiting = copy.deepcopy(base["opened"])
    waiting[0]["issue_dependencies_summary"]["blocked_by"] = 1
    run("prerequisite", {"opened": waiting},
        required=("waits on 1 open prerequisites",), absent=("incident: member #1",))
    run("new-assignment", {"timeline": [{"event": "assigned", "assignee": {"login": "worker"},
                                         "created_at": "2026-10-21T11:59:00Z"}]},
        required=("60s since assignment",), absent=("incident: member",))
    run("unclaimed-member", {"opened": [issue(1, 8, 2, assigned=False),
                                         issue(2, 7, 5, assigned=False)]},
        required=("unassigned; no open prerequisite",), absent=("incident: member",))
    recent = "3\tCLOSED\t2026-10-21T11:59:00Z\tbug\ttask 3\n"
    run("closure", {"members": base["members"] + recent},
        required=("last closed #3; 60s since a member",), absent=("incident: phase",))
    run("new-phase", {"comments": [{"body": "phase: started at " + "b" * 40,
                                    "created_at": "2026-10-21T11:59:00Z"}],
                      "members": base["members"] + recent.replace("11:59", "04:00")},
        required=("60s since a member",), absent=("incident:",))
    run("nothing-left", {"members": recent.replace("11:59", "04:00")},
        absent=("incident:",))
    run("nonmember", {"opened": base["opened"] + [issue(90, 12, 20)]},
        absent=("member #90",))
    ci = issue(4, 1, labels=("ci",), assigned=False)
    run("unassigned", {"opened": base["opened"] + [ci]},
        required=("incident: failure #4", "unassigned for 3600s"))
    run("half-hour", {"opened": base["opened"] + [ci],
                      "timeline": [{"event": "unassigned", "created_at": "2026-10-21T11:30:00Z"}]},
        required=("incident: failure #4", "unassigned for 1800s"))
    run("recent-handback", {"opened": base["opened"] + [ci],
                            "timeline": [{"event": "unassigned", "created_at": "2026-10-21T11:30:01Z"}]},
        required=("unassigned for 1799s",), absent=("incident: failure",))
    run("intermittent", {"opened": base["opened"] + [issue(4, 8, labels=("ci", "intermittent"),
                                                          assigned=False)]},
        absent=("failure #4",))
    run("assigned", {"opened": base["opened"] + [issue(4, 8, labels=("ci",))]},
        absent=("failure #4",))
    run("no-phase", {"phase": "", "opened": [ci]},
        required=("no phase is In progress", "incident: failure #4"))
    run("failed-read", {"opened": None}, error=True)
    run("failed-timeline", {"opened": base["opened"] + [ci], "timeline": None}, error=True)
    run("missing-start", {"comments": []}, error=True)
    run("changed-member", {"opened": []}, error=True)
    if not failed:
        print("progress-test: pass")
    return failed


if __name__ == "__main__":
    sys.exit(main())
