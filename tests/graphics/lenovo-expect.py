"""lenovo-expect.py: turn Linux's results on the Lenovo into the per-case
expectations of the Lenovo's conformance cases (tests/native/gpu.cases).

Usage: python3 -I tests/graphics/lenovo-expect.py

Run from the repository's root. It reads ref/lenovo's IGT and piglit runs,
the tools' own output, and writes:

  tests/graphics/igt-lenovo-kms.list
      the IGT subtests Linux ran, by static name, in parts of at most CAP
      seconds of Linux's run each, a part beginning with `# part NAME`;
      a program whose subtests take longer is split among parts by subtest
  tests/graphics/ref/lenovo/expect/igt-kms.rows
      `NAME RESULT` for each subtest and dynamic subtest of the run
  tests/graphics/ref/lenovo/expect/piglit-quick.rows.gz
      `NAME RESULT` for each piglit test and subtest of the quick run, in
      piglit.sh's form, with the passes of the reruns put in place of the
      first run's results where the run's setup made the failure: the tests
      that failed only beside other work, and the EGL tests that opened the
      default display without EGL_PLATFORM, which piglit.sh sets

The SPIR-V tests stay as the first run had them: they need spirv-as, which
neither Debian's root then nor Rach2's has. The output is the same for the
same input, so a second run changes nothing.
"""
import bz2
import gzip
import json
import os

REF = "tests/graphics/ref/lenovo"
CAP = 600


def igt():
    tests = json.load(gzip.open(os.path.join(REF, "igt/results.json.gz"), "rt",
                                encoding="utf-8"))["tests"]
    rows = sorted("%s %s" % (name, t["result"]) for name, t in tests.items())
    static = sorted(n for n in tests if n.count("@") <= 2)
    seconds = {n: tests[n]["time"]["end"] - tests[n]["time"]["start"] for n in static}
    programs = {}
    for n in static:
        programs.setdefault(n.split("@")[1], []).append(n)
    # Each program is one unit, or each of its subtests one unit when the
    # program alone passes the cap; units fill parts in order.
    units = []
    for p in sorted(programs):
        names = programs[p]
        total = sum(seconds[n] for n in names)
        if total <= CAP:
            units.append((total, ["igt@%s" % p] if names == ["igt@%s" % p] else names))
        else:
            units.extend((seconds[n], [n]) for n in names)
    parts, used = [[]], 0.0
    for s, names in units:
        if parts[-1] and used + s > CAP:
            parts.append([])
            used = 0.0
        parts[-1].extend(names)
        used += s
    out = ["# igt-lenovo-kms.list: the IGT display subtests that Linux ran on the",
           "# Lenovo in the second discovery run's step 9, which lenovo.sh runs part",
           "# by part through igt.sh. Made by lenovo-expect.py from",
           "# ref/lenovo/igt/results.json.gz; do not edit.",
           "#",
           "# A line is `igt@TEST`, a program without subtests, or `igt@TEST@SUBTEST`.",
           "# A part begins with `# part NAME` and takes at most %d s of Linux's run." % CAP,
           "# Linux left out the subtests that hang the GPU or suspend the machine,",
           "# kms_pm_backlight and kms_frontbuffer_tracking but for its basic subtest."]
    for i, names in enumerate(parts, 1):
        out.append("# part kms-%d" % i)
        out.extend(names)
    with open("tests/graphics/igt-lenovo-kms.list", "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    os.makedirs(os.path.join(REF, "expect"), exist_ok=True)
    with open(os.path.join(REF, "expect/igt-kms.rows"), "w", encoding="utf-8") as f:
        f.write("\n".join(rows) + "\n")
    return len(parts), len(rows)


def piglit_rows(tests):
    rows = {}
    for name, t in tests.items():
        rows[name] = t["result"]
        for sub, result in t.get("subtests", {}).items():
            if sub != "__type__":
                rows["%s@%s" % (name, sub)] = result
    return rows


def piglit():
    def load(path):
        return json.load(bz2.open(os.path.join(REF, path), "rt", encoding="utf-8"))["tests"]
    first = load("piglit/results.json.bz2")
    rows = piglit_rows(first)
    for path in ("piglit/rerun/results.json.bz2", "piglit/rerun-egl/results.json.bz2"):
        for name, t in load(path).items():
            if t["result"] != "pass" or "spirv" in name:
                continue
            for key in [k for k in rows if k == name or k.startswith(name + "@")]:
                del rows[key]
            rows.update(piglit_rows({name: t}))
    text = "".join("%s %s\n" % (name, rows[name]) for name in sorted(rows))
    with open(os.path.join(REF, "expect/piglit-quick.rows.gz"), "wb") as f:
        f.write(gzip.compress(text.encode("utf-8"), 9, mtime=0))
    return len(rows)


def main():
    parts, igt_rows = igt()
    print("lenovo-expect: igt: %d parts, %d rows" % (parts, igt_rows))
    print("lenovo-expect: piglit: %d rows" % piglit())


if __name__ == "__main__":
    main()
