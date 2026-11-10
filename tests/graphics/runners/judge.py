"""judge.py: compare a graphics runner's result with the Debian reference's.

Usage:
  python3 -I judge.py webgl REF GOT
  python3 -I judge.py video REF GOT DECODERS-REF DECODERS-GOT [LIMITS]
  python3 -I judge.py results REF GOT
  python3 -I judge.py modes REF GOT
  python3 -I judge.py glmark2 REF GOT
  python3 -I judge.py deqp REF GOT [FLAKES]
  python3 -I judge.py vkcube REF GOT [TIMING-REF TIMING-GOT]

`webgl` compares two pages files of webgl.sh, a chunk's, by page: the lists
must name the same pages at the same indexes, and a page is worse here when
it does not pass and either passed in the reference or passed fewer of its
subtests than the reference did. A page that fails in both with as many
subtests passed, or that passes here alone, is as good.

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
a line, by name: a test or subtest that passed in the reference must pass
here, one the reference skipped must skip here, as Linux's skips are the
guest's answers about its devices, and one that failed or crashed in the
reference may end as it likes. A name the reference has and this run lacks
is worse, and so is one this run alone has that neither passes nor skips.

`modes` compares two modes files of modetest.sh by mode: the same modes in
the same order, and each mode that the reference set and read back set here
too, read back at its size with the reference's checksum.

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

Each prints `judge: worse: ...` for each difference and one line, `judge:
result: pass ...` or `judge: result: fail ...`, and exits 0 or 1.
"""
import collections
import gzip
import os
import statistics
import sys


def lines(path):
    with open(path, encoding="utf-8") as f:
        return [line.split() for line in f if line.strip() and not line.startswith("#")]


def webgl(ref_path, got_path):
    ref = {int(f[0]): f for f in lines(ref_path)}
    got = {int(f[0]): f for f in lines(got_path)}
    worse = []
    for i in sorted(ref):
        r = ref[i]
        g = got.get(i)
        if g is None:
            worse.append("%s: not run here" % r[7])
            continue
        if g[7] != r[7] and g[7] != "-":
            worse.append("page %d is %s here and %s in the reference" % (i, g[7], r[7]))
            continue
        if g[1] == "pass" or (r[1] != "pass" and int(g[2]) >= int(r[2])):
            continue
        if r[1] == "skip" and g[1] == "skip":
            continue
        worse.append("%s: debian %s, %s subtests passed; here %s, %s passed" %
                     (r[7], r[1], r[2], g[1], g[2]))
    for w in worse:
        print("judge: worse: %s" % w)
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
    """The `NAME RESULT` lines of PATH as a dictionary; a name may hold blanks."""
    with open(path, encoding="utf-8") as f:
        return dict(line.rstrip("\n").rsplit(None, 1) for line in f
                    if line.strip() and not line.startswith("#"))


def results(ref_path, got_path):
    ref, got = named(ref_path), named(got_path)
    worse = []
    for name in sorted(ref):
        r, g = ref[name], got.get(name)
        if g is None:
            worse.append("%s: not run here, %s in the reference" % (name, r))
        elif r in ("pass", "skip") and g != r:
            worse.append("%s: %s here, %s in the reference" % (name, g, r))
    for name in sorted(set(got) - set(ref)):
        if got[name] not in ("pass", "skip"):
            worse.append("%s: %s here, not in the reference" % (name, got[name]))
    for w in worse:
        print("judge: worse: %s" % w)
    verdict = "fail" if worse else "pass"
    print("judge: result: %s results %d in the reference, %d here, %d worse than Debian's" %
          (verdict, len(ref), len(got), len(worse)))
    return not worse


def modes(ref_path, got_path):
    ref, got = lines(ref_path), lines(got_path)
    worse = []
    if [f[:4] for f in ref] != [f[:4] for f in got]:
        worse.append("modes %s here, %s in the reference" %
                     (" ".join("%s@%s" % (f[2], f[3]) for f in got),
                      " ".join("%s@%s" % (f[2], f[3]) for f in ref)))
    else:
        for r, g in zip(ref, got):
            if r[4] == "set" and g[4:] != r[4:]:
                worse.append("%s mode %s %s: %s here, %s in the reference" %
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
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as f:
        return dict(line.strip().split(",", 1) for line in f if line.strip())


DEQP_PASS = ("Pass", "QualityWarning", "CompatibilityWarning")


def deqp(ref_path, got_path, flakes_path=None):
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
            worse.append("%s: %s in the reference, %s here" % (case, r, g))
    for (r, g), n in sorted(moves.items()):
        if r != g:
            print("judge: change: %s -> %s %d" % (r, g, n))
    for w in worse:
        print("judge: worse: %s" % w)
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


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "webgl":
        ok = webgl(sys.argv[2], sys.argv[3])
    elif len(sys.argv) in (6, 7) and sys.argv[1] == "video":
        ok = video(*sys.argv[2:])
    elif len(sys.argv) == 4 and sys.argv[1] in ("results", "modes", "glmark2"):
        ok = {"results": results, "modes": modes, "glmark2": glmark2}[sys.argv[1]](*sys.argv[2:4])
    elif len(sys.argv) in (4, 5) and sys.argv[1] == "deqp":
        ok = deqp(*sys.argv[2:])
    elif len(sys.argv) in (4, 6) and sys.argv[1] == "vkcube":
        ok = vkcube(*sys.argv[2:])
    else:
        sys.exit("usage: judge.py webgl REF GOT | video REF GOT DECODERS-REF DECODERS-GOT [LIMITS]"
                 " | results|modes|glmark2 REF GOT | deqp REF GOT [FLAKES]"
                 " | vkcube REF GOT [TIMING-REF TIMING-GOT]")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
