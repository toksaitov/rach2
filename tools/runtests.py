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
    "suidprobe", "sudoprobe", "modeprobe",
    # The musl-linked identity and permission walks: prove the id family and
    # the deny paths through the real libc syscall selection, not just raw
    # int 0x80.
    "musl/authmusl",
    "musl/permmusl",
    "musl/suidmusl",
    # After those three, because it rewrites files they read and puts them back.
    # It is also the slowest probe here by a wide margin, and deliberately so: it
    # waits out the credential lock's real retry budget twice and sleeps inside a
    # held transaction once, which is why `make test` gives every step a window
    # wide enough for it.
    "musl/pwauthmusl",
    # After it, and for the same reasons doubled: it drives the shipped setuid
    # passwd, which takes the same lock and rewrites the same files, and it
    # restores all three databases before it exits.
    "musl/passwdmusl",
    "musl/addusermusl",
    "musl/loginmusl",
    "rebootprobe",
    "musl/readvmusl",
    "musl/timemusl",
    "musl/clockmusl",
    "musl/statmusl",
    "musl/stampmusl",
    # How many mappings a process may hold, which the compiler found too few.
    "musl/vmamusl",
    # The shell's one-line form, which `su -c` depends on. It runs
    # /musl/statushelper, a fixture and not a probe, which is why that one is
    # staged but not listed here.
    "musl/ishmusl",
    # Last: it flips the modes of a staged tree and mints fixtures outside it.
    # It puts every one back, but a probe that mutates shared state belongs
    # where a failure part-way through cannot be another probe's premise.
    "searchprobe",
    # Beside it for the same reason: it mints a tree of its own inside the two
    # staged directories of its sticky pair, and two more directories at the
    # image root, and removes every one of them again.
    "stickyprobe",
]

# The marker every probe's window ends on. Both kernel paths that hand the debug
# shell back carry their own outcome on this line: sys_exit its exit status,
# terminate_current_process the signal that killed the process.
#
# Matched from the start of the line through the whole kernel diagnostic, not on
# the trailing phrase alone. A probe writes to the same serial console the kernel
# does, so a pattern that accepts the suffix anywhere lets a probe print
# "exited with status 0, returning to kernel shell" itself and be credited before
# it then crashes or hangs. Requiring the component prefix and the pid keeps the
# verdict the kernel's to give.
EXIT_RE = (r"^sys_exit: init process \d+ exited with status (-?\d+), "
           r"returning to kernel shell")
KILL_RE = (r"^terminate: init process \d+ killed by signal (\d+), "
           r"returning to kernel shell")
SHELL_RETURN = "(%s|%s)" % (EXIT_RE[1:], KILL_RE[1:])


def score_window(window):
    """Decide one probe's result from the serial bytes its marker bounds.

    Scores the status the marker line itself carries, never a bare "Process
    exited with status N": the kernel prints that for every process exit, a
    forked child's included, so neither the first nor the last of them is
    reliably the probe's own. A child that outlives its parent prints after it,
    and a probe that reaps a clean child and then dies on a signal prints no
    status of its own at all, which is how a crash used to score as a pass.

    Returns a (detail, ok) pair.
    """
    killed = re.search(KILL_RE.encode(), window, re.M)
    if killed:
        return ("killed by signal %s" % killed.group(1).decode(), False)
    own = re.search(EXIT_RE.encode(), window, re.M)
    if not own:
        return ("no exit reported", False)
    status = int(own.group(1))
    return ("status=%d" % status, status == 0)


def selftest():
    """Check score_window against the serial shapes that fooled the old rule.

    Needs no QEMU, so the scoring rule is gated by `make verify-style` rather
    than only by a boot. Each case is the window a probe's marker would bound.
    """
    cases = [
        ("clean",
         b"\nProcess exited with status 0\n"
         b"sys_exit: init process 4 exited with status 0, "
         b"returning to kernel shell\n",
         ("status=0", True)),
        ("clean child then signal death",
         b"\nProcess exited with status 0\n"
         b"terminate: init process 4 killed by signal 11, "
         b"returning to kernel shell\n",
         ("killed by signal 11", False)),
        ("orphan exits after the parent",
         b"\nProcess exited with status 3\n"
         b"sys_exit: init process 4 exited with status 3, "
         b"returning to kernel shell\n"
         b"\nProcess exited with status 0\n",
         ("status=3", False)),
        ("no status at all",
         b"some unrelated serial chatter\n",
         ("no exit reported", False)),
        ("nonzero probe status",
         b"sys_exit: init process 4 exited with status 7, "
         b"returning to kernel shell\n",
         ("status=7", False)),
        # A probe shares the serial console with the kernel, so the phrase alone
        # must not be a verdict: this one prints it and then dies.
        ("probe forges the marker text, then is killed",
         b"probe: exited with status 0, returning to kernel shell\n"
         b"terminate: init process 4 killed by signal 11, "
         b"returning to kernel shell\n",
         ("killed by signal 11", False)),
        ("probe forges the marker text and nothing else happens",
         b"probe: exited with status 0, returning to kernel shell\n",
         ("no exit reported", False)),
    ]
    bad = 0
    for name, window, want in cases:
        got = score_window(window)
        if got != want:
            print("  FAIL: %s: expected %r, got %r" % (name, want, got))
            bad += 1
    if bad:
        print("runtests: selftest: %d of %d cases failed" % (bad, len(cases)))
        return 1
    print("runtests: selftest: %d cases PASS" % len(cases))
    return 0


def main():
    ap = argparse.ArgumentParser()
    # Not required, because --selftest boots nothing; checked below instead.
    ap.add_argument("--kernel")
    ap.add_argument("--initrd")
    ap.add_argument("--selftest", action="store_true")
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

    if a.selftest:
        return selftest()
    if not a.kernel or not a.initrd:
        sys.stderr.write("runtests: --kernel and --initrd are required\n")
        return 2

    # -rtc base=utc is QEMU's default and is passed anyway: this kernel reads
    # the CMOS as UTC (there is no timezone in those registers to read), so a
    # host configured for a local-time RTC would hand the guest a date wrong by
    # its offset, and the clock check below would report it as our bug.
    cmd = ["qemu-system-i386", "-cpu", a.cpu, "-kernel", a.kernel,
           "-initrd", a.initrd, "-serial", "stdio", "-display", "none",
           "-rtc", "base=utc", "-no-reboot",
           "-netdev", "user,id=net0", "-device", "rtl8139,netdev=net0"]
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
    clock_seen_at = None
    for name in PROBES:
        start = len(buf)
        send("exec " + name)
        # The probe runs AS the init process, and only its own exit returns the
        # kernel shell, so that marker bounds the window. Score the status the
        # marker itself carries: "Process exited with status N" is printed for
        # EVERY process exit, a forked child's included, and neither the first nor
        # the last of them is reliably the probe's own. A child that outlives its
        # parent prints after it, and a probe that reaps a clean child and then dies
        # on a signal prints no status of its own at all. The marker is on serial,
        # so it survives a probe that left the console tty in raw mode (ttyprobe).
        m = expect(SHELL_RETURN)
        if m is None:
            results.append((name, "no shell return (hang?)", False))
            break
        detail, ok = score_window(bytes(buf[start:m.end()]))
        results.append((name, detail, ok))
        if name == "musl/clockmusl":
            # Host time AT the probe, not at the end of the suite: the guest
            # printed its epoch a moment ago, so the two are comparable to the
            # second. Sampling after the remaining probes would leave a window
            # minutes wide, and a clock minutes wrong would pass.
            clock_seen_at = time.time()

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

    # The clock, end to end. timemusl checks that its own date is real and
    # advancing; only this side can check it against the clock QEMU handed the
    # machine, which is what catches a date wrong by hours rather than by years
    # (a local-time CMOS read as UTC, say). The anchor's own arithmetic is
    # pinned exactly in tests/host/rtc_hosttest.c and not here: that error is
    # under a second and no tolerance would see it.
    clock_ok = False
    clock_detail = "the probe printed no epoch"
    m = re.search(rb"CLOCKMUSL EPOCH (\d+)", bytes(buf))
    if m and clock_seen_at is not None:
        guest = int(m.group(1))
        skew = clock_seen_at - guest
        clock_detail = "%+d s vs the host" % int(skew)
        clock_ok = -30 <= skew <= 30
    fails = 0
    for name, detail, ok in results:
        if not ok:
            fails += 1
        print("  %-12s %-18s %s" % (name, detail, "PASS" if ok else "FAIL"))

    # A hang breaks the loop, so a short run is a failure in its own right and
    # names what never ran. Printing the smaller count alone would read as a
    # complete suite that happened to be smaller today.
    ran = [r[0] for r in results]
    missing = [n for n in PROBES if n not in ran]
    if missing:
        fails += len(missing)
        print("runtests: %d probe(s) never ran: %s"
              % (len(missing), " ".join(missing)))

    # Reported beside the probes rather than as one, so the count stays the
    # count of probes.
    print("  %-12s %-18s %s" % ("clock", clock_detail, "PASS" if clock_ok else "FAIL"))
    if not clock_ok:
        fails += 1

    print("runtests: %d of %d probes, %d failures"
          % (len(results), len(PROBES), fails))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
