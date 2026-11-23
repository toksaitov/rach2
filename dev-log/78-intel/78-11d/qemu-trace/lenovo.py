"""Trace Chromium's WebGPU pictures on the Lenovo's panel, under Rach2 or Debian."""
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


def collect(case, work, settle, weston, account, env, joined=False, kernel=True):
    """Record the picture, with the kernel from just before readback until it answers.

    JOINED adds Weston's timeline and drm-backend streams, and on Rach2 the
    DRM frame trace. Without KERNEL, on Debian or for a probe-free picture,
    no kernel trace is written; the browser's trace handshake still runs.
    """
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
    traces = [(TRACE, "kernel.bin")] if kernel else []
    if joined and kernel:
        traces.append((DRM_TRACE, "drm.bin"))
    started = []
    begun = stopped = False
    with streams, (case / "measure.log").open("w", buffering=1) as log:
        browser = subprocess.Popen(command, env=env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=log, text=True,
                                   start_new_session=True)
        try:
            for line in browser.stdout:
                print(line, end="", flush=True)
                log.write(line)
                if line == "webgpu: trace start\n" and not begun:
                    for path, name in traces:
                        path.write_text("start\n")
                        started.append((path, name))
                    begun = True
                    browser.stdin.write("start\n")
                    browser.stdin.flush()
                elif line == "webgpu: trace stop\n" and begun and not stopped:
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


def missed(case, name):
    """Print a picture's largest early gap and late frames; return whether it missed a bound."""
    result = json.loads((case / "out/result.json").read_text())
    picture = frames(result["picture"])
    late = picture["late_after_steady"]
    early = max(picture["first_gaps_ms"])
    print("webgpu: %s: %.2f ms early gap, %d late frames" % (name, early, late), flush=True)
    return early > 33.4 or late > 0


def bare_pictures(out, work, weston, account, env, count):
    """Draw COUNT fresh immediate pictures with no trace of any kind."""
    env = dict(env, RACH2_BROWSER_TRACE="0")
    for index in range(1, count + 1):
        name = "bare-%d" % index
        print("webgpu: measure " + name + " without probes", flush=True)
        collect(out / name, work / name, 0, weston, account, env, kernel=False)
        missed(out / name, name)


def profiled_pictures(out, work, weston, account, env, kernel, every):
    """Trace fresh immediate pictures until a frame misses its bound, or all four with EVERY."""
    env = dict(env, RACH2_BROWSER_TRACE="1")
    what = "Chromium, Weston and DRM" if kernel else "Chromium and Weston"
    for index in range(1, 5):
        name = "immediate-%d" % index
        print("webgpu: measure %s with %s tracing" % (name, what), flush=True)
        collect(out / name, work / name, 0, weston, account, env, joined=True, kernel=kernel)
        if missed(out / name, name) and not every:
            break


def main():
    """Use one private DRM Weston and fresh browser profiles."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("user")
    parser.add_argument("output")
    parser.add_argument("--profiled", action="store_true", help="trace Chromium, Weston and DRM")
    parser.add_argument("--bare", type=int, default=0, metavar="N",
                        help="first draw N pictures without probes")
    parser.add_argument("--all", action="store_true",
                        help="trace all four pictures, not only up to the first miss")
    parser.add_argument("--linux", action="store_true",
                        help="Debian's reference: no Rach2 kernel traces")
    args = parser.parse_args()
    user = args.user
    account = pwd.getpwnam(user)
    if os.getuid() != 0 or account.pw_uid == 0:
        raise RuntimeError("need root and a display user")
    if args.linux and TRACE.is_file():
        raise RuntimeError("--linux is for Debian, and this kernel has Rach2's trace file")
    if not args.linux and not TRACE.is_file():
        raise RuntimeError("need the kernel trace file, or --linux on Debian")
    if args.profiled and not args.linux and not DRM_TRACE.is_file():
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
            bare_pictures(out, work, pid, account, env, args.bare)
            if args.profiled:
                profiled_pictures(out, work, pid, account, env, not args.linux, args.all)
            elif not args.bare:
                for name, delay in (("immediate", 0), ("settled", 5)):
                    print("webgpu: measure " + name, flush=True)
                    collect(out / name, work / name, delay, pid, account, env)
        finally:
            stop(weston)


if __name__ == "__main__":
    main()
