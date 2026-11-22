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
#   EXPECT  kernel variant: `fault` (the default) for an untraced descent,
#           `trace` for trace.patch alone or with cli.patch or touch.patch,
#           and `int8` for trace.patch with int8.patch.
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
#               traced kernels must also show the descent's `R` line and
#               then the double fault task's `D` line.
#   fail        QEMU reported a guest-originated `guest-reset` after the
#               descent began, with no panic. A traced kernel must show
#               the `R` line.
#   other       any other end, such as a panic that is not the variant's,
#               one before the descent, or a missing or unexpected trace.
#   incomplete  the boot neither ended nor wrote for QUIET seconds, or
#               the script was interrupted. Its state is kept when available.
#   error       the boot could not be launched or observed, for example
#               because a capture file could not be read. An error makes
#               the script exit with status 1.
# An interruption stops new boots and exits with status 128 plus the signal
# number after reaping each started QEMU.
#
# QEMU pauses at the guest's reset (`-action shutdown=pause`), so that `info
# registers` reads the machine before it quits; on AMD, KVM resets the
# processor at its shutdown intercept, so a triple fault there reads as the
# reset state. OUTDIR keeps the boots that did not pass, each with its
# console, dbg, QEMU's stderr and `state`. QEMU is quit, then killed, and
# always reaped, whatever the writes of the evidence do.
import json, os, re, select, shutil, signal, socket, subprocess, sys, threading, time

kernel, initrd, out = sys.argv[1], sys.argv[2], sys.argv[3]
par, total = int(sys.argv[4]), int(sys.argv[5])
extra = sys.argv[6] if len(sys.argv) > 6 else ""
append = "rach2.rescue rach2.stackoverrun=fork panic=5 reboot=k " + extra
expect = os.environ.get("EXPECT", "fault")
quiet = float(os.environ.get("QUIET", "120"))
lock = threading.Lock()
counts = {"pass": 0, "fail": 0, "other": 0, "incomplete": 0, "error": 0}
started = [0]
stop_signal = 0

DESCENT = b"kstack: overrun: pid"
REBOOT = b"panic: rebooting in"
FAULT_PANIC = b"panic: stopped: double fault: a kernel stack ran out"
VARIANTS = {
    "fault": (FAULT_PANIC, False),
    "trace": (FAULT_PANIC, True),
    "int8": (b"panic: stopped: unrecoverable CPU exception in kernel mode", True),
}
TRACE_FIELDS = rb"[0-9a-f]{8} [0-9a-f]{8} [0-9a-f]{8}\n"
DESCENT_TRACE = re.compile(rb"^R" + TRACE_FIELDS, re.M)
HANDLER_TRACE = re.compile(rb"^D" + TRACE_FIELDS, re.M)
if expect not in VARIANTS:
    sys.exit("repro: EXPECT must be fault, trace or int8, not %s" % expect)
if par < 1 or total < 1:
    sys.exit("repro: PARALLEL and TOTAL must be positive")
expected_panic, need_trace = VARIANTS[expect]


class Stopped(Exception):
    """The main thread received a signal and workers must reap their boots."""


def stop(signum, frame):
    global stop_signal
    stop_signal = signum


def check_stopped():
    if stop_signal:
        raise Stopped


class Qmp:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX)
        self.buf = b""
        self.events = []

    def connect(self, path):
        for _ in range(250):
            check_stopped()
            try:
                self.s.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.02)
        else:
            raise TimeoutError(path)
        if self.read(10) is None:
            raise TimeoutError("QMP greeting")
        self.cmd("qmp_capabilities")

    def read(self, wait):
        """One message, or None when none came within `wait` seconds."""
        end = time.monotonic() + wait
        while b"\n" not in self.buf:
            check_stopped()
            left = end - time.monotonic()
            if left <= 0:
                return None
            r, _, _ = select.select([self.s], [], [], min(left, 0.1))
            if not r:
                continue
            data = self.s.recv(65536)
            if not data:
                raise EOFError
            self.buf += data
        check_stopped()
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, name, args=None):
        check_stopped()
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
    with open(path, "rb") as f:
        return f.read()


def judge(con, dbg, shutdown):
    """Judge an ended boot from its console, debug port and SHUTDOWN data."""
    at = con.find(DESCENT)
    if at < 0:
        return "other"
    descent = DESCENT_TRACE.search(dbg)
    if (need_trace and descent is None) or (not need_trace and dbg):
        return "other"
    after = con[at:]
    panic = after.find(expected_panic)
    if panic >= 0 and after.find(REBOOT, panic) >= 0:
        if descent is not None and not HANDLER_TRACE.search(dbg, descent.end()):
            return "other"
        return "pass"
    if (shutdown.get("guest") is True and shutdown.get("reason") == "guest-reset"
            and b"panic:" not in after):
        return "fail"
    return "other"


def observe(q, con, dbgp):
    """Read the evidence until the boot ends or the observation stops."""
    shutdown = next((e for e in q.events if e["event"] == "SHUTDOWN"), None)
    size, last = -1, time.monotonic()
    while True:
        check_stopped()
        if shutdown is not None:
            time.sleep(0.3)
            check_stopped()
            return judge(slurp(con), slurp(dbgp), shutdown.get("data", {}))
        text, dbg = slurp(con), slurp(dbgp)
        if REBOOT in text:
            return judge(text, dbg, {})
        try:
            ev = q.read(0.1)
        except EOFError:
            return "other"
        if ev is not None and "event" in ev:
            q.events.append(ev)
            if ev["event"] == "SHUTDOWN":
                shutdown = ev
                continue
        if len(text) + len(dbg) != size:
            size, last = len(text) + len(dbg), time.monotonic()
        elif time.monotonic() - last > quiet:
            return "incomplete"


def keep_state(d, q, verdict):
    with open(os.path.join(d, "state"), "w") as f:
        f.write("verdict: %s\n" % verdict)
        f.write("events: %s\n" % json.dumps(q.events))
        f.write("status: %s\n" % json.dumps(q.cmd("query-status")))
        for line in ("info registers", "x/16i $eip", "x/16wx $esp", "info tlb", "info mem"):
            try:
                f.write("== %s\n%s\n" % (line, q.hmp(line)))
            except Stopped:
                raise
            except Exception as e:
                f.write("== %s: %r\n" % (line, e))


def one(n):
    d = os.path.join(out, "b%05d" % n)
    con, dbgp, qmp = os.path.join(d, "console"), os.path.join(d, "dbg"), os.path.join(d, "q")
    argv = ["qemu-system-i386", "-accel", os.environ.get("ACCEL", "kvm"), "-nodefaults", "-display", "none",
            "-no-reboot", "-action", "shutdown=pause", "-cpu",
            "max,rdrand=on,rdseed=on,xsave=on,avx=on,enforce=on", "-smp", "1", "-m",
            "134217728B", "-kernel", kernel, "-initrd", initrd, "-serial", "file:" + con,
            "-debugcon", "file:" + dbgp, "-qmp", "unix:%s,server=on,wait=off" % qmp, "-append", append]
    verdict = "error"
    made_dir = False
    p = None
    q = None
    try:
        check_stopped()
        os.makedirs(d)
        made_dir = True
        with open(os.path.join(d, "stderr"), "w") as err:
            check_stopped()
            p = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=err)
        q = Qmp()
        q.connect(qmp)
        verdict = observe(q, con, dbgp)
        if verdict != "pass":
            keep_state(d, q, verdict)
    except Stopped:
        if verdict == "error":
            verdict = "incomplete"
    except Exception as e:
        print("repro: boot %d: %r" % (n, e), file=sys.stderr, flush=True)
        if made_dir:
            try:
                with open(os.path.join(d, "error"), "w") as f:
                    f.write("%r\n" % e)
            except OSError:
                pass
    finally:
        try:
            if stop_signal and p is not None:
                p.kill()
            elif q is not None:
                q.s.sendall(b'{"execute":"quit"}\n')
        except OSError:
            pass
        try:
            if p is not None:
                try:
                    p.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
        finally:
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
            if stop_signal or started[0] >= total:
                return
            started[0] += 1
            n = started[0]
        one(n)


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
ts = [threading.Thread(target=worker) for _ in range(par)]
for t in ts:
    t.start()
for t in ts:
    t.join()
print("result: %s" % json.dumps(counts), flush=True)
if stop_signal:
    sys.exit(128 + stop_signal)
if counts["error"] or sum(counts.values()) != total:
    sys.exit(1)
