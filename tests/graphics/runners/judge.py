"""judge.py: compare a browser runner's result with the Debian reference's.

Usage:
  python3 -I judge.py webgl REF GOT
  python3 -I judge.py video REF GOT DECODERS-REF DECODERS-GOT
  python3 -I judge.py results REF GOT
  python3 -I judge.py modes REF GOT
  python3 -I judge.py glmark2 REF GOT

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
reference's, the first line of each decoders file.

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

Each prints `judge: worse: ...` for each difference and one line, `judge:
result: pass ...` or `judge: result: fail ...`, and exits 0 or 1.
"""
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


def video(ref_path, got_path, ref_dec, got_dec):
    ref, got = counted(ref_path), counted(got_path)
    worse = []
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
        worse.append("dropped a median of %s frames, the reference at most %d" %
                     (statistics.median(dropped_got), max(dropped_ref)))
    dr, dg = first_line(ref_dec), first_line(got_dec)
    if dr.split(", process:")[0] != dg.split(", process:")[0]:
        worse.append("decoder %r here, %r in the reference" % (dg, dr))
    for w in worse:
        print("judge: worse: %s" % w)
    verdict = "fail" if worse else "pass"
    print("judge: result: %s video %d plays, dropped %s here and %s in the reference, decoder %s" %
          (verdict, len(got), ",".join(map(str, dropped_got)) or "none",
           ",".join(map(str, dropped_ref)) or "none", dg or "unknown"))
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


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "webgl":
        ok = webgl(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 6 and sys.argv[1] == "video":
        ok = video(*sys.argv[2:6])
    elif len(sys.argv) == 4 and sys.argv[1] in ("results", "modes", "glmark2"):
        ok = {"results": results, "modes": modes, "glmark2": glmark2}[sys.argv[1]](*sys.argv[2:4])
    else:
        sys.exit("usage: judge.py webgl REF GOT | video REF GOT DECODERS-REF DECODERS-GOT"
                 " | results|modes|glmark2 REF GOT")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
