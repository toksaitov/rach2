#!/usr/bin/env python3
"""Check installed admission before real build, worker, owner and CI entries."""

import json
import fcntl
import os
from pathlib import Path
import shutil
import shlex
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
    """Run a fixture and require its status, retaining its diagnostics."""
    p = subprocess.Popen(argv, env=env, cwd=cwd, text=True,
                         stdin=subprocess.PIPE if input is not None else None,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        output, _ = p.communicate(input=input)
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


def until(predicate, children):
    """Wait for a fixture's published state while each of CHILDREN runs.

    A child that ends first fails the wait with its status and output.
    """
    while not predicate():
        for child in children:
            if child.poll() is not None:
                output, _ = child.communicate()
                raise AssertionError(("fixture ended before it published its state",
                                      child.args, child.returncode, output))
        time.sleep(0.02)


def rows(root, repo):
    """Observe durable claims and actual compatibility locks at each entry."""
    global stage
    env = dict(os.environ)
    real_make = env.get("ETUDE_MAKE_REAL") or shutil.which("make")
    for key in ("ETUDE_ADMITTED", "ETUDE_SITE", "RACH2_PORT_OFFSET", "SSH_TEST_PORT",
                "RACH2_RUN_WORK", "RACH2_RUN_STATUS", "MAKEFLAGS", "MFLAGS",
                "MAKELEVEL", "MAKEOVERRIDES", "ETUDE_BUILD_JOBS", "JOBS"):
        env.pop(key, None)
    etude = repo / "build/tools/etude/etude"
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
    shutil.copyfile(repo / "tools/etude/launch.sh", launcher)
    launcher.chmod(0o755)
    write(root / "etude-bin/make", (repo / "tools/etude/make.sh").read_text(), executable=True)
    policy = (f"etude-site 2\nendpoint {'macos' if sys.platform == 'darwin' else 'linux'}\n"
              f"launcher {launcher}\nledger {ledger}\nlocks {locks}\netude {etude}\n"
              "capacity 24000,103079215104,171798691840\nwait 3\nseconds 30\n")
    write(site, policy)
    env.update(ETUDE_SITE=str(site), ETUDE_LAUNCH_LEDGER=str(ledger),
               ETUDE_LAUNCH_OBSERVED=str(observed), ETUDE_LAUNCH_LOCKS=str(locks),
               ETUDE_STATE=str(root / "private-state"),
               PATH=str(bin) + os.pathsep + env["PATH"],
               RACH2_WORKERS="2", RACH2_MEMORY_MIB="96", RACH2_CPUS="2",
               EXPECT_MEMORY="5368709120")
    # The command must already have a durable, registered reservation and
    # the matching port and host locks when its first instruction executes.
    write(observer, '''#!/usr/bin/env python3
import fcntl, json, os
from pathlib import Path
ledger = Path(os.environ["ETUDE_LAUNCH_LEDGER"])
text = (ledger / "claims").read_text()
assert text.count("claim ") == 1, text
assert "process case " in text and "state running" in text, text
assert "resource cpu_millis " not in text and "resource disk_bytes " not in text, text
assert f"resource memory_bytes {os.environ['EXPECT_MEMORY']}\\n" in text, text
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
    prefix = (repo / "Makefile").read_text().split("# The architecture (mk/arch.mk):")[0]
    write(tree / "Makefile", prefix + f"all:\n\t@{observe}\n\t@test '$(ARG)' = 'two words'\nendif\n")
    (tree / "tools/etude").mkdir(parents=True)
    shutil.copyfile(launcher, tree / "tools/etude/launch.sh")
    stage = "build"
    run([real_make, "ARG=two words"], env, tree)
    saw()
    clear()

    for flags, jobs in ((["-j", "7"], 7), (["-j16"], 16)):
        env.update(EXPECT_MEMORY=str((jobs + 1) * 1073741824))
        run([str(launcher), "build", real_make, *flags, "ARG=two words"], env, tree)
        saw()
        clear()
    env.update(EXPECT_MEMORY="5368709120")

    # An older preparation's unbounded make is limited before it executes.
    stage = "job-limit"
    job_args = root / "job-args"
    fake_make = root / "record-make"
    write(fake_make, f"#!/bin/sh\nprintf '%s\\n' \"$@\" > '{job_args}'\n", executable=True)
    job_env = dict(env, ETUDE_MAKE_REAL=str(fake_make), ETUDE_BUILD_JOBS="3", MAKELEVEL="0")
    run([str(root / "etude-bin/make"), "-j", "components"], job_env, tree)
    assert job_args.read_text().splitlines() == ["-j", "components", "-j3"]
    for arguments, limit in (([], "1"), (["-j", "1"], "1"), (["--jobs=2"], "2"), (["-j9"], "3"), (["-j12"], "3")):
        run([str(root / "etude-bin/make"), *arguments, "components"], job_env, tree)
        assert job_args.read_text().splitlines() == arguments + ["components", "-j" + limit]
    job_env["MAKEFLAGS"] = " ".join(("-j2", "--", "JOBS=9"))
    run([str(root / "etude-bin/make"), "components"], job_env, tree)
    assert job_args.read_text().splitlines() == ["components", "-j2"]
    job_env.pop("MAKEFLAGS")
    scrubbed = dict(job_env)
    scrubbed.pop("ETUDE_BUILD_JOBS")
    scrubbed.pop("ETUDE_MAKE_REAL")
    write(root / "unwrapped-bin/make", fake_make.read_text(), executable=True)
    scrubbed["PATH"] = str(root / "etude-bin") + os.pathsep + str(root / "unwrapped-bin") + os.pathsep + env["PATH"]
    run([str(root / "etude-bin/make"), "-j", "components"], scrubbed, tree)
    assert job_args.read_text().splitlines() == ["-j", "components", "-j1"]
    job_env["MAKELEVEL"] = "1"
    run([str(root / "etude-bin/make"), "all"], job_env, tree)
    assert job_args.read_text().splitlines() == ["all"]

    # The real worker entry must claim before even its compilation of the
    # lock, which tools/etude/hold.sh gives it.
    stage = "worker"
    shutil.copyfile(repo / "tests/run", tree / "tests/run")
    shutil.copyfile(repo / "tools/etude/hold.sh", tree / "tools/etude/hold.sh")
    write(bin / "cc", f"#!/bin/sh\n{observe}\nexit 19\n", executable=True)
    run(["sh", "tests/run", "style"], env, tree, status=1)
    saw()
    clear()

    # CI admits its build and each test step separately.
    stage = "ci"
    write(bin / "make", f"#!/bin/sh\nexec '{launcher}' build {observe}\n", executable=True)
    write(tree / "tests/run", f'''#!/bin/sh
if [ -z "${{ETUDE_ADMITTED:-}}" ]; then
    exec '{launcher}' legacy sh "$0" "$@"
fi
{observe} || exit 3
for name do
    [ "$name" != core ] || name=default
    echo "run: $name: PASS in 0s"
done
''', executable=True)
    write(tree / "build/tests/failed/.keep", "")
    run(["sh", str(repo / "tools/ci/run.sh"), "core", str(root / "ci")], env, tree)
    saw(4, same=False)
    clear()

    # Owner commands admit their build and test steps independently,
    # preserving quoting, standard input and the command's own status.
    stage = "owner"
    env.update(EXPECT_MEMORY="5368709120")
    testing = root / "owner/tools/dev/testing.sh"
    testing.parent.mkdir(parents=True)
    shutil.copyfile(repo / "tools/dev/testing.sh", testing)
    write(bin / "ssh", '''#!/bin/sh
case $4 in *'bash -s '*) echo held; exec cat >/dev/null ;; esac
exec bash -c "$4"
''', executable=True)
    write(bin / "wsl.exe", '#!/bin/sh\nshift\nexec "$@"\n', executable=True)
    write(bin / "rsync", '#!/bin/sh\n[ "$1" != --version ] || echo "rsync  version 3.2.7"\n', executable=True)
    env["RACH2_TESTING_COPY"] = str(tree)
    command_text = observe + " || exit 3; printf '%s\\n' \"a'b\"; cat; exit 7"
    owner_command = f"RACH2_WORKERS=2 RACH2_MEMORY_MIB=96 '{launcher}' build bash -c {shlex.quote(command_text)}"
    output = run(["sh", str(testing), "run", owner_command],
                 env, tree, status=7, input="owner input\n")
    assert "a'b\nowner input\n" in output, output
    saw()
    clear()

    # An old checkout is admitted from outside its source, with no nested
    # port claim. The stand-in for an older checkout's claim, which
    # tests/host/etude_ports_hosttest.c checks against `etude lock ports`,
    # attempts the same lock an older tests/run takes.
    standin = repo / "tests/system/fixtures/old_ports.py"
    program = root / "old-run"
    stage = "legacy"
    write(program, f'''#!/bin/sh
if [ -n "${{RACH2_PORT_OFFSET:-}}" ]; then exec {observe}; fi
exec '{sys.executable}' '{standin}' '{locks}' 2 '{sys.executable}' '{observer}'
''',
          executable=True)
    result = root / "result"
    env.update(EXPECT_MEMORY="5368709120")
    with (locks / "rach2-ports.0").open("r+") as occupied:
        fcntl.lockf(occupied, fcntl.LOCK_EX)
        run([str(launcher), "-r", str(result), "legacy", str(program)], env, tree)
    assert json.loads(observed.read_text())["offset"] == 1000
    assert result.read_text() == "exited 0\n"
    saw()
    clear()

    # Bisection wraps an old checkout whose entries do not admit themselves,
    # and uses the installed site before a private machine record.
    stage = "bisection"
    env.update(EXPECT_MEMORY="5368709120")
    write(bin / "make", f"#!/bin/sh\nexec {observe}\n", executable=True)
    write(tree / "tests/run", f'''#!/bin/sh
{observe} || exit 3
for name do
    echo "run: $name: PASS in 0s"
done
''', executable=True)
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
    # The observer's workers reach the launcher from core's settings alone.
    write(tree / "tools/ci/core.env", "# The settings of a core run.\nRACH2_WORKERS=2\n")
    core_env = dict(env)
    core_env.pop("RACH2_WORKERS")
    run(["sh", str(repo / "tools/ci/bisect.sh"), "-e", str(etude),
         "core", "i686", "good", "bad", "1", "default"],
        core_env, tree)
    saw(2, same=False)
    clear()

    # A command nested in an admitted owner command reuses its claim and
    # still supplies the separate result that bisection reads.
    stage = "nested"
    env.update(EXPECT_MEMORY="5368709120")
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
        ("format", policy.replace("etude-site 2", "etude-site 1")),
        ("endpoint", policy.replace("endpoint linux", "endpoint windows").replace("endpoint macos", "endpoint windows")),
        ("etude", policy.replace(str(etude), str(root / "missing-etude"))),
        ("capacity", policy.replace("24000,103079215104,171798691840", "24000,0,171798691840")),
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

    # The permission of the lazy boundary profile names that profile alone;
    # a policy with it admits a command as one without it does.
    stage = "boundary"
    build_env = dict(env, EXPECT_MEMORY="5368709120")
    for value, status in (("bigmem-boundary-v1", 0), ("yes", 2), ("bigmem-boundary-v2", 2)):
        write(site, policy + f"boundary_lazy {value}\n")
        run([str(launcher), "build", sys.executable, str(observer)], build_env, tree, status=status)
        if status == 0:
            saw()
            clear()
        else:
            assert not observed.exists(), value
    write(site, policy)

    # Two ordinary host selections overlap; a large or serial one waits.
    stage = "concurrency"
    write(bin / "id", '#!/bin/sh\n[ "$1" != -un ] || { echo "$TEST_ACCOUNT"; exit; }\nexec /usr/bin/id "$@"\n', executable=True)
    write(tree / "tests/run", '''#!/usr/bin/env python3
import os, pathlib, time
assert os.environ["RACH2_WORKERS"] == os.environ.get("TEST_WORKERS", "2")
pathlib.Path(os.environ["TEST_READY"]).write_text(os.environ["RACH2_PORT_OFFSET"])
while not pathlib.Path(os.environ["TEST_RELEASE"]).exists():
    time.sleep(0.02)
''', executable=True)
    for exclusive in ("serial", "large"):
        registry = "host|-|full||384\n"
        if exclusive == "serial":
            registry += "exclusive|-|full||128||serial\n"
        else:
            memory = os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")
            registry += f"exclusive|-|full||{memory // 1048576 // 2 + 1}\n"
        write(tree / "tests/registry", registry)
        children = []
        release = root / f"release-{exclusive}"
        exclusive_release = root / f"release-only-{exclusive}"
        try:
            ready = [root / f"ready-{exclusive}-{i}" for i in range(3)]
            for i in range(2):
                child_env = dict(env, TEST_ACCOUNT=f"account-{i}", TEST_READY=str(ready[i]),
                                 TEST_RELEASE=str(release))
                children.append(subprocess.Popen([str(launcher), "legacy", "tests/run", "host"],
                                                 env=child_env, cwd=tree, stdout=subprocess.PIPE,
                                                 stderr=subprocess.STDOUT))
            until(lambda: all(p.exists() for p in ready[:2]), children)
            text = (ledger / "claims").read_text()
            assert text.count("state running") == 2, text
            assert "account account-0\n" in text and "account account-1\n" in text, text
            assert text.count("resource memory_bytes 5368709120\n") == 2, text
            assert ready[0].read_text() != ready[1].read_text()
            child_env = dict(env, TEST_ACCOUNT="account-exclusive", TEST_READY=str(ready[2]),
                             TEST_RELEASE=str(exclusive_release),
                             TEST_WORKERS="1" if exclusive == "serial" else "2")
            children.append(subprocess.Popen([str(launcher), "legacy", "tests/run", "exclusive"],
                                             env=child_env, cwd=tree, stdout=subprocess.PIPE,
                                             stderr=subprocess.STDOUT))
            until(lambda: "state blocked" in (ledger / "claims").read_text() or
                  "state waiting" in (ledger / "claims").read_text(), children)
            assert not ready[2].exists()
            release.touch()
            for child in children[:2]:
                output, _ = child.communicate()
                assert child.returncode == 0, output
            until(ready[2].exists, children[2:])
            text = (ledger / "claims").read_text()
            assert text.count("state running") == 1, text
            assert "resource memory_bytes 103079215104\n" in text, text
            exclusive_release.touch()
            output, _ = children[2].communicate()
            assert children[2].returncode == 0, output
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
    env["TEST_ACCOUNT"] = "account-cancel"

    # A later launch waits behind the first claim and a cancellation must
    # stop its process before capacity is released.
    stage = "cancellation"
    env.update(EXPECT_MEMORY="103079215104")
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
        first = subprocess.Popen([str(launcher), "alone", sys.executable, str(waiting)],
                                 env=env, cwd=tree, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        children.append(first)
        until(ready.exists, children)
        second = subprocess.Popen([str(launcher), "alone", sys.executable, str(observer)],
                                  env=env, cwd=tree, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        children.append(second)
        until(lambda: "state blocked" in (ledger / "claims").read_text() or
              "state waiting" in (ledger / "claims").read_text(), children)
        assert len(observed.read_text().splitlines()) == 1
        second.terminate()
        second.communicate()
        assert second.returncode == 143
        first.terminate()
        first.communicate()
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
    root = None
    try:
        root = Path(tempfile.mkdtemp(prefix="rach2-etude-launch."))
        rows(root, repo)
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
