"""rows.py: piglit's results as the rows judge.py compares.

Usage:
  python3 -I rows.py RESULTS PATTERN...

RESULTS is piglit's results.json, or results.json.bz2 as Debian's piglit
writes it. Prints `NAME RESULT` for each test whose name a PATTERN finds,
a Python regular expression as piglit's -t takes it, and for each of its
subtests as `NAME@SUBTEST`, as runners/piglit.sh writes its PART.results,
sorted by name.
"""

import bz2
import json
import re
import sys


def main():
    """Print the rows of the tests the patterns name."""
    if len(sys.argv) < 3:
        sys.stderr.write("usage: rows.py RESULTS PATTERN...\n")
        return 2
    path = sys.argv[1]
    opener = bz2.open if path.endswith(".bz2") else open
    with opener(path, "rt", encoding="utf-8") as f:
        tests = json.load(f)["tests"]
    patterns = [re.compile(p) for p in sys.argv[2:]]
    rows = []
    for name, t in tests.items():
        if not any(p.search(name) for p in patterns):
            continue
        rows.append((name, t["result"]))
        for sub, result in t.get("subtests", {}).items():
            if sub != "__type__":
                rows.append(("%s@%s" % (name, sub), result))
    for name, result in sorted(rows):
        print("%s %s" % (name, result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
