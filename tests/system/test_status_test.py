#!/usr/bin/env python3
"""Check live lock claims and stale run records without booting a machine."""

import contextlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


@contextlib.contextmanager
def command(args, env=None):
    """Start a fixture and reap it, including on a failed assertion."""
    child = subprocess.Popen(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        yield child
    finally:
        child.terminate()
        try:
            child.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.communicate()


def until(predicate):
    """Wait at most five seconds for a fixture to publish its state."""
    end = time.monotonic() + 5
    while time.monotonic() < end:
        result = predicate()
        if result:
            return result
        time.sleep(0.02)
    raise AssertionError("fixture did not publish its state")


def load(path):
    """Load a private status reader, including a deliberately faulty copy."""
    spec = importlib.util.spec_from_file_location("test_status", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def rows(reader, helper, root):
    """Exercise claim records, actual lock holders and stale identities."""
    build = root / "build"
    build.mkdir(exist_ok=True)
    work = root / "work"
    work.mkdir(exist_ok=True)
    ports = root / "ports"
    ports.mkdir(exist_ok=True)
    env = dict(os.environ, RACH2_RUN_WORK=str(work))
    env.pop("RACH2_RUN_TREE_LOCK", None)
    env.pop("RACH2_RUN_STATUS", None)
    env.pop("RACH2_HOST_LOCK", None)
    status = build / ".run.status.fixture"
    identity = subprocess.check_output([helper, "identity", str(os.getpid())], text=True).strip()
    status.write_text(f"identity {identity}\nwork {work}\nargs fixture\n")

    def report():
        import io
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            reader.report(root)
        return output.getvalue()

    lock_script = (
        "import fcntl,sys,time; "
        "f=open(sys.argv[1], 'a+'); "
        "fcntl.lockf(f, fcntl.LOCK_SH if sys.argv[2]=='read' else fcntl.LOCK_EX); "
        "print('ready', flush=True); time.sleep(30)"
    )
    claim = work / "claim.wait"
    path = ports / "rach2-ports.0"
    with command([sys.executable, "-c", lock_script, str(path), "write"]) as holder:
        assert holder.stdout.readline() == b"ready\n"
        with command([helper, "ports", str(ports), "1", "true"], env) as waiter:
            until(claim.exists)
            before = claim.read_text()
            output = report()
            assert "waits for ports since " in output and " UTC" in output, output
            assert f"holder {holder.pid} alive" in output, output
            time.sleep(1.1)
            assert claim.read_text() == before, "wait start changed on a retry"
            holder.terminate()
            holder.wait(timeout=5)
            assert waiter.wait(timeout=5) == 0
            assert not claim.exists(), "claim kept its wait after admission"
    for mode, filename in (("shared", "rach2-host.gate"), ("alone", "rach2-host.lock")):
        path = ports / filename
        kind = "write" if mode == "shared" else "read"
        with command([sys.executable, "-c", lock_script, str(path), kind]) as holder:
            assert holder.stdout.readline() == b"ready\n"
            host_env = dict(env, RACH2_HOST_LOCK=mode)
            with command([helper, "ports", str(ports), "1", "true"], host_env) as waiter:
                until(claim.exists)
                output = report()
                assert "waits for host since " in output and str(path) in output, output
                assert f"holder {holder.pid} alive" in output, output
                holder.terminate()
                holder.wait(timeout=5)
                assert waiter.wait(timeout=5) == 0
                assert not claim.exists()
    # Shared host locks have more than one holder. The reader must list both.
    path = ports / "rach2-host.lock"
    with command([sys.executable, "-c", lock_script, str(path), "read"]) as a:
        assert a.stdout.readline() == b"ready\n"
        with command([sys.executable, "-c", lock_script, str(path), "read"]) as b:
            assert b.stdout.readline() == b"ready\n"
            with command([helper, "ports", str(ports), "1", "true"],
                         dict(env, RACH2_HOST_LOCK="alone")):
                until(claim.exists)
                output = report()
                assert f"holder {a.pid} alive" in output, output
                assert f"holder {b.pid} alive" in output, output
    claim.unlink(missing_ok=True)
    work.mkdir(exist_ok=True)
    with command(["sleep", "30"]) as holder:
        ident = subprocess.check_output([helper, "identity", str(holder.pid)], text=True).strip()
        (work / "pool.wait").write_text(
            f"reason memory\nsince 1\nbudget 4000\npending hungry2 3000\n"
            f"holder hungry1 3000 {ident}\n")
        assert f"holder hungry1 pid {holder.pid} alive" in report()
        holder.terminate()
        holder.wait(timeout=5)
        output = report()
        assert f"holder hungry1 pid {holder.pid} dead" in output and "fault:" in output, output
    words = identity.split()
    words[2] = str(int(words[2]) + 1)
    status.write_text(f"identity {' '.join(words)}\nwork {work}\nargs reused\n")
    assert "dead (pid reused)" in report(), report()
    owner = build / ".run.lock" / "owner"
    owner.parent.mkdir(exist_ok=True)
    owner.write_text(f"pid {holder.pid}\npgid 1\nsince old\nargs fixture\n")
    assert "fault: inspect the work and clear the stale tree lock" in report()
    with command(["true"]) as zombie:
        until(lambda: reader.process(zombie.pid) == "dead")
        assert reader.process(zombie.pid) == "dead"
    owner.unlink()
    owner.parent.rmdir()
    (work / "pool.wait").unlink()
    status.unlink()
    assert "no recorded test run" in report()
    print("test-status-test: result: pass")


def main():
    """Run the rows and optionally require a faulty reader to fail them."""
    source = Path(__file__).resolve().parents[2] / "tools/flow/test_status.py"
    text = source.read_text()
    needle = 'words[19] != str(start)'
    assert text.count(needle) == 1, "start-time fault text changed"
    fault = text.replace(needle, 'False')
    if sys.argv[1:] == ["--texts"]:
        print("test-status-test: result: fault text found")
        return
    if not Path("/proc/locks").exists():
        print("test-status-test: result: skipped: Linux testing host required")
        return
    helper = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="test-status.") as temp:
        root = Path(temp)
        copy = root / "reader.py"
        shutil.copyfile(source, copy)
        rows(load(copy), helper, root)
        if "--mutate" in sys.argv[2:]:
            copy.write_text(fault)
            try:
                rows(load(copy), helper, root)
            except AssertionError:
                print("test-status-test: result: mutation caught")
            else:
                raise AssertionError("pid reuse mutation passed")
            copy.write_text(text)
            rows(load(copy), helper, root)


if __name__ == "__main__":
    main()
