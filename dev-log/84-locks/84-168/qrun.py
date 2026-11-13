#!/usr/bin/env python3
"""qrun.py: run one command in a QEMU guest and keep its serial output.

  qrun.py rach2 KERNEL IMAGE OUT COMMAND
      boots the Rach2 KERNEL with the krffs IMAGE as its RAM disk into the
      debug shell, types COMMAND one character after the echo of the last,
      and types `poweroff` once the shell's prompt comes back.
  qrun.py linux KERNEL INITRD OUT
      boots the Linux KERNEL with INITRD, whose /init runs the command and
      powers off.

QEMU runs under KVM with 4 processors and 2 GiB, no network, the serial
console on a pipe. The whole of the serial output goes to OUT. The run ends
by itself: at the guest's power-off, or killed after LIMIT seconds.
"""
import os
import select
import subprocess
import sys
import time

LIMIT = 300


def qemu(kernel, initrd, append):
    args = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "max", "-smp", "4",
            "-m", "2048", "-display", "none", "-no-reboot", "-nic", "none",
            "-serial", "stdio", "-monitor", "none", "-kernel", kernel,
            "-initrd", initrd]
    if append:
        args += ["-append", append]
    return subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=0)


def main():
    mode, kernel, initrd, out = sys.argv[1:5]
    command = sys.argv[5] if len(sys.argv) > 5 else ""
    p = qemu(kernel, initrd, ("console=ttyS0 quiet " + os.environ.get("QAPPEND", "")).strip() if mode == "linux" else "")
    seen = b""
    start = time.time()
    typed = False
    off = False
    with open(out, "wb") as log:
        while True:
            if time.time() - start > LIMIT:
                p.kill()
                log.write(b"\nqrun: killed at the limit\n")
                break
            r, _, _ = select.select([p.stdout], [], [], 1.0)
            if r:
                data = os.read(p.stdout.fileno(), 65536)
                if not data:
                    break
                log.write(data)
                log.flush()
                seen += data
            if p.poll() is not None:
                break
            if mode != "rach2":
                continue
            if not typed and seen.endswith(b"> "):
                for ch in command.encode() + b"\r":
                    mark = len(seen)
                    p.stdin.write(bytes([ch]))
                    p.stdin.flush()
                    t0 = time.time()
                    while len(seen) == mark and time.time() - t0 < 5:
                        r, _, _ = select.select([p.stdout], [], [], 0.5)
                        if r:
                            data = os.read(p.stdout.fileno(), 65536)
                            log.write(data)
                            seen += data
                typed = True
                seen = b""
            elif typed and not off and b"WAKEMUSL" in seen and seen.endswith(b"> "):
                for ch in b"poweroff\r":
                    p.stdin.write(bytes([ch]))
                    p.stdin.flush()
                    time.sleep(0.05)
                off = True
        p.wait()
    print("qrun: %s: exit %d after %.0f s" % (mode, p.returncode, time.time() - start))


main()
