#!/usr/bin/env python3
"""Check that comments in the in-house tree wrap at 80 columns.

`CLAUDE.md` says code comments wrap at 80 columns, and nothing enforced it, so
the count grew: the milestone 6 review recorded 645 over-width lines in the
kernel as deferred, and there were more by the time this was written.

Only whole comment lines are measured, and that boundary is where this project's
two rules meet. `CLAUDE.md` wraps comments at 80. `.clang-format` sets
`ColumnLimit: 100` for code. A comment trailing a code line is part of a code
line and answers to the second rule, which clang-format already enforces;
wrapping it would mean lifting it above the code, which turns an aligned column
of short notes into a stack of interruptions and reads worse than what it
replaced.

A string literal holding a comment delimiter is not a comment either: the scan
tracks literals so `"http://x"` and `printf("# %d")` do not open one.

Covered: the trees this project writes. Not covered: `vendor/`, `build/`, and
`ports/doom/src/`, which is upstream Doom. The eleven files above it in
`ports/doom/` are this project's glue and are covered; reflowing the eighty
below it would make a large diff against upstream for nothing.

Usage: check_comments.py [PATH ...]     paths default to the covered roots
       check_comments.py --selftest
"""

import os
import sys

LIMIT = 80

ROOTS = ("rach2", "utils", "ish", "krffs", "tests", "tools", "mk", "ports/doom")

C_EXT = (".c", ".h")
HASH_EXT = (".py", ".sh", ".mk")

SKIP_DIRS = ("vendor", "build", ".git")

# Upstream Doom, under a directory this project otherwise owns.
SKIP_PATHS = (os.path.join("ports", "doom", "src"),)


def c_comment_lines(text):
    """Line numbers (1-based) that carry C comment text, literal-aware.

    A line is reported once even if it carries several comments. The scan is a
    small state machine rather than a regular expression because a `/*` inside a
    string and a quote inside a comment both occur in this tree.
    """
    lines = set()
    line = 1
    i = 0
    n = len(text)
    state = "code"  # code, block, line_comment, string, char
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if ch == "\n":
            line += 1
            if state == "line_comment":
                state = "code"
            i += 1
            continue

        if state == "code":
            if ch == "/" and nxt == "*":
                state = "block"
                lines.add(line)
                i += 2
                continue
            if ch == "/" and nxt == "/":
                state = "line_comment"
                lines.add(line)
                i += 2
                continue
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            i += 1
            continue

        if state == "block":
            lines.add(line)
            if ch == "*" and nxt == "/":
                state = "code"
                i += 2
                continue
            i += 1
            continue

        if state == "line_comment":
            lines.add(line)
            i += 1
            continue

        # Inside a literal: a backslash escapes the next character, so a
        # trailing backslash cannot end the literal early.
        if ch == "\\":
            i += 2
            continue
        if state == "string" and ch == '"':
            state = "code"
        elif state == "char" and ch == "'":
            state = "code"
        i += 1

    return lines


def hash_comment_lines(text):
    """Line numbers that carry a `#` comment, outside quotes.

    Good enough for shell, make and Python as this tree writes them: a `#` in a
    single- or double-quoted string is not a comment, and a shebang is one but
    is short by nature.

    A triple-quoted Python string spans lines, which the per-line quote tracking
    below cannot see, so those lines are found first and skipped. Without that, a
    test fixture holding a deliberately over-width `#` line reads as a comment
    and this checker fails on its own selftest data.
    """
    skip = set()
    depth = 0
    for number, raw in enumerate(text.splitlines(), 1):
        marks = raw.count('"' * 3) + raw.count("'" * 3)
        if depth:
            skip.add(number)
        if marks % 2:
            depth = 0 if depth else 1
            skip.add(number)

    lines = set()
    for number, raw in enumerate(text.splitlines(), 1):
        if number in skip:
            continue
        in_single = False
        in_double = False
        i = 0
        while i < len(raw):
            ch = raw[i]
            if ch == "\\":
                i += 2
                continue
            if ch == "'" and not in_double:
                in_single = not in_single
            elif ch == '"' and not in_single:
                in_double = not in_double
            elif ch == "#" and not in_single and not in_double:
                lines.add(number)
                break
            i += 1
    return lines


def over_width(path):
    """[(line number, width)] for comment lines wider than the limit."""
    with open(path, "r", errors="replace") as f:
        text = f.read()

    if path.endswith(C_EXT):
        marked = c_comment_lines(text)
    elif path.endswith(HASH_EXT):
        marked = hash_comment_lines(text)
    else:
        return []

    # What opens a comment LINE, per language. `#` is a comment in shell, make
    # and Python, and a preprocessor directive in C: `#define X 1 /* note */` is
    # a code line carrying a trailing comment, not a comment line.
    starts = ("*", "/*", "//") if path.endswith(C_EXT) else ("#",)

    out = []
    for number, raw in enumerate(text.splitlines(), 1):
        if number not in marked or len(raw) <= LIMIT:
            continue
        if not raw.lstrip().startswith(starts):
            continue
        out.append((number, len(raw)))
    return out


def walk(roots):
    for root in roots:
        if os.path.isfile(root):
            yield root
            continue
        for base, dirs, names in os.walk(root):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            if any(sp in base for sp in SKIP_PATHS):
                continue
            for name in sorted(names):
                if name.endswith(C_EXT) or name.endswith(HASH_EXT):
                    yield os.path.join(base, name)


SELFTEST_C = """\
/* a short header */
/* a block comment line made deliberately longer than eighty columns ......... */
int x = 1;  /* a trailing comment on a code line, over eighty, not a comment line */
const char *s = "/* not a comment: a delimiter inside a string, padded past eighty */";
const char *t = "// also not a comment, padded well past eighty columns as well ....";
char q = '\"';  /* a quote inside a char literal must not open a string */
"""

SELFTEST_SH = """\
#!/bin/sh
# a comment line written deliberately to run well past the eighty column rule ....
echo "# not a comment: a hash inside double quotes, padded out past eighty ......"
echo 'a # inside single quotes is not a comment either, padded past eighty ......'
"""


def selftest():
    import tempfile
    failures = 0

    root = tempfile.mkdtemp()
    cpath = os.path.join(root, "t.c")
    with open(cpath, "w") as f:
        f.write(SELFTEST_C)
    # Line 2 is a comment line over the limit. Line 3 is over the limit too but
    # is a code line carrying a trailing comment, which clang-format governs.
    # Lines 4 and 5 are longer still and are string literals.
    got = sorted(n for n, _ in over_width(cpath))
    want = [2]
    if got != want:
        print("check_comments: selftest: C: expected %r, got %r" % (want, got),
              file=sys.stderr)
        failures += 1

    spath = os.path.join(root, "t.sh")
    with open(spath, "w") as f:
        f.write(SELFTEST_SH)
    got = sorted(n for n, _ in over_width(spath))
    want = [2]
    if got != want:
        print("check_comments: selftest: sh: expected %r, got %r" % (want, got),
              file=sys.stderr)
        failures += 1

    if failures:
        return 1
    print("check_comments: selftest OK (2 cases)")
    return 0


def main(argv):
    if len(argv) > 1 and argv[1] == "--selftest":
        return selftest()

    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir)
    if len(argv) > 1:
        roots = argv[1:]
    else:
        roots = [os.path.join(here, r) for r in ROOTS]
        roots = [r for r in roots if os.path.exists(r)]

    total = 0
    files = 0
    for path in walk(roots):
        hits = over_width(path)
        if hits:
            files += 1
            total += len(hits)
            rel = os.path.relpath(path, here)
            for number, width in hits:
                print("%s:%d: comment line is %d columns" % (rel, number, width),
                      file=sys.stderr)

    if total:
        print("check_comments: %d comment line(s) over %d columns in %d file(s)"
              % (total, LIMIT, files), file=sys.stderr)
        return 1

    print("check_comments: OK (no comment line over %d columns)" % LIMIT)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
