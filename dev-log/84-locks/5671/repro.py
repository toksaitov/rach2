#!/usr/bin/env python3
# repro.py: boot machine.panic.kbd's kernel and image many times at once, as
# etude's QEMU command line does, and keep the state of each boot that ends
# without the double fault's panic (task 5671).
#
#   python3 -I repro.py KERNEL INITRD OUTDIR PARALLEL TOTAL [EXTRA_APPEND]
#
# ACCEL in the environment names QEMU's accelerator, kvm by default. A boot
# passes once its console says `panic: rebooting in`, and QEMU is quit then;
# one that ends by QMP's SHUTDOWN without it fails, and one that says
# neither in 30 seconds hangs. QEMU pauses at the guest's reset
# (`-action shutdown=pause`), so that `info registers` reads the machine
# before it quits; on AMD, KVM resets the processor at its shutdown
# intercept, so a triple fault there reads as the reset state. The guest's
# writes to port 0xe9 go to the boot's `dbg`. OUTDIR keeps the boots that
# did not pass; KEEP in the environment keeps every boot.
import json, os, select, shutil, socket, subprocess, sys, threading, time

kernel, initrd, out = sys.argv[1], sys.argv[2], sys.argv[3]
par, total = int(sys.argv[4]), int(sys.argv[5])
extra = sys.argv[6] if len(sys.argv) > 6 else ""
append = "rach2.rescue rach2.stackoverrun=fork panic=5 reboot=k " + extra
lock = threading.Lock()
counts = {"pass": 0, "fail": 0, "hang": 0, "other": 0}
started = [0]


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


def one(n):
    d = os.path.join(out, "b%05d" % n)
    os.makedirs(d, exist_ok=True)
    con, qmp = os.path.join(d, "console"), os.path.join(d, "q")
    argv = ["qemu-system-i386", "-accel", os.environ.get("ACCEL", "kvm"), "-nodefaults", "-display", "none",
            "-no-reboot", "-action", "shutdown=pause", "-cpu",
            "max,rdrand=on,rdseed=on,xsave=on,avx=on,enforce=on", "-smp", "1", "-m",
            "134217728B", "-kernel", kernel, "-initrd", initrd, "-serial", "file:" + con,
            "-debugcon", "file:" + os.path.join(d, "dbg"), "-qmp", "unix:%s,server=on,wait=off" % qmp, "-append", append]
    p = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=open(os.path.join(d, "stderr"), "w"))
    verdict = "other"
    try:
        q = None
        for _ in range(200):
            if os.path.exists(qmp):
                break
            time.sleep(0.02)
        q = Qmp(qmp)
        t0 = time.time()
        while True:
            text = open(con, "rb").read() if os.path.exists(con) else b""
            if b"panic: rebooting in" in text:
                verdict = "pass"
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
                text = open(con, "rb").read()
                verdict = "pass" if b"panic: rebooting in" in text else "fail"
                break
            if time.time() - t0 > 30:
                verdict = "hang"
                break
        if verdict in ("fail", "hang"):
            with open(os.path.join(d, "state"), "w") as f:
                f.write("events: %s\n" % json.dumps(q.events))
                f.write("status: %s\n" % json.dumps(q.cmd("query-status")))
                for line in ("info registers", "x/16i $eip", "x/16wx $esp",
                             "info tlb", "info mem"):
                    try:
                        f.write("== %s\n%s\n" % (line, q.hmp(line)))
                    except Exception as e:
                        f.write("== %s: %s\n" % (line, e))
        try:
            q.s.sendall(b'{"execute":"quit"}\n')
        except Exception:
            pass
    except Exception as e:
        with open(os.path.join(d, "error"), "w") as f:
            f.write(repr(e))
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
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
