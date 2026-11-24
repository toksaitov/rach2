"""Force the wineboot hang of card 5798: one fresh boot of the games-wine
guest for each prefix, its system disk's reads throttled by QMP from the
prefix's start, so that wineboot's first read of Proton is slow.

usage: drive.py RUNDIR PLAN
PLAN is a comma-separated list of step names, run in order; see STEPS.
Each boot: QEMU at KVM, 4 processors, 8 GiB, virgl; root logs in on the
serial console, mounts the payload and starts one.py as the desktop account
in the background; once Weston is up the throttle is set and root makes the
go file. A hang is no ONE END (or ONE DONE) 120 s after the disk is back at
full speed. On a hang the captures are taken while it stands, the guest is
held up to HOLD s (or until RUNDIR/release exists), and the plan goes on.
"""
import json, os, re, socket, subprocess, sys, threading, time
from pathlib import Path

ROOT = Path("/home/toksaitov-s/rach2")
TREE = ROOT / "build/worktrees/5798"
KERNEL = TREE / "build/x86_64/rach2/kernel"
GNU = ROOT / "build/x86_64/images/gnu-test.disk"
PAYLOAD = ROOT / "build/tmp/t5798/payload.ext2"
PASSWORD = (ROOT / "build/tests/cases/credentials/rach2_demo_password").read_text().split("\n")[0]
VM = ROOT / "build/tmp/t5798/vm"
CARD = "5798"
HOLD = 7200
CAP = 900       # the longest a slow step keeps the disk slow
GRACE = 120     # a hang: no end this long after full speed
POST_EVERY = 10

# name: (throttle arguments, seconds the throttle holds or None for until
# the prefix ends or CAP)
STEPS = {
    "full": ({}, 0),
    "2MB/s": ({"bps_rd": 2000000}, None),
    "500KB/s": ({"bps_rd": 500000}, None),
    "50iops": ({"iops_rd": 50}, None),
    "5iops": ({"iops_rd": 5}, None),
    "pause60": ({"iops_rd": 1}, 60),
}

run_dir = Path(sys.argv[1])
plan = sys.argv[2].split(",")
for s in plan:
    if s not in STEPS:
        raise SystemExit("drive: no step %s" % s)
run_dir.mkdir(parents=True, exist_ok=True)
logf = open(run_dir / "drive.log", "a")


def now():
    return time.strftime("%H:%M:%S", time.localtime(time.time() + 3600))


def say(text):
    line = "drive: %s %s" % (now(), text)
    print(line, flush=True)
    logf.write(line + "\n")
    logf.flush()


class Console:
    """The serial console: a reader thread keeps every byte in a file and
    in memory, and wait() looks for a pattern after an offset."""

    def __init__(self, path, keep):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(str(path))
        self.text = bytearray()
        self.cond = threading.Condition()
        self.keep = open(keep, "ab")
        self.closed = False
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        while True:
            try:
                data = self.sock.recv(65536)
            except OSError:
                data = b""
            with self.cond:
                if not data:
                    self.closed = True
                    self.cond.notify_all()
                    return
                self.text += data
                self.keep.write(data)
                self.keep.flush()
                self.cond.notify_all()

    def size(self):
        with self.cond:
            return len(self.text)

    def wait(self, pattern, start, timeout):
        """Return the match of PATTERN after START, or None at TIMEOUT."""
        rx = re.compile(pattern.encode())
        end = time.time() + timeout
        with self.cond:
            while True:
                m = rx.search(self.text, start)
                if m:
                    return m
                left = end - time.time()
                if left <= 0 or self.closed:
                    return None
                self.cond.wait(min(left, 1.0))

    def type(self, line):
        self.sock.sendall(line.encode() + b"\r")


class QMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try:
                self.sock.connect(str(path))
                break
            except OSError:
                time.sleep(0.1)
        self.file = self.sock.makefile("rb")
        self.lock = threading.Lock()
        self.n = 0
        json.loads(self.file.readline())
        self.cmd("qmp_capabilities")

    def cmd(self, execute, **arguments):
        with self.lock:
            self.n += 1
            msg = {"execute": execute, "id": self.n}
            if arguments:
                msg["arguments"] = arguments
            self.sock.sendall(json.dumps(msg).encode() + b"\n")
            while True:
                line = self.file.readline()
                if not line:
                    raise OSError("qmp closed")
                reply = json.loads(line)
                if reply.get("id") == self.n:
                    return reply

    def hmp(self, line):
        return self.cmd("human-monitor-command", **{"command-line": line}).get("return", "")


def throttle(q, args):
    full = {"device": "d0", "bps": 0, "bps_rd": 0, "bps_wr": 0, "iops": 0, "iops_rd": 0,
            "iops_wr": 0}
    full.update(args)
    r = q.cmd("block_set_io_throttle", **full)
    if "error" in r:
        raise RuntimeError("throttle: %s" % r["error"])


def reads(q):
    for dev in q.cmd("query-blockstats").get("return", []):
        if dev.get("device") == "d0":
            s = dev["stats"]
            return s["rd_operations"], s["rd_bytes"]
    return None, None


def capture(q, con, hdir, why):
    """Take the hang's evidence while it stands."""
    hdir.mkdir(parents=True, exist_ok=True)
    say("capture: %s into %s" % (why, hdir))
    (hdir / "why").write_text(why + "\n")
    regs = []
    for i in range(3):
        text = q.hmp("info registers -a")
        regs.append(text)
        (hdir / ("registers.%d" % i)).write_text(text)
        if i < 2:
            time.sleep(5)
    r = q.cmd("pmemsave", val=0x100000, size=0x940000, filename=str(hdir / "kernel-image.bin"))
    say("capture: kernel image %s" % ("ok" if "return" in r else r))
    for block in regs[-1].split("CPU#")[1:]:
        cpu = int(block.split("\n", 1)[0])
        m = re.search(r"RSP=([0-9a-f]{16})", block)
        if m:
            q.cmd("memsave", val=int(m.group(1), 16), size=16384,
                  filename=str(hdir / ("stack.cpu%d" % cpu)), **{"cpu-index": cpu})
    virtio = {"list": q.cmd("x-query-virtio")}
    for dev in virtio["list"].get("return", []):
        p = dev["path"]
        virtio[p] = {"status": q.cmd("x-query-virtio-status", path=p), "queues": []}
        for n in range(8):
            r = q.cmd("x-query-virtio-queue-status", path=p, queue=n)
            if "error" in r:
                break
            virtio[p]["queues"].append(r)
    virtio["blockstats"] = q.cmd("query-blockstats")
    (hdir / "virtio.json").write_text(json.dumps(virtio, indent=1))
    say("capture: registers, stacks, virtio queues taken; asking the guest's shell")
    at = con.size()
    con.type("sh /tmp/games/dump.sh > /tmp/dump.txt 2>&1; cat /tmp/dump.txt; echo DUMP\"\"FINISHED")
    m = con.wait(r"DUMP" + "FINISHED", at, 90)
    say("capture: the guest's shell %s" % ("answered" if m else "did not answer in 90 s"))
    (hdir / "console-after").write_bytes(bytes(con.text[at:]))
    time.sleep(max(0, 120 - 90))
    (hdir / "registers.late").write_text(q.hmp("info registers -a"))
    (hdir / "virtio-late.json").write_text(json.dumps(
        {p: [q.cmd("x-query-virtio-queue-status", path=p, queue=n) for n in range(4)]
         for p in [d["path"] for d in virtio["list"].get("return", [])]}, indent=1))
    say("capture: done")


def serve(q, con):
    """While a hang is held: run each HMP line of RUNDIR/hmp.in into
    hmp.out, and type each line of con.in at the console."""
    f = run_dir / "hmp.in"
    if f.exists():
        out = []
        for line in f.read_text().splitlines():
            if line.strip():
                out.append("(qemu) %s\n%s" % (line, q.hmp(line)))
        (run_dir / "hmp.out.tmp").write_text("\n".join(out))
        os.rename(run_dir / "hmp.out.tmp", run_dir / "hmp.out")
        f.unlink()
    f = run_dir / "con.in"
    if f.exists():
        for line in f.read_text().splitlines():
            con.type(line)
        f.unlink()


def post(text):
    if os.environ.get("DRIVE_NOPOST"):
        return
    subprocess.run(["gh", "issue", "comment", CARD, "--repo", "toksaitov/rach2", "--body", text],
                   check=False, stdout=subprocess.DEVNULL)


def one(n, step):
    """Boot, make one prefix at STEP, power off. Return (result, seconds, detail)."""
    args, hold = STEPS[step]
    vm = VM / ("b%d" % n)
    subprocess.run(["rm", "-rf", str(vm)], check=True)
    vm.mkdir(parents=True)
    subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", "-F", "raw", "-b", str(GNU),
                    str(vm / "gnu.qcow2")], check=True)
    env = dict(os.environ, GALLIUM_DRIVER="d3d12", MESA_D3D12_DEFAULT_ADAPTER_NAME="NVIDIA",
               MESA_SHADER_CACHE_DIR=str(ROOT / "build/cache/mesa-host"))
    argv = ["qemu-system-x86_64", "-accel", "kvm", "-nodefaults",
            "-display", "egl-headless,rendernode=/dev/dri/renderD128", "-no-reboot",
            "-cpu", "host,rdrand=on,rdseed=on,xsave=on,avx=on,enforce=on,invtsc=on",
            "-smp", "4", "-m", "8192M",
            "-device", "virtio-vga-gl,edid=on,xres=1280,yres=800",
            "-kernel", str(KERNEL),
            "-drive", "file=%s,format=qcow2,if=none,id=d0" % (vm / "gnu.qcow2"),
            "-device", "virtio-blk-pci,drive=d0",
            "-drive", "file=%s,format=raw,if=none,id=d1,readonly=on" % PAYLOAD,
            "-device", "virtio-blk-pci,drive=d1",
            "-chardev", "socket,id=c,path=%s,server=on,wait=on" % (vm / "s"),
            "-serial", "chardev:c",
            "-chardev", "socket,id=q,path=%s,server=on,wait=off" % (vm / "q"),
            "-mon", "chardev=q,mode=control", "-append", "rach2.root=ext2"]
    q = None
    qemu = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL,
                            stdout=open(vm / "qemu.out", "wb"), stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            if (vm / "s").exists():
                break
            time.sleep(0.1)
        con = Console(vm / "s", run_dir / ("console.b%d" % n))
        q = QMP(vm / "q")
        t_boot = time.time()

        def stall(stage, timeout_at):
            hdir = run_dir / ("hang-b%d" % n)
            capture(q, con, hdir, "boot %d step %s: no %s" % (n, step, stage))
            post("Loop s: a stall at boot %d, step %s: %s did not come. Captures in "
                 "%s on the testing host; the guest is held for analysis." % (n, step, stage, hdir))
            held = time.time()
            while time.time() - held < HOLD and not (run_dir / "release").exists():
                serve(q, con)
                time.sleep(1)
            return ("stall-" + stage, time.time() - t_boot, str(hdir))

        if not con.wait(r"rach2 login", 0, 300):
            return stall("login prompt", 0)
        at = con.size()
        con.type("root")
        con.wait(r"Password:", at, 60)
        con.type(PASSWORD)
        if not con.wait(r"# $|# \Z", at, 60) and not con.wait(r"# ", at, 30):
            return stall("root prompt", 0)
        at = con.size()
        con.type("mkdir -p /tmp/games /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix && "
                 "mount -t ext2 -o ro /dev/disk1 /tmp/games && echo MOUNT\"\"ED")
        if not con.wait(r"MOUNTED", at, 60):
            return stall("payload mount", 0)
        at = con.size()
        con.type("su -s /bin/bash desktop -c 'python3 /tmp/games/one.py /tmp/one-out /tmp/one-go' "
                 "> /tmp/one.log 2>&1 & tail -f /tmp/one.log &")
        if not con.wait(r"ONE WESTON", at, 300):
            return stall("Weston", 0)
        booted = time.time() - t_boot
        rd0 = reads(q)
        throttle(q, args)
        at = con.size()
        con.type("touch /tmp/one-go")
        t0 = time.time()
        full_at = t0 if not args else (t0 + hold if hold is not None else None)
        slow = bool(args)
        last_log = t0
        while True:
            m = con.wait(r"ONE END[^\r\n]*\r?\n", at, 5)
            t = time.time()
            if slow and ((full_at is not None and t >= full_at) or
                         (hold is None and (m or t - t0 >= CAP))):
                throttle(q, {})
                slow = False
                full_at = t
                say("boot %d %s: disk at full speed after %.1f s; reads %s" %
                    (n, step, t - t0, reads(q)))
            if m:
                break
            if t - last_log >= 30:
                say("boot %d %s: %.0f s, reads %s" % (n, step, t - t0, reads(q)))
                last_log = t
            if not slow and t - full_at >= GRACE:
                return stall("ONE END %d s after full speed" % GRACE, 0)
        end_line = m.group(0).decode(errors="replace")
        took_end = time.time() - t0
        rd1 = reads(q)
        if slow:
            throttle(q, {})
        m2 = con.wait(r"ONE DONE[^\r\n]*\r?\n", at, GRACE)
        if not m2:
            return stall("ONE DONE %d s after ONE END" % GRACE, 0)
        detail = "%s; boot+weston %.1f s; read %d ops %d MB during the prefix" % (
            end_line.strip(), booted, rd1[0] - rd0[0], (rd1[1] - rd0[1]) // 1000000)
        ok = "made True" in end_line and "status 0" in end_line
        at = con.size()
        con.type("poweroff")
        try:
            qemu.wait(timeout=120)
        except subprocess.TimeoutExpired:
            say("boot %d: no power-off in 120 s; quitting QEMU" % n)
            q.cmd("quit")
        return ("made" if ok else "failed", took_end, detail)
    finally:
        if qemu.poll() is None:
            try:
                q.cmd("quit")
            except Exception:
                pass
            try:
                qemu.wait(timeout=60)
            except subprocess.TimeoutExpired:
                qemu.kill()
                qemu.wait()
        subprocess.run(["rm", "-rf", str(vm)], check=False)


results = []
say("start: plan %s; kernel %s" % (",".join(plan), KERNEL))
for n, step in enumerate(plan, 1):
    try:
        res, took, detail = one(n, step)
    except Exception as e:
        res, took, detail = "error", 0.0, repr(e)
    results.append((n, step, res, took, detail))
    say("RESULT boot %d step %s: %s in %.1f s: %s" % (n, step, res, took, detail))
    if n % POST_EVERY == 0 or n == len(plan):
        rows = [r for r in results[-POST_EVERY:]] if n % POST_EVERY == 0 else \
            results[(n // POST_EVERY) * POST_EVERY:]
        lines = ["* boot %d, %s: %s, prefix %.1f s (%s)" % (r[0], r[1], r[2], r[3], r[4])
                 for r in rows]
        hangs = sum(1 for r in results if r[2].startswith("stall"))
        post("Loop s, %s (UTC+6): %d prefixes done, %d stalled.\n\n%s" %
             (now(), n, hangs, "\n".join(lines)))
say("end: %d boots" % len(results))
