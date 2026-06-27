#!/usr/bin/env python3
"""
runtests.py: boot the test image once and run every probe, checking each exits 0.

Boots qemu-system-i386 headless with the kernel and the test RAM disk, waits for
the kernel shell, then `exec`s each probe in turn and reads the kernel's
"Process exited with status N" line for it. A probe passes when it exits 0.

-cpu max is always passed: the entropy-dependent probe (rngprobe) fails closed
without RDSEED/RDRAND. Exit code: 0 if every probe exited 0, else 1.

This is the automated end-to-end pass over the test image. A handful of probes are
exercised by hand under tools/harness.py instead, because each needs a contract a
single shared boot does not set up: forktest and cowprobe exit with the child's
own status (not 0); stdintest blocks on real keyboard input; vmfault and vmro fault
on purpose (harness.py runs each in its own boot, where the fault cleanly ends that
boot); and pingtest, udptest, tcptest, sockettest, and dnstest need a host echo
server or live DNS.

Usage: runtests.py --kernel K --initrd I [--cpu max] [--timeout S]
"""
import argparse, os, re, select, subprocess, sys, time

# One boot runs all of these, in order. See the module docstring for the probes
# that are run by hand under harness.py instead.
PROBES = [
    "hello", "filetest", "shelltest", "dirtest", "stattest", "touchtest",
    "rmtest", "mvtest", "lstest", "dup2test", "auxvprobe", "tlsprobe",
    "errnoprobe", "vmprobe", "forkro", "dirprobe", "timeprobe", "sigprobe",
    "ofdprobe", "waitprobe", "pipeprobe", "pollprobe", "devprobe", "ttyprobe",
    "jobprobe", "miscprobe", "ptyprobe", "rngprobe",
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--initrd", required=True)
    ap.add_argument("--cpu", default="max")
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--steptmo", type=float, default=8.0)
    ap.add_argument("--log", default=None)
    a = ap.parse_args()

    cmd = ["qemu-system-i386", "-cpu", a.cpu, "-kernel", a.kernel,
           "-initrd", a.initrd, "-serial", "stdio", "-display", "none",
           "-no-reboot", "-netdev", "user,id=net0", "-device", "rtl8139,netdev=net0"]
    try:
        p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, bufsize=0)
    except OSError as e:
        sys.stderr.write("qemu launch failed: %s\n" % e)
        return 2

    fd = p.stdout.fileno()
    os.set_blocking(fd, False)
    buf = bytearray()
    deadline = time.time() + a.timeout
    logf = open(a.log, "wb") if a.log else None
    consumed = [0]

    def pump(until):
        while time.time() < until and time.time() < deadline and p.poll() is None:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                buf.extend(chunk)
                if logf:
                    logf.write(chunk); logf.flush()

    def expect(rx):
        pat = re.compile(rx.encode())
        stop = min(deadline, time.time() + a.steptmo)
        while time.time() < stop and p.poll() is None:
            m = pat.search(buf, consumed[0])
            if m:
                consumed[0] = m.end()
                return m
            pump(time.time() + 0.3)
        return None

    def send(text):
        try:
            p.stdin.write((text + "\n").encode()); p.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    if not expect(r"Type 'help'"):
        sys.stderr.write("runtests: kernel shell never appeared\n")
        return 3

    results = []
    for name in PROBES:
        send("exec " + name)
        m = expect(r"Process exited with status (-?\d+)")
        if m is None:
            results.append((name, "no exit (hang?)", False))
            continue
        status = int(m.group(1))
        results.append((name, "status=%d" % status, status == 0))

    pump(time.time() + 1.0)
    for fn in (p.terminate, p.kill):
        if p.poll() is None:
            try:
                fn()
            except OSError:
                pass
            time.sleep(0.3)
    if logf:
        logf.close()

    fails = 0
    for name, detail, ok in results:
        if not ok:
            fails += 1
        print("  %-12s %-18s %s" % (name, detail, "PASS" if ok else "FAIL"))

    print("runtests: %d probes, %d failures" % (len(results), fails))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
