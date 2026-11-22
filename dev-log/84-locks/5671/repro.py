#!/usr/bin/env python3
# repro.py: boot machine.panic.kbd's kernel and image many times at once, as
# etude's QEMU command line does, and keep the state of each boot that ends
# without the double fault's panic (task 5671).
#
#   python3 -I repro.py KERNEL INITRD OUTDIR PARALLEL TOTAL [EXTRA_APPEND]
#
# The environment:
#
#   ACCEL   QEMU's accelerator, kvm by default.
#   EXPECT  what a passing boot shows: `fault` (the default), the descent's
#           double fault, or `int8`, the double fault task entered by
#           `int $8` (int8.patch).
#   QUIET   seconds with no new console or debug port bytes after which a
#           boot that has not ended is judged `incomplete`, 120 by default.
#   KEEP    keep every boot's directory, not only those that did not pass.
#
# A boot ends at QMP's SHUTDOWN or once its console says `panic: rebooting
# in`, and is then judged from its console and its debug port (`dbg`, the
# guest's writes to port 0xe9, which only a kernel with trace.patch
# makes):
#
#   pass        the console has the descent's line, `kstack: overrun: pid`,
#               and after it the variant's panic and `panic: rebooting in`;
#               with a trace, the debug port has the descent's `R` line and
#               after it the double fault task's `D` line. EXPECT=int8 needs
#               the trace.
#   fail        the console has the descent's line and QEMU reported
#               SHUTDOWN with nothing of a panic after it: the double fault
#               was lost.
#   other       any other end, such as a panic that is not the variant's,
#               one before the descent, or a lost QMP connection.
#   incomplete  the boot neither ended nor wrote for QUIET seconds. This
#               says nothing of the guest: a host under load can delay a
#               boot that would pass. Its state is kept as a fail's is.
#
# QEMU pauses at the guest's reset (`-action shutdown=pause`), so that `info
# registers` reads the machine before it quits; on AMD, KVM resets the
# processor at its shutdown intercept, so a triple fault there reads as the
# reset state. OUTDIR keeps the boots that did not pass, each with its
# console, dbg, QEMU's stderr and `state`. QEMU is quit, then killed, and
# always reaped, whatever the writes of the evidence do.
import json, os, re, select, shutil, socket, subprocess, sys, threading, time

kernel, initrd, out = sys.argv[1], sys.argv[2], sys.argv[3]
par, total = int(sys.argv[4]), int(sys.argv[5])
extra = sys.argv[6] if len(sys.argv) > 6 else ""
append = "rach2.rescue rach2.stackoverrun=fork panic=5 reboot=k " + extra
expect = os.environ.get("EXPECT", "fault")
quiet = float(os.environ.get("QUIET", "120"))
lock = threading.Lock()
counts = {"pass": 0, "fail": 0, "other": 0, "incomplete": 0}
started = [0]

DESCENT = b"kstack: overrun: pid"
REBOOT = b"panic: rebooting in"
PANIC = {
    "fault": b"panic: stopped: double fault: a kernel stack ran out",
    "int8": b"panic: stopped: unrecoverable CPU exception in kernel mode",
}
if expect not in PANIC:
    sys.exit("repro: EXPECT must be fault or int8, not %s" % expect)


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(250):
            try:
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.02)
        self.buf = b""
        self.events = []
        self.read(10)
        self.cmd("qmp_capabilities")

    def read(self, wait):
        """One message, or None when none came within `wait` seconds."""
        end = time.time() + wait
        while b"\n" not in self.buf:
            left = end - time.time()
            if left <= 0:
                return None
            r, _, _ = select.select([self.s], [], [], left)
            if not r:
                return None
            data = self.s.recv(65536)
            if not data:
                raise EOFError
            self.buf += data
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, name, args=None):
        m = {"execute": name}
        if args:
            m["arguments"] = args
        self.s.sendall((json.dumps(m) + "\n").encode())
        while True:
            r = self.read(20)
            if r is None:
                raise TimeoutError(name)
            if "event" in r:
                self.events.append(r)
                continue
            return r

    def hmp(self, line):
        r = self.cmd("human-monitor-command", {"command-line": line})
        return r.get("return", json.dumps(r))


def slurp(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return b""


def traced(dbg):
    """Say whether the debug port has the descent's R line and, after it, the
    double fault task's D line."""
    m = re.search(rb"^R[0-9a-f]{8} [0-9a-f]{8} [0-9a-f]{8}$", dbg, re.M)
    return m is not None and re.search(rb"^D[0-9a-f]{8} ", dbg[m.end():], re.M) is not None


def judge(con, dbg, shutdown):
    """Judge an ended boot from its console, its debug port and whether QEMU
    reported SHUTDOWN."""
    at = con.find(DESCENT)
    if at < 0:
        return "other"
    after = con[at:]
    panic = after.find(PANIC[expect])
    if panic >= 0 and after.find(REBOOT, panic) >= 0:
        if expect == "int8" and not dbg:
            return "other"
        if dbg and not traced(dbg):
            return "other"
        return "pass"
    if shutdown and b"panic:" not in after:
        return "fail"
    return "other"


def keep_state(d, q, verdict):
    with open(os.path.join(d, "state"), "w") as f:
        f.write("verdict: %s\n" % verdict)
        f.write("events: %s\n" % json.dumps(q.events))
        f.write("status: %s\n" % json.dumps(q.cmd("query-status")))
        for line in ("info registers", "x/16i $eip", "x/16wx $esp", "info tlb", "info mem"):
            try:
                f.write("== %s\n%s\n" % (line, q.hmp(line)))
            except Exception as e:
                f.write("== %s: %r\n" % (line, e))


def one(n):
    d = os.path.join(out, "b%05d" % n)
    os.makedirs(d, exist_ok=True)
    con, dbgp, qmp = os.path.join(d, "console"), os.path.join(d, "dbg"), os.path.join(d, "q")
    argv = ["qemu-system-i386", "-accel", os.environ.get("ACCEL", "kvm"), "-nodefaults", "-display", "none",
            "-no-reboot", "-action", "shutdown=pause", "-cpu",
            "max,rdrand=on,rdseed=on,xsave=on,avx=on,enforce=on", "-smp", "1", "-m",
            "134217728B", "-kernel", kernel, "-initrd", initrd, "-serial", "file:" + con,
            "-debugcon", "file:" + dbgp, "-qmp", "unix:%s,server=on,wait=off" % qmp, "-append", append]
    with open(os.path.join(d, "stderr"), "w") as err:
        p = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=err)
    verdict = "other"
    q = None
    try:
        for _ in range(200):
            if os.path.exists(qmp):
                break
            time.sleep(0.02)
        q = Qmp(qmp)
        size, last = -1, time.time()
        while True:
            text, dbg = slurp(con), slurp(dbgp)
            if REBOOT in text:
                verdict = judge(text, dbg, False)
                break
            try:
                ev = q.read(0.1)
                if ev is not None and "event" in ev:
                    q.events.append(ev)
            except (EOFError, OSError):
                verdict = "other"
                break
            if any(e["event"] == "SHUTDOWN" for e in q.events):
                time.sleep(0.3)
                verdict = judge(slurp(con), slurp(dbgp), True)
                break
            if len(text) + len(dbg) != size:
                size, last = len(text) + len(dbg), time.time()
            elif time.time() - last > quiet:
                verdict = "incomplete"
                break
        if verdict != "pass":
            keep_state(d, q, verdict)
    except Exception as e:
        try:
            with open(os.path.join(d, "error"), "w") as f:
                f.write(repr(e))
        except OSError:
            pass
    finally:
        try:
            if q is not None:
                q.s.sendall(b'{"execute":"quit"}\n')
        except Exception:
            pass
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        if q is not None:
            q.s.close()
    with lock:
        counts[verdict] += 1
        if verdict == "pass" and not os.environ.get("KEEP"):
            shutil.rmtree(d, ignore_errors=True)
        else:
            print("boot %d: %s" % (n, verdict), flush=True)


def worker():
    while True:
        with lock:
            if started[0] >= total:
                return
            started[0] += 1
            n = started[0]
        one(n)


ts = [threading.Thread(target=worker) for _ in range(par)]
for t in ts:
    t.start()
for t in ts:
    t.join()
print("result: %s" % json.dumps(counts), flush=True)
