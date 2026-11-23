"""Measure Chromium's WebGPU picture under headless Weston or a supplied display."""
from contextlib import contextmanager, ExitStack
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path("tests/graphics/browser").resolve()))
from run import Browser, hasvk_picture, save, server, stop


@contextmanager
def weston_streams(out, env, client):
    """Subscribe to the two Weston streams and close them on every exit."""
    scopes = ("timeline", "drm-backend")
    listed = subprocess.run(client(["weston-debug", "--list"]), env=env, check=True,
                            capture_output=True, text=True, timeout=10)
    available = listed.stdout + listed.stderr
    if any(scope not in available for scope in scopes):
        raise RuntimeError("Weston lacks a required log scope: " + available)
    with ExitStack() as stack:
        for scope in scopes:
            output = stack.enter_context((out / scope).open("w"))
            error = stack.enter_context((out / (scope + ".err")).open("w"))
            process = subprocess.Popen(client(["weston-debug", scope]), env=env,
                                       stdout=output, stderr=error, start_new_session=True)
            stack.callback(stop, process)
        yield
        for scope in scopes:
            if not (out / scope).stat().st_size:
                raise RuntimeError("Weston's " + scope + " stream was empty")


def tids(path):
    """Save the guest TIDs and Chromium's PID-namespace aliases after the picture."""
    rows = []
    for status in Path("/proc").glob("[0-9]*/task/[0-9]*/status"):
        try:
            fields = dict(line.split(":", 1) for line in status.read_text().splitlines()
                          if ":" in line)
            rows.append({"tid": int(fields["Pid"]), "tgid": int(fields["Tgid"]),
                         "nspid": int(fields["NSpid"].split()[-1]),
                         "name": fields["Name"].strip()})
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    save(path, rows)


def picture_done(out, capture):
    """Take a clock pair and read thread identities after the last frame."""
    before = time.monotonic_ns()
    epoch = time.time_ns()
    after = time.monotonic_ns()
    save(out / "clock.json", {"before": before, "epoch": epoch, "after": after})
    if capture:
        capture("stop")
    tids(out / "tids.json")


def trace_command(command):
    """Ask the privileged parent to change the kernel trace before proceeding."""
    print("webgpu: trace " + command, flush=True)
    if sys.stdin.readline().strip() != command:
        raise RuntimeError("kernel trace did not acknowledge " + command)


def measure(out, work, swiftshader, settle=0, capture=None):
    """Use a fresh profile and retain all callbacks, their clock and thread IDs."""
    mode = "webgpu" if swiftshader else "webgpu-hasvk"
    trace = out / "trace.json.gz" if os.environ.get("RACH2_BROWSER_TRACE", "1") == "1" else None
    with server(work, out) as url, Browser("chromium", mode, out, work) as browser:
        print("webgpu: chromium ready")
        if capture:
            capture("start")
        result = hasvk_picture(browser, url, trace, swiftshader=swiftshader,
                               picture_done=lambda: picture_done(out, capture), trace_startup=trace is not None,
                               settle=settle)
        save(out / "result.json", result)
        picture = result["picture"]
        if picture["frames"] != 480:
            raise RuntimeError("expected 480 frames, got %d" % picture["frames"])
        print("webgpu: result: 480 frames, %.3f s, %s" %
              (picture["seconds"], picture["adapter"]["architecture"]))


def main():
    """Run one fresh browser profile and retain its timings and the Weston log."""
    out = Path("out").resolve()
    with tempfile.TemporaryDirectory(dir=out) as temporary:
        work = Path(temporary)
        runtime = work / "run"
        runtime.mkdir(mode=0o700)
        os.environ.update(TMPDIR=str(work), XDG_RUNTIME_DIR=str(runtime),
                          WAYLAND_DISPLAY="wayland-measure", LP_NUM_THREADS="2",
                          MESA_SHADER_CACHE_DIR=str(work / "mesa"))
        drm = os.environ.get("RACH2_FRAME_TIMELINE") == "1"
        backend = ["--backend=headless", "--width=640", "--height=480"]
        if drm:
            backend = ["--backend=drm", "--continue-without-input", "--debug"]
            os.environ["LIBSEAT_BACKEND"] = "noop"
        weston = subprocess.Popen(
            ["weston", "--no-config", *backend, "--renderer=pixman", "--shell=kiosk",
             "--idle-time=0", "--socket=wayland-measure", "--log=" + str(out / "weston.log")],
            start_new_session=True)
        try:
            for _ in range(30):
                if (runtime / "wayland-measure").is_socket():
                    break
                if weston.poll() is not None:
                    raise RuntimeError("Weston exited before opening its socket")
                time.sleep(1)
            else:
                raise RuntimeError("Weston did not open its socket")
            print("qemu: Weston ready")
            (out / "weston.pid").write_text("%d\n" % weston.pid)
            if drm:
                with weston_streams(out, os.environ, lambda args: args):
                    measure(out, work, swiftshader=True)
            else:
                measure(out, work, swiftshader=True)
        finally:
            stop(weston)


if __name__ == "__main__":
    if len(sys.argv) == 1:
        main()
    else:
        _, output, workdir, delay = sys.argv
        for sig in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
            signal.signal(sig, lambda *_: sys.exit(1))
        measure(Path(output), Path(workdir), swiftshader=False, settle=int(delay),
                capture=trace_command)
