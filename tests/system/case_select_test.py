#!/usr/bin/env python3
"""case_select_test.py: the cases tests/run chooses, and the choices it refuses.

Usage: python3 -B tests/system/case_select_test.py [SELECTOR [RUNNER]]

The rows of `choose` run SELECTOR, tests/system/case_select.py by default,
on manifests of its own: RACH2_CASES's identities run in the manifest's
order, the agents' rows in the order given, and a refused choice exits 2,
says why and writes nothing. The rows of `run` run RUNNER, tests/run by
default, with SELECTOR in a private tree of two suites whose make records
each call and fails: a refused RACH2_CASES exits 2 before anything is made
or the tree's build directory exists, and an accepted one says on the start
line that the run is a selection, and goes on to make. Each row prints
`case-select-test: result: pass: ROW` or `fail: ROW`, and the test exits 1
when one fails.
"""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]
FAILED = []


def result(row, ok, detail=""):
    """Report ROW, with DETAIL on a line of its own when it failed."""
    if ok:
        print(f"case-select-test: result: pass: {row}")
        return
    print(f"case-select-test: result: fail: {row}")
    if detail:
        print(f"case-select-test: detail: {row}: {detail}")
    FAILED.append(row)


def manifest(family, cases):
    """Give a manifest of FAMILY with CASES, NAME=ARCHITECTURES each."""
    return {"family_id": family, "cases": [
        {"case_id": family + "." + name, "architectures": archs} for name, archs in cases]}


BOTH = ["i686", "x86_64"]
FIXTURES = {
    "alpha": manifest("host.alpha", [("a", BOTH), ("b", ["i686"]), ("c", "none")]),
    "beta": manifest("host.beta", [("d", BOTH)]),
    "agents": manifest("machine.agents", [("x", BOTH), ("y", BOTH), ("z", BOTH)]),
}


def ids(path):
    """Give the case identities of the manifest at PATH."""
    return [case["case_id"] for case in json.loads(path.read_text())["cases"]]


def choose_rows(selector, scratch):
    """Run the selector's rows on FIXTURES under SCRATCH."""
    cases = scratch / "cases"
    cases.mkdir()
    for doc in FIXTURES.values():
        (cases / (doc["family_id"] + ".json")).write_text(json.dumps(doc))

    def select(arch, suites, *options):
        out = scratch / "out"
        shutil.rmtree(out, ignore_errors=True)
        p = subprocess.run([sys.executable, "-B", str(selector), *options, arch, str(cases),
                            str(out), *suites], capture_output=True, text=True, check=False)
        return p, out

    for row, options, suites, wanted, count in [
            ("manifest-order", ["-c", "host.alpha.c host.alpha.a"], ["alpha"],
             {"alpha": ["host.alpha.a", "host.alpha.c"]}, "2"),
            ("rows-order", ["-r", "agents", "z x"], ["agents"],
             {"agents": ["machine.agents.z", "machine.agents.x"]}, "2"),
            ("both", ["-c", "host.alpha.a", "-r", "agents", "y"], ["alpha", "agents"],
             {"alpha": ["host.alpha.a"], "agents": ["machine.agents.y"]}, "2"),
            ("rows-elsewhere", ["-r", "agents", "x"], ["alpha"], {}, "")]:
        p, out = select("i686", suites, *options)
        got = {f.stem: ids(f) for f in out.glob("*.json")} if out.is_dir() else {}
        result(f"choose {row}", p.returncode == 0 and got == wanted and p.stdout.strip() == count,
               f"status {p.returncode}, chose {got}, printed {p.stdout.strip()!r}, {p.stderr.strip()}")

    for row, arch, options, suites in [
            ("unknown", "i686", ["-c", "host.alpha.nope"], ["alpha"]),
            ("repeated", "i686", ["-c", "host.alpha.a host.alpha.a"], ["alpha"]),
            ("empty-suite", "i686", ["-c", "host.alpha.a"], ["alpha", "beta"]),
            ("other-suite", "i686", ["-c", "host.alpha.a machine.agents.x"], ["alpha"]),
            ("architecture", "x86_64", ["-c", "host.alpha.b"], ["alpha"]),
            ("twice", "i686", ["-c", "machine.agents.x", "-r", "agents", "y"], ["agents"]),
            ("rows-empty", "i686", ["-r", "agents", ""], ["agents"]),
            ("rows-repeated", "i686", ["-r", "agents", "x x"], ["agents"]),
            ("rows-unknown", "i686", ["-r", "agents", "x nope"], ["agents"])]:
        p, out = select(arch, suites, *options)
        wrote = out.is_dir() and any(out.iterdir())
        result(f"refuse {row}", p.returncode == 2 and not wrote and "case-select: " in p.stderr,
               f"status {p.returncode}, wrote {wrote}, said {p.stderr.strip()!r}")


TREE_FILES = ["tests/system/runlock.sh", "tests/system/runner/runutil.c",
              "tools/setup/e2fsprogs_path.sh", "tools/setup/hostcheck.c",
              "tools/setup/ovmf_path.sh"]


def run_rows(selector, runner, scratch):
    """Run the runner's rows in a private tree under SCRATCH."""
    tree = scratch / "tree"
    for f in TREE_FILES:
        (tree / f).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPO / f, tree / f)
    shutil.copy(runner, tree / "tests/run")
    shutil.copy(selector, tree / "tests/system/case_select.py")
    (tree / "tests/registry").write_text("style|-|full||128\nhost|-|full||128\n")
    (tree / "tests/cases").mkdir()
    for doc in [manifest("host.style", [("s1", BOTH), ("s2", BOTH)]),
                manifest("host.host", [("h1", BOTH), ("h2", BOTH)])]:
        (tree / "tests/cases" / (doc["family_id"] + ".json")).write_text(json.dumps(doc))
    made = scratch / "made"
    bin_dir = scratch / "bin"
    bin_dir.mkdir()
    (bin_dir / "make").write_text(f"#!/bin/sh\necho \"make $*\" >> '{made}'\nexit 1\n")
    (bin_dir / "make").chmod(0o755)
    base = {k: v for k, v in os.environ.items() if not k.startswith(("RACH2_", "ETUDE_"))}
    base.update(PATH=f"{bin_dir}{os.pathsep}{os.environ['PATH']}", ARCH="i686",
                ETUDE_SITE="/nonexistent", ETUDE_ADMITTED="/nonexistent",
                RACH2_QEMU_ACCEL="tcg", RACH2_WORKERS="1", TMPDIR=str(scratch))

    def run(cases):
        env = dict(base)
        if cases is not None:
            env["RACH2_CASES"] = cases
        if made.exists():
            made.unlink()
        shutil.rmtree(tree / "build", ignore_errors=True)
        p = subprocess.run(["sh", "tests/run", "style", "host"], cwd=tree, env=env,
                           capture_output=True, text=True, check=False, timeout=50)
        return p, p.stdout + p.stderr

    for row, cases in [("unknown", "host.style.s1 host.host.nope"),
                       ("repeated", "host.style.s1 host.style.s1 host.host.h1"),
                       ("empty-suite", "host.style.s1")]:
        p, text = run(cases)
        result(f"run refuse {row}", p.returncode == 2 and not made.exists() and
               not (tree / "build").exists() and "run: result: refused: the cases chosen" in text,
               f"status {p.returncode}, made {made.exists()}: {text.strip()[-400:]!r}")

    for row, cases, line in [("selection", "host.host.h1 host.style.s2 host.style.s1",
                              ", a selection of 3 cases)"),
                             ("every-case", None, "")]:
        p, text = run(cases)
        start = [l for l in text.splitlines() if l.startswith("run: starting: style host (")]
        said = bool(start) and (line in start[0] if line else "a selection" not in start[0])
        result(f"run {row}", said and made.exists() and "recovery-key" in made.read_text(),
               f"status {p.returncode}, start {start}: {text.strip()[-400:]!r}")


def main():
    """Run every row; exit 1 when one fails."""
    for number, status in [(signal.SIGHUP, 129), (signal.SIGINT, 130), (signal.SIGTERM, 143)]:
        signal.signal(number, lambda *_, status=status: sys.exit(status))
    selector = Path(sys.argv[1] if len(sys.argv) > 1 else REPO / "tests/system/case_select.py").resolve()
    runner = Path(sys.argv[2] if len(sys.argv) > 2 else REPO / "tests/run").resolve()
    with tempfile.TemporaryDirectory(prefix="rach2-case-select.") as scratch:
        (Path(scratch) / "choose").mkdir()
        (Path(scratch) / "run").mkdir()
        choose_rows(selector, Path(scratch) / "choose")
        run_rows(selector, runner, Path(scratch) / "run")
    if FAILED:
        print(f"case-select-test: result: failed: {len(FAILED)} rows")
        return 1
    print("case-select-test: result: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
