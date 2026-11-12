"""Run one Venus guest on the Lenovo's Debian host and reap it on every exit."""
import argparse
import contextlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
import time


def command(args, **kwargs):
    """Run a command, refusing a nonzero exit unless the caller handles it."""
    return subprocess.run(args, check=True, **kwargs)


def read(args):
    """Return a command's stripped text; propagate its error."""
    return subprocess.check_output(args, text=True).strip()


def identity():
    """Refuse a host other than the Lenovo's translation-off Debian on p5."""
    assert os.geteuid() == 0, "run as root on the Lenovo's Debian"
    assert read(["sfdisk", "--disk-id", "/dev/sda"]).lower() == \
        "1acef00d-0f1e-4d2c-8b3a-c0ffee000001", "wrong disk"
    assert read(["findmnt", "-n", "-o", "SOURCE", "/"]) == "/dev/sda5", "wrong root"
    assert read(["findmnt", "-n", "-o", "SOURCE", "/data"]) == "/dev/sda6", "wrong data partition"
    assert "intel_iommu=off" in Path("/proc/cmdline").read_text().split()
    assert not list(Path("/sys/kernel/iommu_groups").iterdir()), "IOMMU translation on"
    assert "[never]" in Path("/sys/kernel/mm/transparent_hugepage/shmem_enabled").read_text()
    assert not Path("/run/user/1000/wayland-77-10").exists(), "a task compositor is already running"
    assert "10.0.13" in read(["qemu-system-x86_64", "--version"]), "different QEMU"
    assert Path("/usr/share/vulkan/icd.d/intel_hasvk_icd.json").is_file(), "missing hasvk ICD"


def interrupted(signum, frame):
    """Unwind through the guest's cleanup on an operator's stop."""
    raise KeyboardInterrupt


def stop(child):
    """End a private process group and reap its leader, including on failure."""
    if child is None:
        return
    with contextlib.suppress(ProcessLookupError):
        os.killpg(child.pid, signal.SIGTERM)
    try:
        child.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass
    with contextlib.suppress(ProcessLookupError):
        os.killpg(child.pid, signal.SIGKILL)
    child.wait()


def main():
    """Keep logs and results, and remove only the private guest's overlay."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("system", choices=("debian", "rach2"))
    parser.add_argument("display", choices=("gtk", "sdl"))
    parser.add_argument("--part", choices=("all", "cube", "browser"), default="all",
                        help="what guest.sh runs: everything, vkcube's rounds or the WebGPU requests")
    parser.add_argument("--mode", default="",
                        help="OUTPUT=MODE of the guest's Weston for vkcube, as vkcube.sh -m takes it")
    parser.add_argument("--assets", type=Path, default=Path("/data/build/77-10"))
    args = parser.parse_args()
    identity()
    assets = args.assets.resolve()
    assert assets.is_relative_to(Path("/data/build")), "assets must be under /data/build"
    assert args.part != "browser" or args.display == "gtk", "the WebGPU requests run under GTK"
    assert not args.mode or "=" in args.mode, "--mode wants OUTPUT=MODE"
    tag = args.system + "-" + args.display
    if args.part != "all":
        tag += "-" + args.part
    if args.mode:
        tag += "-" + args.mode.split("=", 1)[1]
    out = assets / "runs" / tag
    out.mkdir(parents=True, exist_ok=False)
    scratch = assets / "tmp"
    scratch.mkdir(exist_ok=True)
    images = assets / "images"
    key = assets / ("debian.key" if args.system == "debian" else "rach2.key")
    user = "gfx" if args.system == "debian" else "root"
    ssh = ["ssh", "-i", str(key), "-p", "22662", "-o", "BatchMode=yes",
           "-o", "ConnectTimeout=5", "-o", "ServerAliveInterval=15",
           "-o", "ServerAliveCountMax=3", "-o", "StrictHostKeyChecking=accept-new",
           "-o", "UserKnownHostsFile=" + str(out / "known"), user + "@127.0.0.1"]
    privilege = "sudo " if args.system == "debian" else ""
    qemu = None
    weston = None
    serial = out / "serial.log"
    status = 1
    with tempfile.TemporaryDirectory(prefix=tag + ".", dir=scratch) as work:
        work = Path(work)
        command(["chown", "gfx:gfx", str(work)])
        overlay = work / "guest.qcow2"
        command(["qemu-img", "create", "-q", "-f", "qcow2", "-F", "qcow2", "-b",
                 str(images / (args.system + ".qcow2")), str(overlay)])
        command(["chown", "gfx:gfx", str(overlay)])
        serial.touch()
        command(["chown", "gfx:gfx", str(serial)])
        display = args.display + ",gl=on,full-screen=on"
        if args.display == "gtk":
            display += ",show-menubar=off"
        qargs = ["qemu-system-x86_64", "-name", "venus-77-10-" + tag,
                 "-accel", "kvm", "-cpu", "host", "-smp", "4", "-m", "6G",
                 "-object", "memory-backend-memfd,id=mem,size=6G,share=on",
                 "-machine", "q35,memory-backend=mem", "-no-reboot",
                 "-vga", "none", "-device", "virtio-vga-gl,venus=on,blob=on,hostmem=4G",
                 "-display", display, "-audiodev", "none,id=noaudio",
                 "-drive", "file=" + str(overlay) + ",if=none,id=root,format=qcow2",
                 "-device", "ide-hd,drive=root,bus=ide.0",
                 "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:22662-:22",
                 "-device", "e1000,netdev=n0", "-device", "virtio-rng-pci",
                 "-device", "virtio-keyboard-pci", "-device", "virtio-tablet-pci",
                 "-serial", "file:" + str(serial), "-monitor", "none"]
        if args.system == "rach2":
            qargs += ["-kernel", str(images / "kernel"), "-append", "rach2.root=ext2"]
        env = ["XDG_RUNTIME_DIR=/run/user/1000", "WAYLAND_DISPLAY=wayland-77-10",
               "GDK_BACKEND=wayland", "SDL_VIDEODRIVER=wayland",
               "SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR=0",
               "VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_hasvk_icd.json",
               "MESA_SHADER_CACHE_DIR=" + str(work / "mesa-cache"),
               "XDG_CACHE_HOME=" + str(work / "xdg-cache"),
               "LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libm.so.6"]
        (out / "command.json").write_text(json.dumps(qargs, indent=2) + "\n")
        (out / "environment").write_text("\n".join(env) + "\n")
        with (out / "host").open("w") as record:
            command(["sh", "-c", "uname -a; cat /proc/cmdline; cat /proc/loadavg; "
                     "cat /sys/kernel/mm/transparent_hugepage/shmem_enabled; "
                     "qemu-system-x86_64 --version; dpkg-query -W libgl1-mesa-dri "
                     "mesa-vulkan-drivers libvirglrenderer1 weston"], stdout=record)
        try:
            command(["install", "-d", "-o", "gfx", "-g", "gfx", "-m", "700", "/run/user/1000"])
            weston_log = out / "host-weston.log"
            weston_log.touch()
            command(["chown", "gfx:gfx", str(weston_log)])
            with (out / "host-weston.out").open("w") as log:
                weston = subprocess.Popen([
                    "runuser", "-u", "gfx", "--", "env", "XDG_RUNTIME_DIR=/run/user/1000",
                    "LIBSEAT_BACKEND=seatd", "MESA_SHADER_CACHE_DIR=" + str(work / "weston-cache"),
                    "XDG_CACHE_HOME=" + str(work / "weston-xdg"),
                    "weston", "--backend=drm", "--renderer=gl",
                    "--shell=kiosk", "--no-config", "--idle-time=0", "--socket=wayland-77-10",
                    "--log=" + str(weston_log)], stdin=subprocess.DEVNULL, stdout=log,
                    stderr=log, start_new_session=True)
            deadline = time.monotonic() + 60
            while not Path("/run/user/1000/wayland-77-10").is_socket():
                if weston.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("host Weston did not start; read " + str(weston_log))
                time.sleep(0.2)
            with (out / "qemu.log").open("w") as log:
                qemu = subprocess.Popen(["runuser", "-u", "gfx", "--", "env"] + env + qargs,
                                        stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                        start_new_session=True)
                deadline = time.monotonic() + 240
                while True:
                    if qemu.poll() is not None:
                        raise RuntimeError("QEMU exited; read " + str(out / "qemu.log"))
                    if subprocess.run(ssh + ["true"], stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL).returncode == 0:
                        break
                    if time.monotonic() >= deadline:
                        raise RuntimeError("guest SSH did not start; read " + str(serial))
                    time.sleep(2)
                command(ssh + [privilege + "mkdir -p /tmp/77-10"])
                with (assets / "payload.tar.gz").open("rb") as payload:
                    command(ssh + [privilege + "tar -xzf - -C /tmp/77-10"], stdin=payload)
                if args.system == "debian":
                    with (assets / "cts.tar.gz").open("rb") as payload:
                        command(ssh + ["sudo tar -xzf - -C /opt"], stdin=payload)
                remote = "cd /tmp/77-10 && " + privilege + "sh dev-log/77-vgpu/77-10/guest.sh "
                remote += shlex.join([args.system, args.display, args.part, args.mode])
                with (out / "guest.log").open("w") as log:
                    guest = subprocess.Popen(ssh + [remote], stdout=log, stderr=subprocess.STDOUT)
                    try:
                        status = guest.wait()
                    finally:
                        if guest.poll() is None:
                            guest.terminate()
                            try:
                                guest.wait(timeout=10)
                            except subprocess.TimeoutExpired:
                                guest.kill()
                                guest.wait()
                (out / "status").write_text(str(status) + "\n")
                with (out / "results.tar.gz").open("wb") as result:
                    command(ssh + [privilege + "tar -czf - -C /tmp/77-10/build/runs/results ."],
                            stdout=result)
                command(["tar", "-xzf", str(out / "results.tar.gz"), "-C", str(out)])
        finally:
            if qemu is not None and qemu.poll() is None:
                with contextlib.suppress(subprocess.TimeoutExpired):
                    subprocess.run(ssh + [privilege + "poweroff -f"], stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, timeout=20, check=False)
                try:
                    qemu.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    pass
            stop(qemu)
            stop(weston)
    print("venus-lenovo: result: %s status %d; output %s" % (tag, status, out), flush=True)
    return status


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    sys.exit(main())
