#!/usr/bin/env python3
"""Boot Rach2 on KVM and check the reversed stack without an i915 GPU."""

import argparse
import contextlib
import hashlib
import json
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time


def run(argv, **kwargs):
    """Run one command, propagating its failure."""
    return subprocess.run(argv, check=True, **kwargs)


def stop(child):
    """Stop and reap a private child, including after a failed boot."""
    if child is None:
        return
    with contextlib.suppress(ProcessLookupError):
        child.terminate()
    try:
        child.wait(timeout=10)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait()


def interrupted(signum, frame):
    """Unwind through cleanup when the caller stops the run."""
    raise KeyboardInterrupt


def main():
    """Keep the run's output; remove the overlays and reap every child."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("root", type=Path, help="the GNU root's raw partitioned disk")
    parser.add_argument("assets", type=Path, help="build.sh reverse's cache")
    parser.add_argument("output", type=Path, help="a new evidence directory")
    args = parser.parse_args()
    tree = Path(__file__).resolve().parents[2]
    assets = args.assets.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True)
    print("nctx-fallback: evidence:", out, flush=True)
    scratch = tree / "build/tmp"
    scratch.mkdir(parents=True, exist_ok=True)
    serial = out / "console"
    qemu = check = None
    for sig in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    ssh = ["ssh", "-i", str(tree / "krffs/rach2_demo_ed25519"), "-p", str(port),
           "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
           "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3",
           "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
           "-o", "LogLevel=ERROR", "root@127.0.0.1"]
    with tempfile.TemporaryDirectory(prefix="nctx-fallback.", dir=scratch) as work:
        work = Path(work)
        stage = work / "stage"
        scripts = stage / "bed/nctx"
        scripts.mkdir(parents=True)
        payload = stage / "assets"
        payload.mkdir()
        for name in ("reverse.sh", "nctx-env"):
            shutil.copy2(tree / "bed/nctx" / name, scripts / name)
        for name in ("reverse-vmlinuz", "reverse-initrd.img", "mesa-nctx.tar.xz",
                     "nctxcheck", "reverse.sha256", "id_ed25519"):
            shutil.copy2(assets / name, payload / name)
        (payload / "reverse.qcow2").symlink_to("/dev/disk2")
        run(["mke2fs", "-q", "-t", "ext2", "-F", "-d", str(stage),
             str(work / "payload.ext2"), "256M"])
        run(["qemu-img", "create", "-q", "-f", "qcow2", "-F", "raw", "-b",
             str(args.root.resolve()), str(work / "root.qcow2")])
        cpu = ("host,svm=on,npt=on,nrip-save=on,tsc-scale=on,vmcb-clean=on,"
               "flushbyasid=on,pause-filter=on,decodeassists=off,v-vmsave-vmload=off,vgif=off")
        qargs = ["qemu-system-x86_64", "-accel", "kvm", "-cpu", cpu,
                 "-smp", "4", "-m", "8192", "-machine", "q35", "-nodefaults",
                 "-no-reboot", "-display", "none", "-monitor", "none",
                 "-kernel", str(args.kernel.resolve()), "-append",
                 "rach2.root=ext2 rach2.tscreliable", "-serial", "file:" + str(serial),
                 "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{port}-:22",
                 "-device", "e1000,netdev=n0", "-device", "virtio-rng-pci"]
        disks = ((work / "root.qcow2", "qcow2"), (work / "payload.ext2", "raw"),
                 (assets / "reverse.qcow2", "raw"))
        for i, (path, fmt) in enumerate(disks):
            qargs += ["-drive", f"file={path},if=none,id=d{i},format={fmt}",
                      "-device", f"ide-hd,drive=d{i},bus=ide.{i}"]
        (out / "command.json").write_text(json.dumps(qargs, indent=2) + "\n")
        (out / "kernel.sha256").write_text(
            hashlib.sha256(args.kernel.read_bytes()).hexdigest() + "\n")
        try:
            with (out / "qemu.log").open("w") as log:
                qemu = subprocess.Popen(qargs, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            last = 0
            changed = time.monotonic()
            while subprocess.run(ssh + ["true"], stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
                size = serial.stat().st_size if serial.exists() else 0
                if size != last:
                    last, changed = size, time.monotonic()
                    print("nctx-fallback: boot:", size, "console bytes", flush=True)
                if qemu.poll() is not None or time.monotonic() - changed > 120:
                    raise RuntimeError("Rach2 ended or its boot stopped before SSH")
                time.sleep(1)
            command = ("mkdir -p /tmp/nctx-data && "
                       "mount -t ext2 -o ro /dev/disk1 /tmp/nctx-data && "
                       "sh /tmp/nctx-data/bed/nctx/reverse.sh fallback "
                       "/tmp/nctx-data/assets /tmp/nctx-result")
            with (out / "result.txt").open("w") as log:
                check = subprocess.Popen(ssh + [command], stdin=subprocess.DEVNULL,
                                         stdout=log, stderr=subprocess.STDOUT)
            last = -1
            changed = reported = time.monotonic()
            while check.poll() is None:
                now = time.monotonic()
                size = (out / "result.txt").stat().st_size + serial.stat().st_size
                if size != last:
                    last, changed = size, now
                if qemu.poll() is not None or now - changed > 600:
                    raise RuntimeError("Rach2 ended or the check stopped making progress")
                if now - reported >= 30:
                    print("nctx-fallback: running:", size, "output bytes", flush=True)
                    reported = now
                time.sleep(1)
            print((out / "result.txt").read_text(), end="", flush=True)
            if check.returncode:
                raise RuntimeError("the fallback check failed")
        finally:
            stop(check)
            try:
                if qemu is not None and qemu.poll() is None:
                    with contextlib.suppress(subprocess.TimeoutExpired):
                        with (out / "guest.tar").open("wb") as archive:
                            subprocess.run(ssh + ["tar cf - -C /tmp nctx-result"], stdout=archive,
                                           stderr=subprocess.DEVNULL, timeout=30, check=False)
                    with contextlib.suppress(subprocess.TimeoutExpired):
                        subprocess.run(ssh + ["poweroff -f"], stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL, timeout=15, check=False)
            finally:
                stop(qemu)
    print("nctx-fallback: result: pass", flush=True)


if __name__ == "__main__":
    main()
