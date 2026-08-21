#!/usr/bin/env python3
"""Check the prose this project writes: banned terms, and repetition.

`CLAUDE.md` bans a register these models drift into. That ban was a paragraph
asking for a judgement, and across twenty-five milestones it did not hold: the
terms it names by hand were still in the tree, and the larger habit it does not
name, repeating one ordinary phrase until it becomes a signature, was never
looked for at all.

Two modes, because the rule has two halves.

`--banned` fails on terms that are wrong at any frequency. It reads comments AND
string literals, in every language here, because a person reads a test failure
message and it does not matter whether that message came from C or from shell.

`--report` measures a watch list of ordinary phrases and never fails anything. It
reads comments only: a string literal is not prose and counting it would move the
word totals the rates are computed against.

Prose is measured one comment at a time, and each comment is normalized first:
its `*`, `//` and `#` decoration is removed and its whitespace collapsed. Without
that, the numbers measure formatting instead of writing. A phrase broken across a
line break would not be counted at all, a lone `*` would count as a word (there
are 18,275 of them on our side and 34,890 upstream), and since the two corpora
wrap comments differently, every rate and every ratio would be biased by layout.
Counting per comment also stops a phrase being found across the boundary between
two unrelated comments.

The baseline for `--report` is the upstream sources this repository vendors:
musl, busybox, dropbear, tinycc, pdpmake, and the Doom engine. They are human
comment prose in the same domain, written by people with no connection to this
project, which is what makes them a control. They are read from the PINNED
TARBALLS, not from the extracted directories, because extraction happens during
a build and a fresh clone has only the tarballs; a check that needed the
extracted trees could not run on the machine this exists to serve.

Three populations are reported and never pooled: C and header comments, the
comments in shell, make and Python, and Markdown prose. Only the first has an
upstream counterpart, so only the first gets a ratio. Comparing our Markdown to
upstream C comments would compare a tutorial to a register description.

What a ratio is worth differs by phrase, and the report says so rather than
implying every multiple means the same thing. See `dev-log/26-portable/`.

Usage: prose_check.py --banned [PATH ...]
       prose_check.py --fixture [--write]
       prose_check.py --report
       prose_check.py --ngrams [N]
       prose_check.py --selftest
"""

import os
import re
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.normpath(os.path.join(HERE, os.pardir))
sys.path.insert(0, HERE)

from check_comments import c_spans, hash_comment_lines   # one lexer, not two

ROOTS = ("rach2", "utils", "ish", "krffs", "tests", "tools", "mk", "ports/doom")
SKIP_DIRS = ("vendor", "build", ".git")
SKIP_PATHS = (os.path.join("ports", "doom", "src"),)

C_EXT = (".c", ".h")
HASH_EXT = (".py", ".sh", ".mk")
MD_EXT = (".md",)

TOP_MD = ("CLAUDE.md", "AGENTS.md", "Readme.md")

# Wrong at any frequency. Every one of these was either named in the register
# rule or named by a reader outside this project who had never seen it.
# A term belongs here only if it has no legitimate use in this domain. Two that
# `CLAUDE.md` named do not qualify and were moved to the review half: "the trap"
# collides with the shell builtin, which twelve scripts discuss correctly, and
# "sight" collides with the ordinary idiom "anywhere in sight". "bench" went the
# same way: `utils/dhcpmsg.c` describes a message that works on the bench and
# not in the building, which is a test bench and is the right word for it.
BANNED = (
    "load-bearing", "load bearing", "worth stating plainly", "worth noting",
    "blast radius", "tripwire", "timebox",
    "seam",
)

# Ordinary phrases, measured and never gated. A high multiple here is a question
# to ask, not a defect: teaching prose compares and negates more often than
# terse implementation comments do, which is most of what `which is` and
# `not a` measure.
WATCH = (
    "carries", "holds", "rather than", "which is", "not a", "honest",
    "the point", "in practice", "worth", "note that",
)

# This file states the rule, so it must be able to name what it bans.
# `CLAUDE.md` states it too, in one section; the rest of that file is checked
# normally.
EXEMPT_FILES = (os.path.join("tools", "prose_check.py"),)
EXEMPT_SECTION = {"CLAUDE.md": "### Register rule"}


def rel(path):
    return os.path.relpath(os.path.abspath(path), TOP)


DECORATION = re.compile(r"^[ \t]*(?:\*+|//+|#+)[ \t]?", re.M)


def normalize(text):
    """One comment, with its decoration gone and its whitespace collapsed."""
    return " ".join(DECORATION.sub(" ", text).split())


def hash_spans(text):
    """`(kind, line, body)` for `#` comments and quoted strings, line by line.

    Shell, make and Python as this tree writes them. A string spanning lines is
    not tracked, which costs nothing here and keeps this honest about what it
    is: the same per-line scan the width check already uses.
    """
    comment_lines = hash_comment_lines(text)
    for number, raw in enumerate(text.splitlines(), 1):
        quote = None
        start = 0
        i = 0
        while i < len(raw):
            ch = raw[i]
            if ch == "\\":
                i += 2
                continue
            if quote:
                if ch == quote:
                    yield ("string", number, raw[start:i])
                    quote = None
                i += 1
                continue
            if ch in "\"'":
                quote, start = ch, i + 1
                i += 1
                continue
            if ch == "#" and number in comment_lines:
                yield ("comment", number, raw[i + 1:])
                break
            i += 1


def md_spans(text, path=None):
    """Markdown paragraphs, with code and any exempt section removed."""
    skip_heading = EXEMPT_SECTION.get(rel(path)) if path else None
    para = []
    fenced = False
    exempt = False
    for raw in text.splitlines():
        if raw.strip().startswith("```"):
            fenced = not fenced
            continue
        if fenced or raw.startswith("    "):
            continue
        if skip_heading:
            if raw.startswith(skip_heading):
                exempt = True
                continue
            if exempt and raw.startswith("#"):
                exempt = False
            if exempt:
                continue
        if raw.strip():
            para.append(raw)
        elif para:
            yield ("comment", 0, " ".join(para))
            para = []
    if para:
        yield ("comment", 0, " ".join(para))


def prose_of(path, text, strings):
    """[normalized span] for one file, by language."""
    want = ("comment", "string") if strings else ("comment",)
    if path.endswith(C_EXT):
        it = c_spans(text)
    elif path.endswith(HASH_EXT):
        it = hash_spans(text)
    elif path.endswith(MD_EXT):
        it = md_spans(text, path)
    else:
        return []
    out = []
    for kind, _, body in it:
        if kind not in want:
            continue
        n = normalize(body)
        if n:
            out.append(n)
    return out


def walk(roots):
    for root in roots:
        full = root if os.path.isabs(root) else os.path.join(TOP, root)
        if os.path.isfile(full):
            yield full
            continue
        for base, dirs, names in os.walk(full):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            if any(sp in base for sp in SKIP_PATHS):
                continue
            for name in sorted(names):
                if name.endswith(C_EXT + HASH_EXT + MD_EXT):
                    yield os.path.join(base, name)


def ours(strings=False, paths=None):
    """{population: [(path, [span])]}.

    With `paths`, only those files and directories, and a path that selects
    nothing is an error: a scope that quietly matches no file reports success
    and means the opposite.
    """
    if paths:
        selected = []
        for p in paths:
            if not os.path.exists(p):
                raise SystemExit("prose_check: no such path: %s" % p)
            found = [f for f in walk([p])]
            if not found:
                raise SystemExit(
                    "prose_check: %s selects no file this tool reads" % p)
            selected.extend(found)
    else:
        selected = list(walk(ROOTS)) + [os.path.join(TOP, m) for m in TOP_MD]

    pops = {"c": [], "hash": [], "md": []}
    for path in selected:
        if not os.path.exists(path) or rel(path) in EXEMPT_FILES:
            continue
        text = open(path, errors="replace").read()
        key = ("c" if path.endswith(C_EXT)
               else "hash" if path.endswith(HASH_EXT) else "md")
        pops[key].append((path, prose_of(path, text, strings)))
    return pops


def upstream():
    """{project: [normalized C-comment span]} from the tarballs and Doom."""
    out = {}
    vendor = os.path.join(TOP, "vendor")
    if os.path.isdir(vendor):
        for name in sorted(os.listdir(vendor)):
            d = os.path.join(vendor, name)
            if not os.path.isdir(d):
                continue
            balls = [f for f in sorted(os.listdir(d))
                     if f.endswith((".tar.gz", ".tgz", ".tar.bz2", ".tar.xz"))]
            if not balls:
                continue
            spans = []
            with tarfile.open(os.path.join(d, balls[0])) as tf:
                for m in tf:
                    if not m.isfile() or not m.name.endswith(C_EXT):
                        continue
                    f = tf.extractfile(m)
                    if f is None:
                        continue
                    spans.extend(prose_of(
                        "x.c", f.read().decode("utf-8", "replace"), False))
            out[name] = spans
    doom = os.path.join(TOP, "ports", "doom", "src")
    if os.path.isdir(doom):
        spans = []
        for base, _, names in os.walk(doom):
            for n in sorted(names):
                if n.endswith(C_EXT):
                    spans.extend(prose_of(
                        "x.c", open(os.path.join(base, n),
                                    errors="replace").read(), False))
        out["doom"] = spans
    return out


def count(spans, phrase):
    """Occurrences of `phrase`, counted inside each comment separately.

    Per span, so a phrase is never found across the boundary between two
    unrelated comments, and never missed because a line break fell inside it.
    """
    if isinstance(spans, str):
        spans = [spans]
    pat = re.compile(r"\b" + re.escape(phrase) + r"\b", re.I)
    return sum(len(pat.findall(s)) for s in spans)


def words(spans):
    if isinstance(spans, str):
        spans = [spans]
    return sum(len(s.split()) for s in spans)


def cmd_banned(argv):
    pops = ours(strings=True, paths=argv or None)
    findings = 0
    for pop in ("c", "hash", "md"):
        for path, prose in pops[pop]:
            for term in BANNED:
                n = count(prose, term)
                if n:
                    print("%s: %d use(s) of \"%s\"" % (rel(path), n, term))
                    findings += n
    if findings:
        print("prose_check: %d banned term use(s)" % findings, file=sys.stderr)
        return 1
    print("prose_check: OK (no banned term in the in-house prose)")
    return 0


def cmd_report(argv):
    pops = ours()
    up = upstream()
    up_all = [span for spans in up.values() for span in spans]
    uw = words(up_all)

    print("upstream corpus, C and header comments, from the pinned tarballs:")
    for name in sorted(up):
        print("  %-12s %9d words" % (name, words(up[name])))
    print("  %-12s %9d words" % ("TOTAL", uw))
    print()

    for pop, label in (("c", "C and header comments"),
                       ("hash", "shell, make and Python comments"),
                       ("md", "Markdown prose")):
        text = [span for _, spans in pops[pop] for span in spans]
        w = words(text)
        print("%s: %d words" % (label, w))
        if pop == "c":
            print("  %-14s %8s %10s %8s %8s" %
                  ("phrase", "count", "ours/1k", "up/1k", "ratio"))
        else:
            print("  %-14s %8s %10s   (no upstream counterpart)" %
                  ("phrase", "count", "ours/1k"))
        for phrase in WATCH:
            n = count(text, phrase)
            rate = n * 1000.0 / w if w else 0.0
            if pop == "c" and uw:
                un = count(up_all, phrase)
                urate = un * 1000.0 / uw
                ratio = ("%.1fx" % (rate / urate)) if urate else "n/a"
                print("  %-14s %8d %10.2f %8.2f %8s"
                      % (phrase, n, rate, urate, ratio))
            else:
                print("  %-14s %8d %10.2f" % (phrase, n, rate))
        print()
    return 0


FIXTURE = os.path.join(TOP, "tools", "prose_upstream.txt")


def fixture_lines():
    up = upstream()
    out = ["# Upstream comment prose, counted from the pinned tarballs and the",
           "# tracked Doom sources. Regenerate deliberately, and only when a",
           "# tarball is repinned: prose_check.py --fixture --write",
           "#",
           "# project words " + " ".join(w.replace(" ", "_") for w in WATCH)]
    for name in sorted(up):
        text = up[name]
        counts = [str(count(text, w)) for w in WATCH]
        out.append("%s %d %s" % (name, words(text), " ".join(counts)))
    return out


def cmd_fixture(argv):
    """Write or verify the committed upstream counts.

    The baseline must not move silently. A repinned tarball changes it, and that
    is a deliberate act; anything else changing it means the extractor changed,
    which is exactly what this catches.
    """
    lines = fixture_lines()
    if "--write" in argv:
        open(FIXTURE, "w").write("\n".join(lines) + "\n")
        print("prose_check: wrote %s" % rel(FIXTURE))
        return 0
    if not os.path.exists(FIXTURE):
        print("prose_check: %s is missing" % rel(FIXTURE), file=sys.stderr)
        return 1
    have = open(FIXTURE).read().rstrip("\n").split("\n")
    if have != lines:
        print("prose_check: the upstream counts moved from the committed "
              "fixture", file=sys.stderr)
        for a, b in zip(have, lines):
            if a != b:
                print("  committed: %s\n  computed:  %s" % (a, b),
                      file=sys.stderr)
        return 1
    print("prose_check: fixture OK (%d projects)" % (len(lines) - 5))
    return 0


def cmd_ngrams(argv):
    """The most repeated 3- and 4-word sequences that are not on the watch list.

    A fixed watch list is beaten by a synonym and blind to the next habit. This
    is how the next one gets found without waiting for an outside reader.
    """
    top = int(argv[0]) if argv else 15
    pops = ours()
    spans = [sp for _, ss in pops["c"] + pops["md"] for sp in ss]
    toks = re.findall(r"[a-z']+", " \n ".join(spans).lower())
    seen = {}
    for n in (3, 4):
        for i in range(len(toks) - n):
            g = " ".join(toks[i:i + n])
            seen[g] = seen.get(g, 0) + 1
    watched = tuple(w.lower() for w in WATCH)
    rows = [(c, g) for g, c in seen.items()
            if c > 2 and not any(w in g for w in watched)]
    rows.sort(reverse=True)
    print("most repeated sequences not already watched:")
    for c, g in rows[:top]:
        print("  %6d  %s" % (c, g))
    return 0


SELFTEST_C = '''/* a comment naming a load-bearing thing */
const char *s = "the seam was reached";
int x = 1;  /* plain */
/* a phrase broken by a line break: rather
 * than counting it, an unnormalized scan misses it
 */
/* ends with rather */
/* than begins the next one */
'''

SELFTEST_SH = '''#!/bin/sh
# an ordinary comment
echo "the seam failed"          # a message a person reads
'''

SELFTEST_MD = '''# Doc

A sentence with worth noting in it.

```
a fenced block with load-bearing inside
```
'''


def selftest():
    import tempfile
    fails = 0
    d = tempfile.mkdtemp()

    def bad(msg):
        print("prose_check: selftest: " + msg, file=sys.stderr)

    cp = os.path.join(d, "t.c")
    open(cp, "w").write(SELFTEST_C)
    text = open(cp).read()

    only = prose_of(cp, text, False)
    if any("seam" in sp for sp in only):
        bad("a string literal leaked into comment prose"); fails += 1
    if not any("load-bearing" in sp for sp in only):
        bad("missed a banned term in a comment"); fails += 1

    both = prose_of(cp, text, True)
    if not any("seam" in sp for sp in both):
        bad("a string literal was not scanned for --banned"); fails += 1

    # Normalization: a phrase split by a line break is still one phrase.
    if count(only, "rather than") != 1:
        bad("a phrase broken across lines was not counted once"); fails += 1

    # ...but two adjacent comments do not make a phrase between them.
    if count(only, "than begins") != 1 or count(only, "rather than begins"):
        bad("a phrase was matched across two separate comments"); fails += 1

    # Decoration is not prose: no span may be a bare marker.
    if any(sp.strip() in ("*", "//", "#") for sp in only):
        bad("comment decoration was counted as a word"); fails += 1

    sp = os.path.join(d, "t.sh")
    open(sp, "w").write(SELFTEST_SH)
    sh = prose_of(sp, open(sp).read(), True)
    if not any("seam" in x for x in sh):
        bad("a shell string a person reads was not scanned"); fails += 1

    mp = os.path.join(d, "t.md")
    open(mp, "w").write(SELFTEST_MD)
    md = prose_of(mp, open(mp).read(), False)
    if any("load-bearing" in x for x in md):
        bad("a fenced block counted as Markdown prose"); fails += 1
    if not any("worth noting" in x for x in md):
        bad("missed a banned term in Markdown"); fails += 1

    # The register rule may name what it bans; nothing else may.
    claude = os.path.join(TOP, "CLAUDE.md")
    if os.path.exists(claude):
        body = prose_of(claude, open(claude).read(), False)
        if any("load-bearing" in x for x in body):
            bad("the register rule's own text was not exempted"); fails += 1

    # A scope that selects nothing is an error, not a pass: a path that does
    # not exist, and a directory holding no file this tool reads.
    empty = os.path.join(d, "empty")
    os.makedirs(empty, exist_ok=True)
    for arg in (os.path.join(d, "nope"), empty):
        try:
            ours(paths=[arg])
            bad("a scope selecting no file was accepted: %s" % arg); fails += 1
        except SystemExit:
            pass

    if fails:
        return 1
    print("prose_check: selftest OK (12 cases)")
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[-4], file=sys.stderr)
        return 2
    mode, rest = argv[1], argv[2:]
    if mode == "--selftest":
        return selftest()
    if mode == "--banned":
        return cmd_banned(rest)
    if mode == "--report":
        return cmd_report(rest)
    if mode == "--fixture":
        return cmd_fixture(rest)
    if mode == "--ngrams":
        return cmd_ngrams(rest)
    print("prose_check: unknown mode: %s" % mode, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
