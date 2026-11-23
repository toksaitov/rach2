"""Trace the immediate and settled Chromium pictures on the Lenovo's panel."""
import argparse
import json
import os
from pathlib import Path
import pwd
import shutil
import signal
import subprocess
import sys
import tempfile
from contextlib import nullcontext

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(Path("tests/graphics/lenovo").resolve()))
from hasvk import end_desktop, machine, private_weston, run, stop, user_command
from picture import weston_streams
from stall import frames

TRACE = Path("/sys/kernel/debug/bkl/trace")
DRM_TRACE = Path("/sys/kernel/debug/drm_frame_trace")


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


def collect(case, work, settle, weston, account, env, joined=False):
    """Record the kernel from just before readback until the picture answers."""
    case.mkdir()
    out = case / "out"
    desktop_dir(out, account)
    desktop_dir(work, account)
    (out / "weston.pid").write_text("%d\n" % weston)
    command = client_command(account.pw_name, [sys.executable, "-u", HERE / "picture.py",
                                              out, work, settle])
    env = dict(env, TMPDIR=str(work), MESA_SHADER_CACHE_DIR=str(work / "mesa"))
    streams = (weston_streams(case, env, lambda args: user_command(account.pw_name, args))
               if joined else nullcontext())
    traces = [(TRACE, "kernel.bin")]
    if joined:
        traces.append((DRM_TRACE, "drm.bin"))
    started = []
    stopped = False
    with streams, (case / "measure.log").open("w", buffering=1) as log:
        browser = subprocess.Popen(command, env=env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=log, text=True,
                                   start_new_session=True)
        try:
            for line in browser.stdout:
                print(line, end="", flush=True)
                log.write(line)
                if line == "webgpu: trace start\n" and not started:
                    for path, name in traces:
                        path.write_text("start\n")
                        started.append((path, name))
                    browser.stdin.write("start\n")
                    browser.stdin.flush()
                elif line == "webgpu: trace stop\n" and started and not stopped:
                    for path, _ in reversed(started):
                        path.write_text("stop\n")
                    stopped = True
                    browser.stdin.write("stop\n")
                    browser.stdin.flush()
            if browser.wait() != 0 or not stopped:
                raise RuntimeError("browser measurement failed; read " + str(case / "measure.log"))
        finally:
            stop(browser)
            browser.stdin.close()
            browser.stdout.close()
            for path, _ in started:
                path.write_text("stop\n")
            for path, name in started:
                try:
                    with path.open("rb") as source, (case / name).open("wb") as target:
                        shutil.copyfileobj(source, target)
                finally:
                    if path == TRACE:
                        path.write_text("clear\n")


def profiled_pictures(out, work, weston, account, env):
    """Trace fresh immediate pictures until a frame misses its bound."""
    env = dict(env, RACH2_BROWSER_TRACE="1")
    for index in range(1, 5):
        name = "immediate-%d" % index
        print("webgpu: measure " + name + " with Chromium, Weston and DRM tracing", flush=True)
        collect(out / name, work / name, 0, weston, account, env, joined=True)
        result = json.loads((out / name / "out/result.json").read_text())
        picture = frames(result["picture"])
        late = picture["late_after_steady"]
        early = max(picture["first_gaps_ms"])
        print("webgpu: %s: %.2f ms early gap, %d late frames" % (name, early, late), flush=True)
        if early > 33.4 or late:
            break


def main():
    """Use one private DRM Weston and fresh browser profiles."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("user")
    parser.add_argument("output")
    parser.add_argument("--profiled", action="store_true", help="trace Chromium, Weston and DRM")
    args = parser.parse_args()
    user = args.user
    account = pwd.getpwnam(user)
    if os.getuid() != 0 or account.pw_uid == 0 or not TRACE.is_file():
        raise RuntimeError("need root, a display user and the kernel trace file")
    if args.profiled and not DRM_TRACE.is_file():
        raise RuntimeError("need the DRM frame trace build")
    _, icd = machine()
    out = Path(args.output).resolve()
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
            if args.profiled:
                profiled_pictures(out, work, pid, account, env)
            else:
                for name, delay in (("immediate", 0), ("settled", 5)):
                    print("webgpu: measure " + name, flush=True)
                    collect(out / name, work / name, delay, pid, account, env)
        finally:
            stop(weston)


if __name__ == "__main__":
    main()
