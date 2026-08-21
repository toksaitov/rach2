#!/usr/bin/env python3
"""Check dev-log/Readme.md against the milestone directories beside it.

`AGENTS.md` tells a reader that the last entry in the index is where the tree
stands. Nothing a milestone does touches the index, so nothing but a check keeps
that sentence true.

The rule is equality, not membership. The entries must be exactly the finished
milestones, in the directories' numeric order, so that the last entry is the
newest and the ones before it read as the history they claim to be. A directory
holding only a `plan.md` is work that is planned and not yet built, and it must
have no entry, because an entry states that the milestone happened.

A milestone is finished when it has the record of what was built. That file
is `Readme.md`, or `results.md`, which is what milestone 6 called its record.

Bullets inside fenced code blocks are examples, not entries and are
skipped, so documentation that shows the index's shape does not become part of
it.

Usage: check_devlog.py [DEVLOG_DIR]
       check_devlog.py --selftest
"""

import os
import re
import shutil
import sys
import tempfile

# A milestone directory: two digits, a dash, a lowercase name. Anything else in
# dev-log/ is ignored, not demanded of the index.
DIR_RE = re.compile(r"^(\d\d)-([a-z0-9-]+)$")

# An entry: `* \`NN-name\`: ...` at the left margin. An indented bullet is a
# sub-item, and a name without the colon is prose that mentions a milestone.
BULLET_RE = re.compile(r"^\*\s+`(\d\d-[a-z0-9-]+)`\s*:")

# The record of what a milestone built.
RECORD_NAMES = ("Readme.md", "results.md")

FENCE_RE = re.compile(r"^\s*(```|~~~)")


def milestone_dirs(devlog):
    """(name, finished) per milestone directory, in numeric order."""
    found = []
    for name in os.listdir(devlog):
        path = os.path.join(devlog, name)
        if not os.path.isdir(path):
            continue
        m = DIR_RE.match(name)
        if not m:
            continue
        finished = any(os.path.isfile(os.path.join(path, r))
                       for r in RECORD_NAMES)
        found.append((int(m.group(1)), name, finished))
    found.sort()
    return [(name, finished) for _, name, finished in found]


def index_entries(text):
    """The milestone names the index lists, in order, ignoring fenced blocks."""
    names = []
    in_fence = False
    for line in text.splitlines():
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        m = BULLET_RE.match(line)
        if m:
            names.append(m.group(1))
    return names


def check(dirs, entries):
    """Return a list of complaints; empty means the index matches the tree."""
    problems = []
    known = dict(dirs)
    expected = [name for name, finished in dirs if finished]

    for name in entries:
        if name not in known:
            problems.append(
                "the index names %s, which is not a directory" % name)
        elif not known[name]:
            problems.append(
                "the index names %s, which has a plan and no "
                "record, so the work is not done and the entry states "
                "that it is" % name)

    seen = set()
    for name in entries:
        if name in seen:
            problems.append("the index names %s more than once" % name)
        seen.add(name)

    for name in expected:
        if name not in seen:
            problems.append(
                "dev-log/%s is finished and has no entry in the index" % name)

    # Order is the half a membership check misses. Reported against the first
    # position that differs, since one displaced entry shifts everything after
    # it and listing them all would say the same thing many times.
    if entries != expected and not problems:
        for i, (got, want) in enumerate(zip(entries, expected)):
            if got != want:
                problems.append(
                    "the index's entry %d is %s where the directories give %s; "
                    "the entries must be in the milestones' numeric order, "
                    "because a reader takes the last one as the current state"
                    % (i + 1, got, want))
                break
        else:
            problems.append(
                "the index lists %d entries and there are %d finished "
                "milestones" % (len(entries), len(expected)))

    return problems


def _tree(root, spec):
    """Build a dev-log directory from {name: record filename or None}."""
    for name, record in spec:
        os.mkdir(os.path.join(root, name))
        open(os.path.join(root, name, "plan.md"), "w").close()
        if record:
            open(os.path.join(root, name, record), "w").close()


def _run(spec, index_text):
    """Run the real path over a temporary tree and return its complaints."""
    root = tempfile.mkdtemp()
    try:
        _tree(root, spec)
        with open(os.path.join(root, "Readme.md"), "w") as f:
            f.write(index_text)
        with open(os.path.join(root, "Readme.md")) as f:
            entries = index_entries(f.read())
        return check(milestone_dirs(root), entries)
    finally:
        shutil.rmtree(root)


def selftest():
    """Drive every rule from a temporary tree, asserting what is reported."""
    spec = [("24-metal", "Readme.md"),
            ("25-sealed", "Readme.md"),
            ("26-portable", None)]
    six = [("06-review", "results.md"),
           ("07-init", "Readme.md")]

    def entry(name):
        return "* `%s`: a sentence.\n" % name

    cases = [
        ("a current index", spec,
         entry("24-metal") + entry("25-sealed"), []),
        ("milestone 6's record name", six,
         entry("06-review") + entry("07-init"), []),
        ("the newest finished milestone missing", spec,
         entry("24-metal"),
         ["dev-log/25-sealed is finished and has no entry in the index"]),
        ("an in-flight milestone indexed as done", spec,
         entry("24-metal") + entry("25-sealed") + entry("26-portable"),
         ["the index names 26-portable, which has a plan and no record, so "
          "the work is not done and the entry states that it is"]),
        ("entries out of order", spec,
         entry("25-sealed") + entry("24-metal"),
         ["the index's entry 1 is 25-sealed where the directories give "
          "24-metal; the entries must be in the milestones' numeric "
          "order, because a reader takes the last one as the current "
          "state"]),
        ("an entry for a directory that is gone", spec,
         entry("24-metal") + entry("25-sealed") + entry("27-polish"),
         ["the index names 27-polish, which is not a directory"]),
        ("a milestone named twice", spec,
         entry("24-metal") + entry("25-sealed") + entry("25-sealed"),
         ["the index names 25-sealed more than once"]),
        ("a decoy entry inside a fenced block", spec,
         entry("24-metal") + "```\n" + entry("25-sealed") + "```\n",
         ["dev-log/25-sealed is finished and has no entry in the index"]),
        ("a fenced example does not satisfy the order rule", spec,
         entry("24-metal") + entry("25-sealed")
         + "```\n" + entry("26-portable") + "```\n", []),
    ]

    failures = 0
    for name, dirs, text, want in cases:
        got = _run(dirs, text)
        if got != want:
            print("check_devlog: selftest: %s:\n  expected %r\n  got %r"
                  % (name, want, got), file=sys.stderr)
            failures += 1

    text = ("* `25-sealed`: an entry.\n"
            "  * `24-metal`: indented, so a sub-item.\n"
            "* not-a-milestone: no backticks.\n"
            "* `26-portable` no colon after the name.\n"
            "~~~\n* `07-init`: fenced with tildes.\n~~~\n")
    if index_entries(text) != ["25-sealed"]:
        print("check_devlog: selftest: the entry pattern read %r"
              % index_entries(text), file=sys.stderr)
        failures += 1

    if failures:
        return 1
    print("check_devlog: selftest OK (%d cases)" % (len(cases) + 1))
    return 0


def main(argv):
    if len(argv) > 1 and argv[1] == "--selftest":
        return selftest()

    devlog = argv[1] if len(argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), os.pardir, "dev-log")
    index = os.path.join(devlog, "Readme.md")

    if not os.path.isdir(devlog):
        print("check_devlog: %s is not a directory" % devlog, file=sys.stderr)
        return 1
    if not os.path.isfile(index):
        print("check_devlog: %s is missing" % index, file=sys.stderr)
        return 1

    with open(index) as f:
        entries = index_entries(f.read())
    dirs = milestone_dirs(devlog)

    problems = check(dirs, entries)
    if problems:
        for p in problems:
            print("check_devlog: %s" % p, file=sys.stderr)
        return 1

    done = [name for name, finished in dirs if finished]
    planned = [name for name, finished in dirs if not finished]
    print("check_devlog: OK (%d finished, newest is %s%s)"
          % (len(done), done[-1] if done else "none",
             ", %d planned" % len(planned) if planned else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
