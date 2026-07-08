#!/usr/bin/env python3
"""
runtests.py: boot the test image once and run every probe, checking each exits 0.

Boots qemu-system-i386 headless with the kernel and the test RAM disk, waits for
the kernel shell, then `exec`s each probe in turn and reads the kernel's
"Process exited with status N" line for it. A probe passes when it exits 0.

This depends on the test image holding no /sbin/init: without it the kernel falls
to the debug shell, which `exec`s each probe as a transient init that returns here
on exit (no `rach2.rescue` is needed, and none is passed). A real image instead
boots /sbin/init and never reaches this shell. Do not stage a /sbin/init in the
test image, or it would boot that and the harness would hang waiting for a prompt
that never comes.

-cpu max is always passed: the entropy-dependent probe (rngprobe) fails closed
without RDSEED/RDRAND. Exit code: 0 if every probe exited 0, else 1.

This is the automated end-to-end pass over the test image. A handful of probes are
exercised by hand under tools/harness.py instead, because each needs a contract a
single shared boot does not set up: forktest and cowprobe exit with the child's
own status (not 0); stdintest and ttyprobe block on real terminal input; vmfault,
vmro, and winprobe fault on purpose (harness.py runs each in its own boot, where
the kernel kills the faulting process and the shell returns); and pingtest,
udptest, tcptest, sockettest, and dnstest need a host echo server or live DNS.

Usage: runtests.py --kernel K --initrd I [--cpu max] [--timeout S]
"""
import argparse, os, re, select, subprocess, sys, time

# One boot runs all of these, in order. See the module docstring for the probes
# that are run by hand under harness.py instead.
PROBES = [
    "hello", "filetest", "shelltest", "dirtest", "stattest", "touchtest",
    "rmtest", "mvtest", "lstest", "dup2test", "auxvprobe", "tlsprobe",
    "errnoprobe", "vmprobe", "forkro", "cowregrant", "usercopyfault",
    "forkleak", "fdstale", "dirgrow", "dirprobe", "pathcanon", "timeprobe",
    "sigprobe", "credprobe", "ownprobe", "permprobe",
    "trapprobe", "ofdprobe", "waitprobe", "pipeprobe", "pollprobe", "devprobe",
    "jobprobe", "miscprobe", "ptyprobe", "rngprobe", "orphanprobe", "loopprobe",
    # The musl-linked identity and permission walks: prove the id family and
    # the deny paths through the real libc syscall selection, not just raw
    # int 0x80.
    "musl/authmusl",
    "musl/permmusl",
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--initrd", required=True)
    ap.add_argument("--cpu", default="max")
    # The step window is generous because only a failing probe ever waits it
    # out: a passing probe returns on its serial match, so the green path costs
    # nothing, and the overall --timeout still bounds a run full of hangs. A
    # tight window read as speed was really a bet on the host: 8 s was calibrated
    # on one machine and flaked on a slower one.
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--steptmo", type=float, default=20.0)
    # Boot-to-shell gets its own window: the whole self-test sequence runs
    # before the prompt, and on a slow host it alone can exceed a probe step.
    ap.add_argument("--boottmo", type=float, default=60.0)
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

    def expect(rx, window=None):
        pat = re.compile(rx.encode())
        stop = min(deadline, time.time() + (window or a.steptmo))
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

    if not expect(r"Type 'help'", window=a.boottmo):
        sys.stderr.write("runtests: kernel shell never appeared\n")
        return 3

    results = []
    for name in PROBES:
        start = len(buf)
        send("exec " + name)
        # The probe runs AS the init process; the kernel prints "Init process ...
        # returning to kernel shell" (on serial, so it survives a probe that left the
        # console tty in raw mode, e.g. ttyprobe) only when the probe itself exits,
        # after any children it forked. The kernel prints "Process exited with status
        # N" for EVERY process exit (a forked child too), so the probe's own status is
        # the LAST such line before that marker, not the first. Anchoring on the marker
        # also keeps us from sending the next exec while a forked parent still runs.
        m = expect(r"returning to kernel shell")
        if m is None:
            results.append((name, "no shell return (hang?)", False))
            break
        window = bytes(buf[start:m.start()])
        statuses = re.findall(rb"Process exited with status (-?\d+)", window)
        if not statuses:
            results.append((name, "no exit reported", False))
            continue
        status = int(statuses[-1])
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
