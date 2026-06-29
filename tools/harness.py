#!/usr/bin/env python3
"""
harness.py: Rach2's tracked QEMU serial harness for probe/test runs.

Boots qemu-system-i386 headless with the kernel and a KRFFS initrd, attaches
QEMU's isa-debug-exit device (iobase 0xF4), and drives the guest with an
expect/send script. With a HARNESS=1 kernel build the kernel writes each process
exit status to that port (see rach2/sys/kio.c harness_exit), so QEMU stops with
((status << 1) | 1) and we read a REAL exit code: a hang or triple-fault then
shows up as a timeout, not a missing serial marker.

This is the tracked replacement for the throwaway /tmp drive.py: the
isa-debug-exit wiring lives in the repo so probe pass/fail is reproducible.

Usage:
  harness.py --kernel K --initrd I [--net NETVAL] [--timeout S] [STEP ...]

  STEP forms:
    expect=REGEX   wait until REGEX appears (per-step cap = --steptmo)
    send=TEXT      write TEXT + newline to the guest serial
    wait=SECS      sleep SECS, draining serial
    exec=NAME|MARK run "exec NAME", re-sending if the shell drops it, until MARK
    probe=NAME     boot, run "exec NAME", and let the kernel's harness_exit stop
                   QEMU; the process exit code is then decoded from QEMU's.

Exit code: 0 if the guest process exited 0 (QEMU returned 1 via isa-debug-exit);
1 if it exited non-zero; 2 if QEMU failed to launch; 3 on timeout/hang (QEMU
never self-exited), an unambiguous failure.
"""
import argparse, os, re, select, subprocess, sys, time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", required=True)
    # --initrd is optional: a disk-only boot (--drive + rach2.fsdisk) needs no
    # RAM-disk module.
    ap.add_argument("--initrd", default=None)
    ap.add_argument("--append", default=None)
    # --drive may repeat; each value is passed verbatim to a QEMU -drive (e.g.
    # file=disk.img,format=raw,if=ide,index=0,media=disk).
    ap.add_argument("--drive", action="append", default=[])
    ap.add_argument("--net", default="user,id=net0")
    ap.add_argument("--device", default="rtl8139,netdev=net0")
    # --qemu-arg may repeat; each value is one token appended verbatim to the
    # QEMU command line, for topology the fixed flags do not cover (e.g. the
    # AHCI pair: --qemu-arg -device --qemu-arg ich9-ahci,id=ahci0). The NIC
    # --device stays wired, so a passthrough never disturbs it.
    ap.add_argument("--qemu-arg", action="append", default=[], dest="qemu_arg")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--steptmo", type=float, default=15.0)
    ap.add_argument("--grace", type=float, default=3.0)
    ap.add_argument("--bootwait", type=float, default=9.0)
    ap.add_argument("--log", default=None)
    ap.add_argument("--no-debug-exit", action="store_true")
    # --ok PATTERN passes iff PATTERN appears in the serial output, regardless of
    # the QEMU exit code. For a probe run in the debug shell (which returns to the
    # shell rather than stopping QEMU via isa-debug-exit), the printed success
    # token is the result.
    ap.add_argument("--ok", default=None)
    ap.add_argument("steps", nargs="*")
    a = ap.parse_args()

    # -no-reboot so a triple-fault exits QEMU (the harness reads it as failure)
    # rather than looping; NOT -no-shutdown, which would block the isa-debug-exit
    # device from actually stopping the VM.
    cmd = ["qemu-system-i386", "-kernel", a.kernel,
           "-serial", "stdio", "-display", "none", "-no-reboot",
           "-netdev", a.net, "-device", a.device]
    if a.initrd:
        cmd += ["-initrd", a.initrd]
    if a.append:
        cmd += ["-append", a.append]
    for d in a.drive:
        cmd += ["-drive", d]
    cmd += a.qemu_arg
    if not a.no_debug_exit:
        cmd += ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]

    # A probe=NAME step expands to: boot wait, exec NAME, then drain while the
    # kernel's harness_exit stops QEMU.
    steps = []
    for s in a.steps:
        if s.startswith("probe="):
            probe = s[len("probe="):]
            # Wait for the kernel shell prompt (robust against boot jitter), exec
            # the probe, then drain until the kernel's harness_exit stops QEMU
            # (bounded by the global deadline).
            steps += ["expect=Type 'help'", "execq=" + probe]
        else:
            steps.append(s)

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
        while time.time() < until and time.time() < deadline:
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
            if p.poll() is not None:
                try:
                    rest = os.read(fd, 1 << 20)
                    if rest:
                        buf.extend(rest)
                        if logf:
                            logf.write(rest); logf.flush()
                except OSError:
                    pass
                break

    def expect(rx):
        pat = re.compile(rx.encode())
        stop = min(deadline, time.time() + a.steptmo)
        while time.time() < stop and p.poll() is None:
            m = pat.search(buf, consumed[0])
            if m:
                consumed[0] = m.end()
                return True
            pump(time.time() + 0.3)
        return False

    def send(text):
        try:
            p.stdin.write((text + "\n").encode()); p.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    for step in steps:
        if p.poll() is not None:
            break
        if step.startswith("expect="):
            expect(step[len("expect="):])
        elif step.startswith("send="):
            send(step[len("send="):])
        elif step.startswith("wait="):
            pump(time.time() + float(step[len("wait="):]))
        elif step.startswith("exec="):
            name, _, marker = step[len("exec="):].partition("|")
            tries = 0
            while tries < 4 and p.poll() is None:
                send("exec " + name)
                if expect(marker):
                    break
                tries += 1
        elif step.startswith("execq="):
            # Run a probe, re-sending if the kernel shell drops the command, and
            # stop as soon as the kernel's harness_exit stops QEMU.
            name = step[len("execq="):]
            tries = 0
            while tries < 6 and p.poll() is None:
                send("exec " + name)
                pump(time.time() + 3.0)
                tries += 1
        else:
            sys.stderr.write("unknown step: %s\n" % step)

    pump(time.time() + a.grace)
    qemu_rc = p.poll()
    for fn in (p.terminate, p.kill):
        if p.poll() is None:
            try:
                fn()
            except OSError:
                pass
            time.sleep(0.4)
    pump(time.time() + 0.5)
    if logf:
        logf.close()

    # A serial-token check passes on the printed token alone, for a probe run in
    # the debug shell that never stops QEMU itself.
    if a.ok is not None:
        if a.ok.encode() in buf:
            sys.stderr.write("HARNESS: saw %r\n" % a.ok)
            return 0
        sys.stderr.write("HARNESS: pattern %r not seen\n" % a.ok)
        return 1

    if qemu_rc is None:
        sys.stderr.write("HARNESS: timeout/hang: QEMU never self-exited\n")
        return 3
    # Decode the result from the kernel's SERIAL status print, not from QEMU's
    # exit code: the host truncates exit codes to 8 bits, so ((status<<1)|1)
    # aliases (e.g. status 128 -> host 1) and a QEMU launch error also yields 1;
    # either would false-PASS. The serial line is the true, untruncated status and
    # also proves the guest actually ran. The exit code is used only to confirm
    # QEMU stopped via isa-debug-exit (an odd code) rather than hanging or crashing.
    last = None
    for mm in re.finditer(rb"Process exited with status (-?\d+)", buf):
        last = mm
    status = int(last.group(1)) if last else None
    via_debug_exit = (qemu_rc & 1) == 1
    sys.stderr.write("HARNESS: qemu_exit=%d via_debug_exit=%s guest_status=%s\n"
                     % (qemu_rc, via_debug_exit, "?" if status is None else status))
    if status is None or not via_debug_exit:
        sys.stderr.write("HARNESS: no clean guest exit reported, failing\n")
        return 1
    return 0 if status == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
