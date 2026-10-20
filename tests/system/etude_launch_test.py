#!/usr/bin/env python3
"""Check installed admission before real build, worker, owner and CI entries."""

import json
import fcntl
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

stage = "setup"


def write(path, text, executable=False):
    """Write a private fixture, making its parent directories first."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    if executable:
        path.chmod(0o755)


def run(argv, env, cwd, status=0, input=None):
    """Run a bounded fixture and require its status, retaining its diagnostics."""
    p = subprocess.Popen(argv, env=env, cwd=cwd, text=True,
                         stdin=subprocess.PIPE if input is not None else None,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        output, _ = p.communicate(input=input, timeout=40)
        assert p.returncode == status, (argv, p.returncode, output)
        return output
    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
                p.communicate()


def until(predicate):
    """Wait for a fixture's published state within ten seconds."""
    end = time.monotonic() + 10
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("fixture did not publish its state")


def rows(root, repo, sources):
    """Observe durable claims and actual compatibility locks at each entry."""
    global stage
    env = dict(os.environ)
    for key in ("ETUDE_ADMITTED", "ETUDE_SITE", "RACH2_PORT_OFFSET", "SSH_TEST_PORT",
                "RACH2_RUN_GROUP", "RACH2_RUN_WORK", "RACH2_RUN_UTIL",
                "RACH2_RUN_TREE_LOCK", "RACH2_RUN_STATUS", "MAKEFLAGS", "MFLAGS",
                "MAKELEVEL", "MAKEOVERRIDES"):
        env.pop(key, None)
    bridge = repo / "build/tools/etude/etude-bridge"
    ledger = root / "ledger"
    locks = root / "locks"
    tree = root / "tree"
    bin = root / "bin"
    launcher = root / "launch"
    site = root / "site"
    observed = root / "observed"
    observer = root / "observe.py"
    for directory in (ledger, locks, tree, bin):
        directory.mkdir()
    write(ledger / "claims", "etude-ledger 1\n")
    shutil.copyfile(sources / "tools/etude/launch.sh", launcher)
    launcher.chmod(0o755)
    policy = (f"etude-site 1\nendpoint {'macos' if sys.platform == 'darwin' else 'linux'}\n"
              f"launcher {launcher}\nledger {ledger}\nlocks {locks}\nbridge {bridge}\n"
              "capacity 8000,134217728,1048576\nwait 3\nseconds 30\n")
    write(site, policy)
    env.update(ETUDE_SITE=str(site), ETUDE_LAUNCH_LEDGER=str(ledger),
               ETUDE_LAUNCH_OBSERVED=str(observed), ETUDE_LAUNCH_LOCKS=str(locks),
               ETUDE_STATE=str(root / "private-state"),
               PATH=str(bin) + os.pathsep + env["PATH"])
    # The command must already have a durable, registered reservation and
    # the matching port and host locks when its first instruction executes.
    write(observer, '''#!/usr/bin/env python3
import fcntl, json, os
from pathlib import Path
ledger = Path(os.environ["ETUDE_LAUNCH_LEDGER"])
text = (ledger / "claims").read_text()
assert text.count("claim ") == 1, text
assert "process case " in text and "state running" in text, text
assert "resource cpu_millis 8000\\n" in text, text
assert "resource memory_bytes 134217728\\n" in text, text
assert "resource disk_bytes 1048576\\n" in text, text
assert os.environ["RACH2_WORKERS"] == "2"
assert os.environ["RACH2_MEMORY_MIB"] == "96"
offset = int(os.environ["RACH2_PORT_OFFSET"])
assert offset % 1000 == 0 and 0 <= offset <= 9000
locks = Path(os.environ["ETUDE_LAUNCH_LOCKS"])
for name in ("rach2-host.lock", "rach2-ports." + str(offset // 1000)):
    with (locks / name).open("r+") as f:
        try:
            fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            pass
        else:
            raise AssertionError("reservation lock is missing: " + name)
with Path(os.environ["ETUDE_LAUNCH_OBSERVED"]).open("a") as f:
    f.write(json.dumps({"claim": text, "offset": offset}) + "\\n")
''', executable=True)
    observe = f"'{sys.executable}' '{observer}'"

    def clear():
        """Start a row after all prior claims and observations are gone."""
        assert (ledger / "claims").read_text() == "etude-ledger 1\n"
        observed.unlink(missing_ok=True)

    def saw(count=1, same=True):
        """Require entries to share one claim, or to have one per command."""
        records = [json.loads(line) for line in observed.read_text().splitlines()]
        assert len(records) == count, records
        assert len({r["claim"] for r in records}) == (1 if same else count), records
        assert (ledger / "claims").read_text() == "etude-ledger 1\n"

    write(tree / "tests/system/suites.mk", "# RACH2_PORT_OFFSET\n")
    write(tree / "tests/registry", "style|-|full\nhost|-|full\ndefault|qemu|core\n")
    prefix = (sources / "Makefile").read_text().split("# The architecture (mk/arch.mk):")[0]
    write(tree / "Makefile", prefix + f"all:\n\t@{observe}\n\t@test '$(ARG)' = 'two words'\nendif\n")
    (tree / "tools/etude").mkdir(parents=True)
    shutil.copyfile(launcher, tree / "tools/etude/launch.sh")
    stage = "build"
    run([shutil.which("make"), "ARG=two words"], env, tree)
    saw()
    clear()

    # The real worker entry must claim before even its helper compilation.
    stage = "worker"
    shutil.copyfile(sources / "tests/run", tree / "tests/run")
    write(bin / "cc", f"#!/bin/sh\n{observe}\nexit 19\n", executable=True)
    run(["sh", "tests/run", "style"], env, tree, status=1)
    saw()
    clear()

    # Ordinary CI must claim before make and retain that claim across tests.
    stage = "ci"
    write(bin / "make", f"#!/bin/sh\nexec {observe}\n", executable=True)
    write(tree / "tests/run", f'''#!/bin/sh
{observe} || exit 3
for name do
    [ "$name" != core ] || name=default
    echo "run: $name: PASS in 0s"
done
''', executable=True)
    write(tree / "build/tests/failed/.keep", "")
    run(["sh", str(sources / "tools/ci/run.sh"), "core", str(root / "ci")], env, tree)
    saw(4)
    clear()

    # The owner transport must claim arbitrary commands before they build,
    # preserving quoting, standard input and the command's own status.
    stage = "owner"
    testing = root / "owner/tools/dev/testing.sh"
    testing.parent.mkdir(parents=True)
    shutil.copyfile(sources / "tools/dev/testing.sh", testing)
    write(bin / "ssh", '''#!/bin/sh
case $4 in *'bash -s '*) echo held; exec cat >/dev/null ;; esac
exec bash -c "$4"
''', executable=True)
    write(bin / "wsl.exe", '#!/bin/sh\nshift\nexec "$@"\n', executable=True)
    write(bin / "rsync", '#!/bin/sh\n[ "$1" != --version ] || echo "rsync  version 3.2.7"\n', executable=True)
    env["RACH2_TESTING_COPY"] = str(tree)
    output = run(["sh", str(testing), "run", observe + "; printf '%s\\n' \"a'b\"; cat; exit 7"],
                 env, tree, status=7, input="owner input\n")
    assert "a'b\nowner input\n" in output, output
    saw()
    clear()

    trailing = observe + "; [ \"${BASH_EXECUTION_STRING: -1}\" = $'\\n' ] || exit 9\n"
    run(["sh", str(testing), "run", trailing], env, tree)
    saw()
    clear()

    # An old checkout is admitted from outside its source, with no nested
    # runutil lock. A fixture helper attempts the same lock tests/run takes.
    runutil = repo / "build/tests/system/runutil"
    program = root / "old-run"
    stage = "legacy"
    write(program, f'''#!/bin/sh
if [ -n "${{RACH2_PORT_OFFSET:-}}" ]; then exec {observe}; fi
exec '{runutil}' ports '{locks}' 2 '{sys.executable}' '{observer}'
''',
          executable=True)
    result = root / "result"
    with (locks / "rach2-ports.0").open("r+") as occupied:
        fcntl.lockf(occupied, fcntl.LOCK_EX)
        run([str(launcher), "-r", str(result), "legacy", str(program)], env, tree)
    assert json.loads(observed.read_text())["offset"] == 1000
    assert result.read_text() == "exited 0\n"
    saw()
    clear()

    # An external bisection must choose the installed site even when a
    # private machine record would offer a different ledger and capacity.
    stage = "bisection"
    private = root / "private-ledger"
    write(private / "claims", "etude-ledger 1\n")
    write(root / "private-state/machine",
          f"etude-machine 1\nledger {private}\ncpus 16\nmemory_bytes 268435456\n")
    write(bin / "gh", '''#!/bin/sh
case "$*" in *'--json state '*) echo OPEN ;; esac
''', executable=True)
    write(bin / "git", '''#!/bin/sh
case $1 in rev-parse) echo bad ;; rev-list) echo good ;; log) echo 'bad fixture' ;; checkout) : ;; *) exit 2 ;; esac
''', executable=True)
    write(bin / "timeout", '#!/bin/sh\n[ "$1" != -k ] || shift 2\nshift\nexec "$@"\n', executable=True)
    env.update(GITHUB_REPOSITORY="test/rach2", GITHUB_RUN_ID="1", RUNNER_TEMP=str(root / "ci-temp"))
    run(["sh", str(sources / "tools/ci/bisect.sh"), "-e", str(bridge),
         "core", "i686", "good", "bad", "1", "default"],
        env, tree)
    saw(2, same=False)
    clear()

    # A command nested in an admitted owner command reuses its claim and
    # still supplies the separate result that bisection reads.
    stage = "nested"
    nested = f"{observe} && '{launcher}' -r '{result}' legacy '{sys.executable}' '{observer}'"
    run([str(launcher), "build", "/bin/sh", "-c", nested], env, tree)
    assert result.read_text() == "exited 0\n"
    saw(2)
    clear()
    signalled = f"{observe} && '{launcher}' -r '{result}' legacy /bin/sh -c 'kill -TERM $$'"
    run([str(launcher), "build", "/bin/sh", "-c", signalled], env, tree, status=143)
    assert result.read_text() == "incomplete\n"
    saw()
    clear()
    write(tree / "tests/system/suites.mk", "# fixed ports\n")
    nested_old = f"'{launcher}' -r '{result}' legacy '{sys.executable}' '{observer}'"
    run([str(launcher), "build", "/bin/sh", "-c", nested_old], env, tree, status=2)
    assert result.read_text() == "refused\n" and not observed.exists()
    clear()
    write(tree / "tests/system/suites.mk", "# RACH2_PORT_OFFSET\n")

    # Refusals may not reach the command, including a missing explicitly
    # selected policy and inherited port settings that would bypass locks.
    stage = "refusals"
    for name, bad_policy in (
        ("format", policy.replace("etude-site 1", "etude-site 2")),
        ("endpoint", policy.replace("endpoint linux", "endpoint windows").replace("endpoint macos", "endpoint windows")),
        ("bridge", policy.replace(str(bridge), str(root / "missing-bridge"))),
        ("capacity", policy.replace("8000,134217728,1048576", "8000,0,1048576")),
        ("duplicate", policy + "capacity 1,1,1\n"),
    ):
        write(site, bad_policy)
        run([str(launcher), "build", sys.executable, str(observer)], env, tree, status=2)
        assert not observed.exists(), name
    write(site, policy)
    for changes in ({"ETUDE_SITE": str(root / "missing-site")}, {"RACH2_PORT_OFFSET": "0"},
                    {"SSH_TEST_PORT": "2222"}, {"ETUDE_ADMITTED": "/other/site"}):
        run([str(launcher), "build", sys.executable, str(observer)], dict(env, **changes), tree, status=2)
        assert not observed.exists(), changes
    write(ledger / "claims", "etude-ledger 2\n")
    run([str(launcher), "build", sys.executable, str(observer)], env, tree, status=2)
    assert not observed.exists()
    write(ledger / "claims", "etude-ledger 1\n")
    write(tree / "tests/system/suites.mk", "# fixed ports\n")
    run([str(launcher), "legacy", sys.executable, str(observer)], env, tree, status=2)
    assert not observed.exists()
    write(tree / "tests/system/suites.mk", "# RACH2_PORT_OFFSET\n")

    # A later launch waits behind the first claim and a cancellation must
    # stop its process before capacity is released.
    stage = "cancellation"
    ready = root / "ready"
    release = root / "release"
    waiting = root / "waiting.py"
    write(waiting, f'''import pathlib, time
exec(pathlib.Path({str(observer)!r}).read_text())
pathlib.Path({str(ready)!r}).touch()
while not pathlib.Path({str(release)!r}).exists():
    time.sleep(0.02)
''')
    children = []
    try:
        first = subprocess.Popen([str(launcher), "build", sys.executable, str(waiting)],
                                 env=env, cwd=tree, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        children.append(first)
        until(ready.exists)
        second = subprocess.Popen([str(launcher), "build", sys.executable, str(observer)],
                                  env=env, cwd=tree, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        children.append(second)
        until(lambda: "state blocked" in (ledger / "claims").read_text() or
              "state waiting" in (ledger / "claims").read_text())
        assert len(observed.read_text().splitlines()) == 1
        second.terminate()
        second.communicate(timeout=10)
        assert second.returncode == 143
        first.terminate()
        first.communicate(timeout=10)
        assert first.returncode == 143
        assert (ledger / "claims").read_text() == "etude-ledger 1\n"
        ready.unlink()
        clear()
    finally:
        for child in children:
            if child.poll() is None:
                child.terminate()
                try:
                    child.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.communicate()


def main():
    """Run each assertion with private configuration and remove all scratch."""
    def stopped(signum, frame):
        """Unwind child cleanup before removing the fixture's files."""
        raise InterruptedError(f"stopped by signal {signum}")

    for signum in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(signum, stopped)
    repo = Path(__file__).resolve().parents[2]
    sources = Path(os.environ.get("ETUDE_LAUNCH_SOURCES", repo))
    root = None
    try:
        root = Path(tempfile.mkdtemp(prefix="rach2-etude-launch."))
        rows(root, repo, sources)
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f"etude-launch-test: failed: {stage}: {error}", file=sys.stderr)
        return 1
    finally:
        if root is not None:
            claims = root / "ledger/claims"
            if claims.exists() and "claim " in claims.read_text():
                print(f"etude-launch-test: evidence: claims retained at {root}", file=sys.stderr)
            else:
                shutil.rmtree(root)
    print("etude-launch-test: result: pass: installed launch admission")
    return 0


if __name__ == "__main__":
    sys.exit(main())
