"""Measure Chromium's immediate WebGPU picture under QEMU's headless Weston."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path("tests/graphics/browser").resolve()))
from run import Browser, hasvk_picture, save, server, stop


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


def picture_done(out):
    """Take a clock pair and read thread identities after the last frame."""
    before = time.monotonic_ns()
    epoch = time.time_ns()
    after = time.monotonic_ns()
    save(out / "clock.json", {"before": before, "epoch": epoch, "after": after})
    tids(out / "tids.json")


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
        weston = subprocess.Popen(
            ["weston", "--no-config", "--backend=headless", "--renderer=pixman",
             "--width=640", "--height=480", "--shell=kiosk", "--idle-time=0",
             "--socket=wayland-measure", "--log=" + str(out / "weston.log")],
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
            with server(work, out) as url, Browser("chromium", "webgpu", out, work) as browser:
                print("qemu: Chromium ready")
                result = hasvk_picture(browser, url, out / "trace.json.gz", swiftshader=True,
                                       picture_done=lambda: picture_done(out))
                save(out / "result.json", result)
                picture = result["picture"]
                if picture["frames"] != 480:
                    raise RuntimeError("expected 480 frames, got %d" % picture["frames"])
                print("qemu: result: 480 frames, %.3f s, %s" %
                      (picture["seconds"], picture["adapter"]["architecture"]))
        finally:
            stop(weston)


if __name__ == "__main__":
    main()
