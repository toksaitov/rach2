"""judge.py: compare a graphics runner's result with the Debian reference's.

Usage:
  python3 -I judge.py webgl REF GOT [FLAKES] [-x EXCLUSIONS]
  python3 -I judge.py video REF GOT DECODERS-REF DECODERS-GOT [LIMITS]
  python3 -I judge.py results REF GOT [SAMPLE [FLAKES]] [-l LIST] [-s SCOPE]
      [-x EXCLUSIONS]
  python3 -I judge.py modes REF GOT [FAULTS]
  python3 -I judge.py glmark2 REF GOT
  python3 -I judge.py deqp REF GOT [FLAKES] [-x EXCLUSIONS]
  python3 -I judge.py vkcube REF GOT [TIMING-REF TIMING-GOT]
  python3 -I judge.py gate GATE MEASURES

`webgl` compares two pages files of webgl.sh, a chunk's, by page: the lists
must name the same pages at the same indexes, a page this run did not run
is worse, and a page omitted on one side must be omitted on the other,
since an omitted page passes no subtests and would otherwise match any
page that passed none. Of the pages both ran, one is worse here when it
does not pass and either passed in the reference or passed fewer of its
subtests than the reference did. A page that fails in both with as many
subtests passed, or that passes here alone, is as good. FLAKES names
subtests that are not judged, `PATH ERE` a line, `#` beginning a comment:
the page PATH's subtests whose message the extended regular expression ERE
matches. Each such subtest that failed in the verdict's run here, as
webgl.sh's `failures` file beside GOT names it, is counted as passed, and
the page is judged on the rest; `judge: flake: ...` names the count.

`video` compares two plays files of video.sh, a clip's, by its counted
plays: as many as the reference's, none with a media error, each showing at
least the fewest frames the reference showed, the median of the drops no
more than the most the reference dropped in one play, since browser numbers
in QEMU vary from play to play, and the decoder the same as the
reference's, the first line of each decoders file. LIMITS, video.limits,
names the clips whose drops are a known limit of Rach2, each by the name of
its plays file without `.plays`: for such a clip a median over the
reference's most is printed as `judge: limit: ...` and is not worse, and
every other check holds.

`results` compares two results files of igt.sh or piglit.sh, `NAME RESULT`
a line, compressed with gzip when the name ends in .gz, by name: a test or
subtest that passed in the reference must pass here, one the reference
skipped must skip here, as Linux's skips are the guest's answers about its
devices, and one that failed or crashed in the reference may end as it
likes. A name the reference has and this run lacks is worse, and so is one
this run alone has that neither passes nor skips. SAMPLE, the names a
list's run of piglit.sh took, its NAME.sample, keeps of the reference those
tests and their subtests, for a run of a part of the list the reference ran
whole. LIST, the list the sample was taken from, tells a subtest from a
test: piglit's names of tests may hold `@` too, so that the reference's
`T@3@2` is a test of its own beside `T@3`, and without LIST it would be
taken for a subtest of `T@3` and judged in `T@3`'s sample. A reference name
belongs to the longest of its `@` prefixes, itself included, that LIST
names, or that SAMPLE names without LIST. FLAKES names the tests and subtests that are not judged, one
extended regular expression a line, searched for in the name, `#` beginning
a comment. SCOPE, when given, names the part of both files that the run
covers, one line each: `=NAME`, the name and every name below it
(NAME@...), `~RE`, the names that the Python regular expression RE matches,
and `!RE`, names left out although another line takes them; a name in no
line of a SCOPE is not compared.

`modes` compares two modes files of modetest.sh by mode: the same modes in
the same order, and each mode that the reference set and read back set here
too, read back at its size with the reference's checksum. FAULTS,
modetest.faults, names the pictures Linux's fault spoiled in the reference,
`WxH LINUX-CRC CRC` a line: a mode the reference read back at WxH with
LINUX-CRC must be read back here with CRC, the checksum of modetest's whole
pattern, which is printed as `judge: upstream: ...` and is not worse.

`glmark2` compares two validations files of glmark2.sh by scene: the same
scenes, and each that the reference validated as Success a Success here.
The scores are recorded beside them and not judged, since they measure the
host's load as much as the guest.

`deqp` compares two results.csv files of deqp.sh, or of step 9's runs,
compressed or not, by case over the cases GOT ran, which may be a sample
of the reference's: a case is worse here when it passes in the reference
(Pass or a warning) and does not pass here, when the reference does not
support it and it fails here, or when the reference does not have it.
NotSupported here for a case that passes in the reference is worse, since
the system offers less. A case that FLAKES, deqp.sh's flakes file, names is
not worse. It also prints the count of each change of status.

`vkcube` compares two frames files of vkcube.sh by frame: the same frames
held, each read back at the reference's size with its checksum. With the
two timing files it also prints both rates and their ratio, `judge: rate:
R here, R in the reference, ratio Q`, which it does not judge, since the
rate in FIFO measures the host's load and QEMU's display as much as the
guest.

`gate` compares MEASURES, `KEY VALUE` a line, with the speed gate's file
GATE: a line `loss PERCENT`, the most a measure may lose against Linux's,
and a line `KEY VALUE higher|lower` for each measure, Linux's value and
whether a higher or a lower value is better. A measure that loses more than
PERCENT of Linux's, or has no line in GATE, fails the gate.

EXCLUSIONS, for webgl, results and deqp, is the owner's list of approved
differences, `NAME REASON` a line, NAME a page's URL, a result's name or a
case: a difference there is printed as `judge: excluded: ...` and is not
worse.

Each prints `judge: worse: ...` for each difference and one line, `judge:
result: pass ...` or `judge: result: fail ...`, and exits 0 or 1.
"""
import collections
import gzip
import os
import re
import statistics
import sys


def text(path):
    """Open PATH for reading text, through gzip when its name ends in .gz."""
    if path.endswith(".gz"):
        return gzip.open(path, "rt", encoding="utf-8")
    return open(path, encoding="utf-8")


def lines(path):
    with text(path) as f:
        return [line.split() for line in f if line.strip() and not line.startswith("#")]


def excluded(path):
    """The names of the owner's exclusions file PATH, or none without one."""
    if not path:
        return set()
    with text(path) as f:
        return {line.split()[0] for line in f if line.strip() and not line.startswith("#")}


def report(worse, exclusions, name_of):
    """Split WORSE into the differences the exclusions take and the rest,
    printing each; NAME_OF gives a difference's name."""
    left = []
    for w in worse:
        if name_of(w) in exclusions:
            print("judge: excluded: %s" % w[1])
        else:
            print("judge: worse: %s" % w[1])
            left.append(w)
    return left


def webgl_flakes(flakes_path, got_path):
    """The subtests of each page that FLAKES_PATH names and the verdict's run
    here failed, by page index: the lines of the `failures` file beside
    GOT_PATH, of each page's last run, whose message a page's expression
    matches. Without FLAKES_PATH or that file, none."""
    if not flakes_path:
        return {}
    patterns = collections.defaultdict(list)
    with open(flakes_path, encoding="utf-8") as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                path, expr = line.strip().split(None, 1)
                patterns[path].append(re.compile(expr))
    failures_path = os.path.join(os.path.dirname(got_path), "failures")
    if not os.path.exists(failures_path):
        return {}
    runs = collections.defaultdict(list)
    with open(failures_path, encoding="utf-8") as f:
        for line in f:
            index, run, message = (line.rstrip("\n").split(" ", 2) + ["", ""])[:3]
            runs[int(index), int(run)].append(message)
    last = {}
    for index, run in runs:
        last[index] = max(run, last.get(index, 0))
    got = {int(f[0]): f[7] for f in lines(got_path)}
    flaky = {}
    for index, run in last.items():
        exprs = patterns.get(got.get(index), [])
        n = sum(1 for m in runs[index, run] if any(e.search(m) for e in exprs))
        if n:
            flaky[index] = n
    return flaky


def webgl(ref_path, got_path, exclusions_path=None, flakes_path=None):
    ref = {int(f[0]): f for f in lines(ref_path)}
    got = {int(f[0]): f for f in lines(got_path)}
    flaky = webgl_flakes(flakes_path, got_path)
    worse = []
    for i in sorted(ref):
        r = ref[i]
        g = got.get(i)
        if g is None or g[1] == "notrun":
            worse.append((r[7], "%s: not run here" % r[7]))
            continue
        if g[7] != r[7]:
            worse.append((r[7], "page %d is %s here and %s in the reference" % (i, g[7], r[7])))
            continue
        if (g[1] == "omitted") != (r[1] == "omitted"):
            worse.append((r[7], "%s: omitted %s alone" %
                          (r[7], "here" if g[1] == "omitted" else "in the reference")))
            continue
        if r[1] == "omitted":
            continue
        passed, failed = int(g[2]), int(g[3])
        if g[1] == "fail" and i in flaky:
            print("judge: flake: %s: %d failed subtests named, not judged" % (r[7], flaky[i]))
            passed, failed = passed + flaky[i], failed - flaky[i]
        if g[1] == "pass" or (g[1] == "fail" and failed == 0 and int(g[5]) == 0):
            continue
        if r[1] != "pass" and passed >= int(r[2]):
            continue
        if r[1] == "skip" and g[1] == "skip":
            continue
        worse.append((r[7], "%s: debian %s, %s subtests passed; here %s, %s passed" %
                      (r[7], r[1], r[2], g[1], g[2])))
    for i in sorted(set(got) - set(ref)):
        worse.append((got[i][7], "page %d, %s, is here alone" % (i, got[i][7])))
    worse = report(worse, excluded(exclusions_path), lambda w: w[0])
    verdict = "fail" if worse else "pass"
    print("judge: result: %s webgl %d pages, %d worse than Debian's" % (verdict, len(ref), len(worse)))
    return not worse


def counted(path):
    return [f for f in lines(path) if f[1] == "0"]


def first_line(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.readline().strip()
    except OSError:
        return ""


def video(ref_path, got_path, ref_dec, got_dec, limits_path=None):
    ref, got = counted(ref_path), counted(got_path)
    clip = os.path.basename(got_path)
    if clip.endswith(".plays"):
        clip = clip[:-len(".plays")]
    limited = limits_path is not None and clip in {f[0] for f in lines(limits_path)}
    worse = []
    limit = ""
    if len(got) != len(ref):
        worse.append("%d plays counted here, %d in the reference" % (len(got), len(ref)))
    for g in got:
        if g[7] != "none":
            worse.append("play %s: %s" % (g[0], g[7]))
    shown = [int(f[2]) for f in ref if f[2] != "-"]
    for g in got:
        if g[2] == "-" or (shown and int(g[2]) < min(shown)):
            worse.append("play %s showed %s frames, the reference at least %s" %
                         (g[0], g[2], min(shown) if shown else "-"))
    dropped_ref = [int(f[3]) for f in ref if f[3] != "-"]
    dropped_got = [int(f[3]) for f in got if f[3] != "-"]
    if dropped_got and dropped_ref and statistics.median(dropped_got) > max(dropped_ref):
        over = ("dropped a median of %s frames, the reference at most %d" %
                (statistics.median(dropped_got), max(dropped_ref)))
        if limited:
            limit = ", a named limit"
            print("judge: limit: %s: %s" % (clip, over))
        else:
            worse.append(over)
    dr, dg = first_line(ref_dec), first_line(got_dec)
    if dr.split(", process:")[0] != dg.split(", process:")[0]:
        worse.append("decoder %r here, %r in the reference" % (dg, dr))
    for w in worse:
        print("judge: worse: %s" % w)
    verdict = "fail" if worse else "pass"
    print("judge: result: %s video %d plays, dropped %s here and %s in the reference%s, decoder %s" %
          (verdict, len(got), ",".join(map(str, dropped_got)) or "none",
           ",".join(map(str, dropped_ref)) or "none", limit, dg or "unknown"))
    return not worse


def named(path):
    """The `NAME RESULT` lines of PATH, gzip's or plain, as a dictionary; a
    name may hold blanks."""
    with text(path) as f:
        return dict(line.rstrip("\n").rsplit(None, 1) for line in f
                    if line.strip() and not line.startswith("#"))


def sampled(ref, sample_path, list_path=None):
    """The results of REF for the tests SAMPLE_PATH names, one a line, and
    for their subtests, `TEST@SUBTEST`. A name of REF is the test, or a
    subtest of the test, that is its longest `@` prefix, itself included,
    that LIST_PATH names, the list SAMPLE_PATH was taken from, gzip's or
    plain with `#` beginning a comment, or that SAMPLE_PATH names without
    LIST_PATH."""
    with open(sample_path, encoding="utf-8") as f:
        tests = {line.rstrip("\n") for line in f if line.strip()}
    listed = tests
    if list_path:
        with text(list_path) as f:
            listed = {line.rstrip("\n") for line in f
                      if line.strip() and not line.startswith("#")}
    kept = {}
    for name, result in ref.items():
        cut = len(name)
        while cut != -1 and name[:cut] not in listed:
            cut = name.rfind("@", 0, cut)
        if cut != -1 and name[:cut] in tests:
            kept[name] = result
    return kept


def scope(path):
    """A function that says whether a name is in the SCOPE file PATH, or
    takes every name without one."""
    if not path:
        return lambda name: True
    exact, take, leave = set(), [], []
    with text(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("="):
                exact.add(line[1:])
            elif line.startswith("~"):
                take.append(re.compile(line[1:]))
            elif line.startswith("!"):
                leave.append(re.compile(line[1:]))

    def within(name):
        parts = name.split("@")
        taken = any("@".join(parts[:i]) in exact for i in range(1, len(parts) + 1))
        taken = taken or any(r.search(name) for r in take)
        return taken and not any(r.search(name) for r in leave)
    return within


def results(ref_path, got_path, sample_path=None, flakes_path=None,
            scope_path=None, exclusions_path=None, list_path=None):
    within = scope(scope_path)
    ref = {k: v for k, v in named(ref_path).items() if within(k)}
    got = {k: v for k, v in named(got_path).items() if within(k)}
    if sample_path:
        ref = sampled(ref, sample_path, list_path)
    flakes = []
    if flakes_path:
        with open(flakes_path, encoding="utf-8") as f:
            flakes = [re.compile(line.strip()) for line in f
                      if line.strip() and not line.startswith("#")]
    flaky = {name for name in set(ref) | set(got) if any(p.search(name) for p in flakes)}
    worse = []
    for name in sorted(set(ref) - flaky):
        r, g = ref[name], got.get(name)
        if g is None:
            worse.append((name, "%s: not run here, %s in the reference" % (name, r)))
        elif r in ("pass", "skip") and g != r:
            worse.append((name, "%s: %s here, %s in the reference" % (name, g, r)))
    for name in sorted(set(got) - set(ref) - flaky):
        if got[name] not in ("pass", "skip"):
            worse.append((name, "%s: %s here, not in the reference" % (name, got[name])))
    worse = report(worse, excluded(exclusions_path), lambda w: w[0])
    verdict = "fail" if worse or not got else "pass"
    print("judge: result: %s results %d in the reference, %d here, %d worse than Debian's%s" %
          (verdict, len(ref), len(got), len(worse),
           ", %d flakes named" % len(flaky) if flakes_path else ""))
    return verdict == "pass"


def modes(ref_path, got_path, faults_path=None):
    ref, got = lines(ref_path), lines(got_path)
    faults = {(f[0], f[1]): f[2] for f in lines(faults_path)} if faults_path else {}
    worse = []
    if [f[:4] for f in ref] != [f[:4] for f in got]:
        worse.append("modes %s here, %s in the reference" %
                     (" ".join("%s@%s" % (f[2], f[3]) for f in got),
                      " ".join("%s@%s" % (f[2], f[3]) for f in ref)))
    else:
        for r, g in zip(ref, got):
            if r[4] != "set":
                continue
            want = r[4:]
            right = faults.get((r[5], r[6])) if len(r) == 7 else None
            if right is not None:
                want = [r[4], r[5], right]
            if g[4:] != want:
                worse.append("%s mode %s %s: %s here, %s in the reference%s" %
                             (r[0], r[1], r[2], " ".join(g[4:]), " ".join(r[4:]),
                              ", whose whole pattern is %s" % right if right else ""))
            elif right is not None:
                print("judge: upstream: %s mode %s %s: %s here, %s in the reference,"
                      " where Linux's mode set cleared its top" %
                      (r[0], r[1], r[2], " ".join(g[4:]), " ".join(r[4:])))
    for w in worse:
        print("judge: worse: %s" % w)
    verdict = "fail" if worse else "pass"
    print("judge: result: %s modes %d in the reference, %d here, %d worse than Debian's" %
          (verdict, len(ref), len(got), len(worse)))
    return not worse


def glmark2(ref_path, got_path):
    ref = {f[0]: f[1] for f in lines(ref_path)}
    got = {f[0]: f[1] for f in lines(got_path)}
    worse = []
    for scene in sorted(ref):
        g = got.get(scene)
        if g is None:
            worse.append("%s: not run here" % scene)
        elif ref[scene] == "Success" and g != "Success":
            worse.append("%s: %s here, Success in the reference" % (scene, g))
    for w in worse:
        print("judge: worse: %s" % w)
    verdict = "fail" if worse else "pass"
    print("judge: result: %s glmark2 %d scenes, %d validated here, %d worse than Debian's" %
          (verdict, len(ref), sum(1 for v in got.values() if v == "Success"), len(worse)))
    return not worse


def statuses(path):
    """Map each case of a results.csv file, gzip's or plain, to its status."""
    with text(path) as f:
        return dict(line.strip().split(",", 1) for line in f if line.strip())


DEQP_PASS = ("Pass", "QualityWarning", "CompatibilityWarning")


def deqp(ref_path, got_path, flakes_path=None, exclusions_path=None):
    ref, got = statuses(ref_path), statuses(got_path)
    flakes = set()
    if flakes_path:
        with open(flakes_path, encoding="utf-8") as f:
            flakes = {line.split(",", 1)[0] for line in f if line.strip()}
    moves = collections.Counter()
    worse = []
    for case, g in got.items():
        r = ref.get(case, "Absent")
        moves[(r, g)] += 1
        if g in DEQP_PASS or case in flakes:
            continue
        if r in DEQP_PASS or r == "Absent" or (r == "NotSupported" and g != "NotSupported"):
            worse.append((case, "%s: %s in the reference, %s here" % (case, r, g)))
    for (r, g), n in sorted(moves.items()):
        if r != g:
            print("judge: change: %s -> %s %d" % (r, g, n))
    worse = report(worse, excluded(exclusions_path), lambda w: w[0])
    ok = bool(got) and not worse
    print("judge: result: %s deqp %d cases, %d worse than the reference's, %d flakes named" %
          ("pass" if ok else "fail", len(got), len(worse), len(flakes & set(got))))
    return ok


def rate(path):
    """The `rate R` line of a timing file of vkcube.sh, as a number."""
    for f in lines(path):
        if f[0] == "rate":
            return float(f[1])
    return None


def vkcube(ref_path, got_path, ref_timing=None, got_timing=None):
    ref = {f[1]: f[2:] for f in lines(ref_path)}
    got = {f[1]: f[2:] for f in lines(got_path)}
    worse = []
    for frame in sorted(ref, key=int):
        g = got.get(frame)
        if g is None:
            worse.append("frame %s: not read here" % frame)
        elif g != ref[frame]:
            worse.append("frame %s: %s here, %s in the reference" %
                         (frame, " ".join(g), " ".join(ref[frame])))
    for frame in sorted(set(got) - set(ref), key=int):
        worse.append("frame %s: read here alone" % frame)
    for w in worse:
        print("judge: worse: %s" % w)
    if ref_timing is not None:
        r, g = rate(ref_timing), rate(got_timing)
        print("judge: rate: %s here, %s in the reference, ratio %s" %
              (g, r, "%.2f" % (g / r) if r and g is not None else "none"))
    verdict = "fail" if worse else "pass"
    print("judge: result: %s vkcube %d frames in the reference, %d here, %d worse than Debian's" %
          (verdict, len(ref), len(got), len(worse)))
    return bool(got) and not worse


def gate(gate_path, measures_path):
    loss, linux = None, {}
    for f in lines(gate_path):
        if f[0] == "loss" and len(f) == 2:
            loss = float(f[1]) / 100
        elif len(f) == 3 and f[2] in ("higher", "lower"):
            linux[f[0]] = (float(f[1]), f[2])
        else:
            sys.exit("judge: %s: a line that is neither `loss PERCENT` nor `KEY VALUE higher|lower`: %s"
                     % (gate_path, " ".join(f)))
    if loss is None:
        sys.exit("judge: %s: no `loss` line" % gate_path)
    measures = [f for f in lines(measures_path) if len(f) == 2]
    worse = []
    for key, value in measures:
        value = float(value)
        if key not in linux:
            worse.append("%s: %g here, no value of Linux's in the gate" % (key, value))
            continue
        ref, better = linux[key]
        ratio = value / ref if better == "higher" else (ref / value if value else 0.0)
        line = "%s: %g here, %g on Linux, %.1f%% of Linux's" % (key, value, ref, 100 * ratio)
        if ratio < 1 - loss:
            worse.append(line)
        else:
            print("judge: within: %s" % line)
    for w in worse:
        print("judge: worse: %s" % w)
    ok = bool(measures) and not worse
    print("judge: result: %s gate %d measures, %d losing more than %g%% of Linux's" %
          ("pass" if ok else "fail", len(measures), len(worse), 100 * loss))
    return ok


def main():
    args, opts = [], {}
    rest = sys.argv[1:]
    while rest:
        a = rest.pop(0)
        if a in ("-l", "-s", "-x") and rest:
            opts[a] = rest.pop(0)
        else:
            args.append(a)
    x, s, l = opts.get("-x"), opts.get("-s"), opts.get("-l")
    kind = args[0] if args else ""
    if l and (kind != "results" or len(args) < 4):
        args = []
    if len(args) in (3, 4) and kind == "webgl" and not s:
        ok = webgl(args[1], args[2], x, args[3] if len(args) == 4 else None)
    elif len(args) in (5, 6) and kind == "video" and not opts:
        ok = video(*args[1:])
    elif len(args) in (3, 4, 5) and kind == "results":
        ok = results(*args[1:], scope_path=s, exclusions_path=x, list_path=l)
    elif len(args) in (3, 4) and kind == "modes" and not opts:
        ok = modes(*args[1:])
    elif len(args) == 3 and kind == "glmark2" and not opts:
        ok = glmark2(*args[1:3])
    elif len(args) in (3, 4) and kind == "deqp" and not s:
        ok = deqp(args[1], args[2], args[3] if len(args) == 4 else None, x)
    elif len(args) in (3, 5) and kind == "vkcube" and not opts:
        ok = vkcube(*args[1:])
    elif len(args) == 3 and kind == "gate" and not opts:
        ok = gate(args[1], args[2])
    else:
        sys.exit("usage: judge.py webgl REF GOT [FLAKES] [-x EXCLUSIONS]"
                 " | video REF GOT DECODERS-REF DECODERS-GOT [LIMITS]"
                 " | results REF GOT [SAMPLE [FLAKES]] [-l LIST] [-s SCOPE] [-x EXCLUSIONS]"
                 " | modes REF GOT [FAULTS] | glmark2 REF GOT"
                 " | deqp REF GOT [FLAKES] [-x EXCLUSIONS]"
                 " | vkcube REF GOT [TIMING-REF TIMING-GOT] | gate GATE MEASURES")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
