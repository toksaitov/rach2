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
import time
from contextlib import ExitStack, nullcontext

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


def browser_gpu(profile):
    """Find the GPU process belonging to this picture's private profile."""
    option = b"--user-data-dir=" + os.fsencode(profile)
    for cmdline in Path("/proc").glob("[0-9]*/cmdline"):
        try:
            args = cmdline.read_bytes()
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
        if b"--type=gpu-process" in args and option in args:
            return cmdline.parent
    raise RuntimeError("Chromium's GPU process is missing")


def file_tracer(process, output, log):
    """Attach to the GPU's main thread before the page starts, or reap the failed tracer."""
    tracer = subprocess.Popen(["strace", "-ttt", "-T", "-y", "-s", "512",
                               "-e", "trace=%file,close,read,pread64,lseek,fstat",
                               "-o", output, "-p", process.name],
                              stdout=log, stderr=log, start_new_session=True)
    expected = "TracerPid:\t%d" % tracer.pid
    deadline = time.monotonic() + 5
    attached = False
    try:
        while tracer.poll() is None and time.monotonic() < deadline:
            if expected in (process / "status").read_text().splitlines():
                attached = True
                return tracer
            time.sleep(0.05)
        raise RuntimeError("strace did not attach to Chromium's GPU process")
    finally:
        if not attached:
            stop(tracer)


def collect(case, work, settle, weston, account, env, joined=False, kernel=True,
            cache_root=None, files=False):
    """Record the picture, with the kernel from just before readback until it answers.

    JOINED adds Weston's timeline and drm-backend streams, and on Rach2 the
    DRM frame trace. Without KERNEL, on Debian or for a probe-free picture,
    no kernel trace is written; the browser's trace handshake still runs.
    CACHE_ROOT puts this picture's fresh Mesa cache outside the browser's
    work directory without moving its profile or Weston's cache.
    FILES records the GPU thread's file calls and changes their timing.
    """
    case.mkdir()
    out = case / "out"
    desktop_dir(out, account)
    desktop_dir(work, account)
    (out / "weston.pid").write_text("%d\n" % weston)
    command = client_command(account.pw_name, [sys.executable, "-u", HERE / "picture.py",
                                              out, work, settle])
    cache = work / "mesa" if cache_root is None else cache_root / case.name
    env = dict(env, TMPDIR=str(work), MESA_SHADER_CACHE_DIR=str(cache))
    streams = (weston_streams(case, env, lambda args: user_command(account.pw_name, args))
               if joined else nullcontext())
    traces = [(TRACE, "kernel.bin")] if kernel else []
    if joined and kernel:
        traces.append((DRM_TRACE, "drm.bin"))
    started = []
    begun = stopped = False
    tracer = None
    calls = case / "gpu-files.strace"
    with streams, (case / "measure.log").open("w", buffering=1) as log:
        browser = subprocess.Popen(command, env=env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=log, text=True,
                                   start_new_session=True)
        try:
            for line in browser.stdout:
                print(line, end="", flush=True)
                log.write(line)
                if line == "webgpu: trace start\n" and not begun:
                    if files:
                        tracer = file_tracer(browser_gpu(work / "profile"), calls, log)
                    for path, name in traces:
                        path.write_text("start\n")
                        started.append((path, name))
                    begun = True
                    browser.stdin.write("start\n")
                    browser.stdin.flush()
                elif line == "webgpu: trace stop\n" and begun and not stopped:
                    for path, _ in reversed(started):
                        path.write_text("stop\n")
                    stop(tracer)
                    tracer = None
                    stopped = True
                    browser.stdin.write("stop\n")
                    browser.stdin.flush()
            if browser.wait() != 0 or not stopped:
                raise RuntimeError("browser measurement failed; read " + str(case / "measure.log"))
            if files and (not calls.is_file() or calls.stat().st_size == 0):
                raise RuntimeError("strace did not record GPU file calls")
        finally:
            stop(tracer)
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


def bare_pictures(out, work, weston, account, env, count, cache_root):
    """Draw COUNT fresh immediate pictures with no trace of any kind."""
    env = dict(env, RACH2_BROWSER_TRACE="0")
    for index in range(1, count + 1):
        name = "bare-%d" % index
        print("webgpu: measure " + name + " without probes", flush=True)
        collect(out / name, work / name, 0, weston, account, env, kernel=False,
                cache_root=cache_root)
        missed(out / name, name)


def profiled_pictures(out, work, weston, account, env, kernel, every, cache_root):
    """Trace fresh immediate pictures until a frame misses its bound, or all four with EVERY."""
    env = dict(env, RACH2_BROWSER_TRACE="1")
    what = "Chromium, Weston and DRM" if kernel else "Chromium and Weston"
    for index in range(1, 5):
        name = "immediate-%d" % index
        print("webgpu: measure %s with %s tracing" % (name, what), flush=True)
        collect(out / name, work / name, 0, weston, account, env, joined=True, kernel=kernel,
                cache_root=cache_root)
        if missed(out / name, name) and not every:
            break


def main():
    """Use one private DRM Weston and fresh browser profiles."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("user")
    parser.add_argument("output")
    trace = parser.add_mutually_exclusive_group()
    trace.add_argument("--profiled", action="store_true", help="trace Chromium, Weston and DRM")
    trace.add_argument("--file-trace", action="store_true",
                       help="draw one picture with Chromium and GPU file calls traced")
    parser.add_argument("--bare", type=int, default=0, metavar="N",
                        help="first draw N pictures without probes")
    parser.add_argument("--all", action="store_true",
                        help="trace all four pictures, not only up to the first miss")
    parser.add_argument("--linux", action="store_true",
                        help="Debian's reference: no Rach2 kernel traces")
    parser.add_argument("--mesa-cache-parent", type=Path, metavar="DIR",
                        help="put the pictures' fresh Mesa caches under DIR")
    parser.add_argument("--mesa-cache-parts", type=int, metavar="N",
                        help="set the number of parts in the browser's Mesa cache database")
    args = parser.parse_args()
    if args.mesa_cache_parts is not None and args.mesa_cache_parts < 1:
        parser.error("--mesa-cache-parts must be positive")
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
    with ExitStack() as stack:
        work = Path(stack.enter_context(tempfile.TemporaryDirectory(
            prefix="webgpu-", dir=os.environ["TMPDIR"])))
        os.chown(work, account.pw_uid, account.pw_gid)
        cache_root = None
        if args.mesa_cache_parent is not None:
            cache_root = Path(stack.enter_context(tempfile.TemporaryDirectory(
                prefix="webgpu-mesa-", dir=args.mesa_cache_parent))).resolve()
            os.chown(cache_root, account.pw_uid, account.pw_gid)
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
            if args.mesa_cache_parts is not None:
                env["MESA_DISK_CACHE_DATABASE_NUM_PARTS"] = str(args.mesa_cache_parts)
            bare_pictures(out, work, pid, account, env, args.bare, cache_root)
            if args.profiled:
                profiled_pictures(out, work, pid, account, env, not args.linux, args.all, cache_root)
            elif args.file_trace:
                print("webgpu: measure GPU file calls", flush=True)
                collect(out / "files", work / "files", 0, pid, account,
                        dict(env, RACH2_BROWSER_TRACE="1"), kernel=not args.linux,
                        cache_root=cache_root, files=True)
            elif not args.bare:
                for name, delay in (("immediate", 0), ("settled", 5)):
                    print("webgpu: measure " + name, flush=True)
                    collect(out / name, work / name, delay, pid, account, env,
                            cache_root=cache_root)
        finally:
            stop(weston)


if __name__ == "__main__":
    main()
