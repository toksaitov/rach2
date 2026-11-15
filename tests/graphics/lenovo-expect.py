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
      piglit.sh's form, with the passes of the EGL reruns put in place of
      the first run's results: those tests opened the default display
      without EGL_PLATFORM, which piglit.sh sets

The 1,033 compiler tests skipped before execution by GL_EXT_gpu_shader4's
fast-skip check, and two EGL tests skipped for absent displays, accept
`skip-or-pass`. These are setup skips, not answers about the kernel's device.
IGT's absent sw_sync device and the cursor, extended-mode and feature
discovery tests that found no second connected output use the same rule.
The owner's deferred DRM leases, msr device, runtime PM and vgem use `xfail`
with a comment naming the deferral; a missing parent test still fails.

The tests that failed beside the first run's other tests and passed alone
stay as the first run had them, since piglit.sh runs a test on each CPU
through GBM as that run did; arb_timer_query@timestamp-get is one, whose
query waits behind the other tests' batches on the render ring. The SPIR-V
tests stay too: they need spirv-as, which neither Debian's root then nor
Rach2's has. The output is the same for the same input, so a second run
changes nothing.
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
    rows = [igt_row(name, tests[name]) for name in sorted(tests)]
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


def igt_row(name, test):
    """Write a reference row, retaining the owner's deferred requirements."""
    deferred = {
        "igt@kms_lease": "DRM leases (#5231)",
        "igt@kms_pm_rpm": "msr device and i915 runtime PM (#5235)",
        "igt@kms_vblank@ts-continuation-dpms-rpm": "i915 runtime PM (#5235)",
        "igt@kms_vblank@ts-continuation-modeset-rpm": "i915 runtime PM (#5235)",
        "igt@kms_prime@basic-crc-vgem": "vgem (#5236)",
    }
    if test["result"] == "pass":
        for prefix, reason in deferred.items():
            if name == prefix or name.startswith(prefix + "@"):
                return "# deferred: %s\n%s xfail" % (reason, name)
    result = "skip-or-pass" if igt_setup_skip(name, test) else test["result"]
    return "%s %s" % (name, result)


def igt_setup_skip(name, test):
    """Identify the recorded missing-device and second-output setup skips."""
    if test["result"] != "skip":
        return False
    out = test.get("out", "")
    if ("Test requirement not met in function igt_require_sw_sync," in out
            and "Test requirement: kernel_has_sw_sync()\n" in out
            and "Last errno: 2, No such file or directory\n" in out):
        return True
    if name.startswith("igt@kms_cursor_legacy@"):
        return ("Test requirement not met in function find_connected_pipe," in out
                and "Test requirement: found\nNo second valid output found\n" in out)
    # The reference had only eDP-1; the native run also has HDMI-A-1.
    if name == "igt@kms_feature_discovery@display-2x":
        return "Test requirement: output_count >= 2\n" in out
    return (name == "igt@kms_display_modes@extended-mode-basic"
            and "Test requirement: count > 1\nMinimum 2 outputs are required\n" in out)


def piglit_rows(tests):
    rows = {}
    for name, t in tests.items():
        rows[name] = t["result"]
        for sub, result in t.get("subtests", {}).items():
            if sub != "__type__":
                rows["%s@%s" % (name, sub)] = result
    return rows


def piglit_setup_skip(name, test):
    """Identify step 9's setup skips from their recorded diagnostics."""
    if test["result"] != "skip":
        return False
    if (test.get("returncode") is None and not test.get("pid")
            and test.get("out") ==
            "Test requires extension GL_EXT_gpu_shader4 which is not available"):
        return True
    return ((name == "spec@egl_ext_client_extensions@conformance test 3"
             and test.get("out") == "Failed to initialize default display\n")
            or (name == "spec@egl_ext_surface_compression@create"
                and test.get("out") ==
                "piglit: error: failed to connect to Wayland display\n\n"))


def piglit():
    def load(path):
        return json.load(bz2.open(os.path.join(REF, path), "rt", encoding="utf-8"))["tests"]
    first = load("piglit/results.json.bz2")
    rows = piglit_rows(first)
    for name, t in load("piglit/rerun-egl/results.json.bz2").items():
        if t["result"] == "pass":
            for key in [k for k in rows if k == name or k.startswith(name + "@")]:
                del rows[key]
            rows.update(piglit_rows({name: t}))
    for name, test in first.items():
        if rows.get(name) == "skip" and piglit_setup_skip(name, test):
            rows[name] = "skip-or-pass"
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
