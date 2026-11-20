"""Trace the immediate and settled Chromium pictures on the Lenovo's panel."""
import os
from pathlib import Path
import pwd
import shutil
import signal
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(Path("tests/graphics/lenovo").resolve()))
from hasvk import end_desktop, machine, private_weston, run, stop, user_command

TRACE = Path("/sys/kernel/debug/bkl/trace")


def desktop_dir(path, account):
    """Make a new directory writable by the display's user."""
    path.mkdir(mode=0o700)
    os.chown(path, account.pw_uid, account.pw_gid)


def weston_pid():
    """Read the private Weston's PID after the old compositor has exited."""
    pids = []
    for stat in Path("/proc").glob("[0-9]*/stat"):
        try:
            name, fields = stat.read_text().rsplit(")", 1)
            if name.endswith("(weston") and fields.split()[0] != "Z":
                pids.append(int(stat.parent.name))
        except (FileNotFoundError, ProcessLookupError):
            continue
    pid, = pids
    return pid


def client_command(user, args):
    """Run a graphics client with an empty Mesa cache as the display user."""
    return user_command(user, ["sh", "-c", '. tests/graphics/guest-env.sh && exec "$@"',
                               "graphics", *args])


def collect(case, work, settle, weston, account, env):
    """Record the kernel from just before readback until the picture answers."""
    case.mkdir()
    out = case / "out"
    desktop_dir(out, account)
    desktop_dir(work, account)
    (out / "weston.pid").write_text("%d\n" % weston)
    command = client_command(account.pw_name, [sys.executable, "-u", HERE / "picture.py",
                                              out, work, settle])
    env = dict(env, TMPDIR=str(work), MESA_SHADER_CACHE_DIR=str(work / "mesa"))
    started = stopped = False
    with (case / "measure.log").open("w", buffering=1) as log:
        browser = subprocess.Popen(command, env=env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=log, text=True,
                                   start_new_session=True)
        try:
            for line in browser.stdout:
                print(line, end="", flush=True)
                log.write(line)
                if line == "webgpu: trace start\n" and not started:
                    TRACE.write_text("start\n")
                    started = True
                    browser.stdin.write("start\n")
                    browser.stdin.flush()
                elif line == "webgpu: trace stop\n" and started and not stopped:
                    TRACE.write_text("stop\n")
                    stopped = True
                    browser.stdin.write("stop\n")
                    browser.stdin.flush()
            if browser.wait() != 0 or not stopped:
                raise RuntimeError("browser measurement failed; read " + str(case / "measure.log"))
        finally:
            stop(browser)
            browser.stdin.close()
            browser.stdout.close()
            if started:
                TRACE.write_text("stop\n")
                try:
                    shutil.copyfile(TRACE, case / "kernel.bin")
                finally:
                    TRACE.write_text("clear\n")


def main():
    """Use one private DRM Weston and two fresh browser profiles."""
    user, output = sys.argv[1:]
    account = pwd.getpwnam(user)
    if os.getuid() != 0 or account.pw_uid == 0 or not TRACE.is_file():
        raise RuntimeError("need root, a display user and the kernel trace file")
    _, icd = machine()
    out = Path(output).resolve()
    out.mkdir()
    display = out / "display"
    desktop_dir(display, account)
    for sig in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: sys.exit(1))
    with tempfile.TemporaryDirectory(prefix="webgpu-", dir=os.environ["TMPDIR"]) as scratch:
        work = Path(scratch)
        os.chown(work, account.pw_uid, account.pw_gid)
        runtime = work / "run"
        desktop_dir(runtime, account)
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), WAYLAND_DISPLAY="wayland-hasvk",
                   TMPDIR=str(work), GRAPHICS_CLASS="performance", GRAPHICS_DONE="1",
                   VK_DRIVER_FILES=str(icd), MESA_SHADER_CACHE_DIR=str(work / "cube-cache"),
                   PYTHONDONTWRITEBYTECODE="1")
        end_desktop()
        weston = private_weston(user, display, env)
        try:
            pid = weston_pid()
            run(client_command(user, ["vkcube", "--wsi", "wayland", "--c", "300"]),
                display / "cube.log", env)
            for name, delay in (("immediate", 0), ("settled", 5)):
                print("webgpu: measure " + name, flush=True)
                collect(out / name, work / name, delay, pid, account, env)
        finally:
            stop(weston)


if __name__ == "__main__":
    main()
