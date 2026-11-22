#!/usr/bin/env python3
"""rach2.py: task 5676's measurement of piglit's streaming-texture-leak on
Rach2's GNU root under the virgl profile, KVM, 8 CPUs and 8 GiB, with QEMU's
virtio-gpu trace events on.

    python3 dev-log/86-shared-region/measure/rach2.py OUT

Boots an overlay of build/x86_64/images/gnu-test.disk with
build/x86_64/rach2/kernel, as the virgl suite's machine `gnu` boots it
(tests/cases/machine.virgl.json): the disk on virtio, rach2.root=ext2, the
virgl display of rach2/qemu_devices.sh, and a read-only ext2 data disk with
stl.sh, ioctlcount.c and tests/graphics/guest-env.sh. It logs in as root
at the serial console with the demo password, as graphics_scenario.c does,
copies the data disk to /tmp/stl, runs stl.sh as the test account with the
conformance class and GRAPHICS_DONE=1, prints its files, and powers the
guest off. OUT gets `console`, the whole serial log, `samples`, `result`
and `fdinfo`, stl.sh's files cut from it, and QEMU's `trace`. The run
holds a claim of the site's ledger, as debian-ref.sh's does.
"""
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DISK = os.path.join(TREE, "build/x86_64/images/gnu-test.disk")
KERNEL = os.path.join(TREE, "build/x86_64/rach2/kernel")
PASSWORD = os.path.join(TREE, "build/tests/cases/credentials/rach2_demo_password")


class Console:
    """QEMU's serial port on its standard output, logged whole to `log`."""

    def __init__(self, proc, log):
        self.proc = proc
        self.log = log
        self.buf = b""

    def send(self, text):
        self.proc.stdin.write(text.encode())
        self.proc.stdin.flush()

    def wait(self, pattern, seconds, quiet=None):
        """Wait for regex `pattern` in what came since the last match; give up
        after `seconds`, or after `quiet` seconds with no output."""
        rx = re.compile(pattern.encode())
        end = time.time() + seconds
        last = time.time()
        while True:
            m = rx.search(self.buf)
            if m:
                got = self.buf[:m.end()]
                self.buf = self.buf[m.end():]
                return got.decode(errors="replace")
            now = time.time()
            if now > end or (quiet and now - last > quiet) or self.proc.poll() is not None:
                raise RuntimeError("no %r on the console" % pattern)
            r, _, _ = select.select([self.proc.stdout], [], [], 1.0)
            if r:
                data = os.read(self.proc.stdout.fileno(), 65536)
                if data:
                    self.log.write(data)
                    self.log.flush()
                    self.buf += data
                    last = now

    def run(self, command, seconds, quiet=None):
        """Run `command` at the root prompt; its output and exit status."""
        self.send(command + '; echo "__ST""ATUS $?"\n')
        got = self.wait(r"__STATUS (\d+)", seconds, quiet)
        status = int(re.search(r"__STATUS (\d+)", got).group(1))
        self.wait(r"# ", 30)
        return got, status


def admitted():
    """Run this script again as a claim of the site's ledger, as
    debian-ref.sh's `admitted` does, with 8 CPUs and 8 GiB and QEMU's 1 GiB."""
    site = os.environ.get("ETUDE_SITE", "/etc/rach2/etude-site")
    if os.environ.get("STL_CLAIM") or not os.path.isfile(site):
        return
    fields = {}
    for line in open(site):
        k, _, v = line.strip().partition(" ")
        fields[k] = v
    subprocess.check_call(["make", "-s", "-C", TREE, "ARCH=i686", "build/tools/etude/etude"])
    claim = "stl-rach2:%s:%d:%d" % (os.environ.get("USER", "user"), os.getpid(), int(time.time()))
    argv = [os.path.join(TREE, "build/tools/etude/etude"), "admit", "-c", "-d", fields["locks"],
            "-w", fields["wait"], fields["ledger"], claim, os.environ.get("USER", "user"), "shared",
            "8000,%d,1" % ((8192 + 1024) * 1048576), fields["capacity"], fields["seconds"],
            shutil.which("env"), "STL_CLAIM=" + claim, sys.executable] + sys.argv
    os.execv(argv[0], argv)


def main():
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    admitted()
    for f in (DISK, KERNEL, PASSWORD):
        if not os.path.exists(f):
            sys.exit("rach2: no %s" % f)
    os.makedirs(os.path.join(TREE, "build/tmp"), exist_ok=True)
    work = tempfile.mkdtemp(prefix="rach2-stl.", dir=os.path.join(TREE, "build/tmp"))
    proc = None
    try:
        data = os.path.join(work, "data")
        os.mkdir(data)
        for f in (os.path.join(HERE, "stl.sh"), os.path.join(HERE, "ioctlcount.c"),
                  os.path.join(TREE, "tests/graphics/guest-env.sh")):
            shutil.copy(f, data)
        mke2fs = subprocess.check_output(
            ["sh", "-c", ". tools/setup/e2fsprogs_path.sh && command -v mke2fs"], cwd=TREE,
            text=True).strip()
        subprocess.check_call([mke2fs, "-q", "-t", "ext2", "-F", "-b", "4096", "-d", data,
                               os.path.join(work, "data.ext2"), "8m"])
        subprocess.check_call(["qemu-img", "create", "-q", "-f", "qcow2", "-F", "raw", "-b", DISK,
                               "-o", "cluster_size=65536", os.path.join(work, "gnu.qcow2")])
        trace = os.path.join(out, "trace")
        if os.path.exists(trace):
            os.unlink(trace)
        env = dict(os.environ, GALLIUM_DRIVER="d3d12", MESA_D3D12_DEFAULT_ADAPTER_NAME="NVIDIA")
        argv = ["qemu-system-x86_64", "-accel", "kvm", "-cpu", "host,invtsc=on", "-smp", "8",
                "-m", "8192M", "-nodefaults", "-no-reboot",
                "-vga", "none", "-device", "virtio-vga-gl,edid=on,xres=1280,yres=800",
                "-display", "egl-headless,rendernode=/dev/dri/renderD128",
                "-kernel", KERNEL, "-append", "rach2.root=ext2",
                "-drive", "file=%s,if=virtio,format=qcow2" % os.path.join(work, "gnu.qcow2"),
                "-drive", "file=%s,if=virtio,format=raw,readonly=on" % os.path.join(work, "data.ext2"),
                "-serial", "stdio", "-monitor", "none",
                "-trace", "events=" + os.path.join(HERE, "events"), "-D", trace,
                "-msg", "timestamp=on"]
        with open(os.path.join(out, "argv"), "w") as f:
            f.write(" ".join(argv) + "\n")
        log = open(os.path.join(out, "console"), "wb")
        proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
        c = Console(proc, log)
        c.wait(r"rach2 login", 900)
        time.sleep(2)
        c.send("root\n")
        c.wait(r"Password:", 60)
        c.send(open(PASSWORD).read().strip() + "\n")
        c.wait(r"# ", 120)
        _, st = c.run("mkdir -p /tmp/stl-data /tmp/stl && mount -t ext2 -o ro /dev/disk1 /tmp/stl-data"
                      " && cp /tmp/stl-data/* /tmp/stl/ && umount /tmp/stl-data && chown -R test /tmp/stl",
                      120)
        if st:
            raise RuntimeError("the data disk could not be copied")
        start = time.time()
        # stl.sh prints a sample a second; ten minutes with none is a hang.
        _, st = c.run("su -s /bin/sh test -c 'cd /tmp/stl && GRAPHICS_CLASS=conformance"
                      " GRAPHICS_DONE=1 STL_ECHO=1; export GRAPHICS_CLASS GRAPHICS_DONE STL_ECHO;"
                      " . /tmp/stl/guest-env.sh && graphics_env > /tmp/stl/graphics.env &&"
                      " sh /tmp/stl/stl.sh /tmp/stl /tmp/stl/out'", 4 * 3600, quiet=600)
        print("rach2: stl.sh: status %d after %.0f s" % (st, time.time() - start))
        for name in ("samples", "result", "fdinfo", "log"):
            got, _ = c.run("echo '==== %s'; cat /tmp/stl/out/%s; echo '==== end'" % (name, name), 120)
            m = re.search(r"\n==== %s\r?\n(.*?)==== end" % name, got, re.S)
            with open(os.path.join(out, name), "w") as f:
                f.write(m.group(1).replace("\r", "") if m else "")
        c.run("cat /tmp/stl/graphics.env; dmesg | tail -n 60", 120)
        c.send("poweroff -f\n")
        try:
            proc.wait(60)
        except subprocess.TimeoutExpired:
            pass
        return 0
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
