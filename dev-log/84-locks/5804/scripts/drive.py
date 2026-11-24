#!/usr/bin/env python3
"""t5804 host driver: cold wineboot in a fresh prefix on plain Debian under
KVM with the GNU image's Proton, its reads throttled by QMP
block_set_io_throttle on Debian's own QEMU, as loop s does for Rach2 on #5798.

  drive.py RUNDIR STEP:COUNT...      STEP is bps (2 MB/s) or iops (50)

Each run boots a fresh overlay of BASE (the prepared Debian image with
/opt/proton and rach2-game), starts Weston, sets the throttle on the system
disk, sends "go", lifts the throttle (all 0) at the prefix's end or 900 s,
and calls a stall when no end comes 120 s after full speed. A stall's guest
is held, after its captures, until RUNDIR/release appears."""
import datetime
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import time

TREE = Path(__file__).resolve().parents[3]
CACHE = TREE / "build/cache/debian-ref"
PREPARED = CACHE / "prepared.qcow2"
KEY = CACHE / "id_ed25519"
HERE = Path(__file__).resolve().parent
BASE = TREE / "build/cache/t5804/wine.qcow2"
STEPS = {"full": dict(bps_rd=0, iops_rd=0), "bps": dict(bps_rd=2000000, iops_rd=0), "iops": dict(bps_rd=0, iops_rd=50)}
CAP, HANG = 900, 120
ISSUE = "5804"
UTC6 = datetime.timezone(datetime.timedelta(hours=6))
QEMU = None


def now6():
    return datetime.datetime.now(UTC6).strftime("%H:%M:%S")


def say(text):
    print("t5804: %s %s" % (now6(), text), flush=True)


def stopped(signum, _frame):
    raise SystemExit(128 + signum)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class QMP:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.sock = socket.socket(socket.AF_UNIX)
                self.sock.connect(str(path))
                break
            except OSError:
                time.sleep(0.1)
        self.file = self.sock.makefile("rw")
        json.loads(self.file.readline())
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.file.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.file.flush()
        while True:
            reply = json.loads(self.file.readline())
            if "event" in reply:
                continue
            if "error" in reply:
                raise RuntimeError("qmp %s: %s" % (name, reply["error"]))
            return reply["return"]

    def hmp(self, line):
        return self.cmd("human-monitor-command", **{"command-line": line})

    def reads(self):
        for dev in self.cmd("query-blockstats"):
            if dev.get("device") == "d0":
                return dev["stats"]["rd_operations"], dev["stats"]["rd_bytes"]
        raise RuntimeError("no d0 in query-blockstats")

    def throttle(self, bps_rd=0, iops_rd=0):
        self.cmd("block_set_io_throttle", id="/machine/peripheral/vd0/virtio-backend", bps=0, bps_rd=bps_rd, bps_wr=0, iops=0,
                 iops_rd=iops_rd, iops_wr=0)


def words(port, disk, qmp, console):
    """QEMU's words for Debian on the virgl profile, as debian-ref.sh boots it,
    with the system disk named d0/vd0 and a QMP socket."""
    script = (
        'DISPLAY_PROFILE=virgl DISPLAY_MODE=none ARCH=x86_64 AUDIO_DEVICE=none '
        'NIC=virtio-net-pci NET_HOSTFWD="hostfwd=tcp:127.0.0.1:%d-:22" MEMORY=8192M; '
        '. "%s/rach2/qemu_devices.sh"; '
        'echo "$ACCEL_OPTS $NET_OPTS $DISPLAY_OPTS"; echo "$GALLIUM_DRIVER $MESA_D3D12_DEFAULT_ADAPTER_NAME"'
        % (port, TREE))
    out = subprocess.run(["sh", "-c", script], stdout=subprocess.PIPE, text=True,
                         check=True).stdout.splitlines()
    gallium, adapter = out[1].split()
    argv = (["qemu-system-x86_64"] + out[0].split() +
            ["-smp", "4", "-m", "8192M", "-machine", "q35", "-nodefaults", "-no-reboot",
             "-drive", "file=%s,if=none,id=d0,format=qcow2" % disk,
             "-device", "virtio-blk-pci,drive=d0,id=vd0", "-device", "virtio-rng-pci",
             "-serial", "file:%s" % console, "-monitor", "none",
             "-qmp", "unix:%s,server=on,wait=off" % qmp])
    env = dict(os.environ, GALLIUM_DRIVER=gallium, MESA_D3D12_DEFAULT_ADAPTER_NAME=adapter)
    return argv, env


class Guest:
    """A booted Debian guest on DISK (an overlay made of BASE when BASE is
    given, and removed at the end), reached by SSH as gfx and by QMP."""

    def __init__(self, rundir, disk, base=None):
        global QEMU
        self.dir = rundir
        self.disk = disk
        self.owned = base is not None
        if base is not None:
            subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", "-F", "qcow2", "-b",
                            str(base), str(disk)], check=True)
        self.port = free_port()
        self.qsock = rundir / "qmp.sock"
        if self.qsock.exists():
            self.qsock.unlink()
        argv, env = words(self.port, disk, self.qsock, rundir / "console.log")
        (rundir / "qemu.argv").write_text(" ".join(argv) + "\n")
        self.qemu = QEMU = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL,
                                            stdout=open(rundir / "qemu.out", "wb"),
                                            stderr=subprocess.STDOUT)
        self.opts = ["-i", str(KEY), "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
                     "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR"]
        self.ssh = ["ssh"] + self.opts + ["-p", str(self.port), "gfx@127.0.0.1"]
        self.qmp = QMP(self.qsock)
        for _ in range(180):
            if self.qemu.poll() is not None:
                raise RuntimeError("QEMU ended before SSH answered")
            if subprocess.run(["ssh", "-o", "ConnectTimeout=5"] + self.ssh[1:] + ["true"],
                              stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0:
                return
            time.sleep(1)
        raise RuntimeError("SSH did not answer in 180 tries")

    def run(self, command, out=None, timeout=600, stdin=None):
        result = subprocess.run(self.ssh + [command], stdin=stdin or subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=timeout)
        if out is not None:
            out.write_bytes(result.stdout)
        return result.returncode, result.stdout.decode(errors="replace")

    def put(self, *files):
        subprocess.run(["scp", "-q"] + self.opts + ["-P", str(self.port)] +
                       [str(f) for f in files] + ["gfx@127.0.0.1:/tmp/"], check=True,
                       stdin=subprocess.DEVNULL)

    def end(self):
        global QEMU
        try:
            self.run("sudo systemctl poweroff", timeout=30)
        except subprocess.TimeoutExpired:
            pass
        try:
            self.qemu.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.qemu.terminate()
            self.qemu.wait()
        QEMU = None
        if self.qsock.exists():
            self.qsock.unlink()
        if self.owned and self.disk.exists():
            self.disk.unlink()


def prepare(rundir):
    """Make BASE: the prepared Debian image with /opt/proton from the GNU
    image and /usr/local/bin/rach2-game, one boot."""
    if BASE.exists():
        return
    BASE.parent.mkdir(parents=True, exist_ok=True)
    tmp = BASE.with_suffix(".part")
    subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", "-F", "qcow2", "-b",
                    str(PREPARED), str(tmp)], check=True)
    pdir = rundir / "prepare"
    pdir.mkdir(exist_ok=True)
    say("prepare: booting %s" % tmp)
    g = Guest(pdir, tmp)
    try:
        tar = subprocess.Popen(["tar", "-C", str(HERE / "stage"), "-cf", "-", "proton",
                                "rach2-game"], stdout=subprocess.PIPE)
        status, text = g.run("sudo mkdir -p /opt/t5804 && sudo tar -xf - -C /opt/t5804 && "
                             "sudo mv /opt/t5804/proton /opt/proton && "
                             "sudo install -m 755 /opt/t5804/rach2-game /usr/local/bin/rach2-game && "
                             "sudo rm -rf /opt/t5804 && sudo chown -R root:root /opt/proton && "
                             "du -s /opt/proton && sudo sync", stdin=tar.stdout, timeout=1800)
        tar.wait()
    finally:
        g.end()
    (pdir / "install.log").write_text(text)
    if status != 0 or tar.returncode != 0:
        raise RuntimeError("prepare: install failed: %s" % text[-400:])
    tmp.rename(BASE)
    os.chmod(BASE, 0o444)
    say("prepare: result: %s, %s" % (BASE, text.strip().splitlines()[-1]))


def cpu_seconds(pid):
    stat = Path("/proc/%d/stat" % pid).read_text()
    fields = stat[stat.rindex(")") + 2:].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def loadavg():
    return " ".join(Path("/proc/loadavg").read_text().split()[:3])


def capture(g, rundir, result):
    """Take a stall's captures while the guest stands."""
    with open(rundir / "regs.txt", "w") as regs:
        for i in range(3):
            regs.write("=== %s take %d\n%s\n" % (now6(), i + 1, g.qmp.hmp("info registers -a")))
            if i < 2:
                time.sleep(5)
    (rundir / "blockstats.json").write_text(json.dumps(g.qmp.cmd("query-blockstats"), indent=1))
    g.put(HERE / "hang.py")
    g.run("sudo python3 /tmp/hang.py", out=rundir / "threads.txt", timeout=300)
    g.run("ps -eLo pid,tid,user,stat,wchan:32,comm,args", out=rundir / "ps.txt", timeout=120)
    g.run("cat /tmp/t5804-out/prefix.log; echo ===; cat /tmp/t5804.*/logs/steam-2733.log",
          out=rundir / "wine.log", timeout=120)
    g.run("sudo dmesg", out=rundir / "dmesg.txt", timeout=120)
    text = (rundir / "threads.txt").read_text(errors="replace")
    result["fx_same"] = text.count(" SAME ")
    result["fx_diff"] = text.count(" DIFF ")
    wine = (rundir / "wine.log").read_text(errors="replace")
    result["failmsg"] = "The explorer process failed to start" in wine
    result["nodrv"] = "no driver could be loaded" in wine


def one(index, step, base, top, record):
    rundir = top / ("%02d-%s" % (index, step))
    rundir.mkdir()
    result = {"run": index, "step": step, "start": now6()}
    say("run %d %s: booting" % (index, step))
    try:
        g = Guest(rundir, rundir / "run.qcow2", base)
    except BaseException:
        if QEMU is not None and QEMU.poll() is None:
            QEMU.terminate()
            QEMU.wait()
        (rundir / "run.qcow2").unlink(missing_ok=True)
        raise
    try:
        g.put(HERE / "one.py")
        log = rundir / "one.log"
        ssh = subprocess.Popen(g.ssh + ["python3 -u /tmp/one.py"], stdin=subprocess.PIPE,
                               stdout=open(log, "wb"), stderr=subprocess.STDOUT)
        t = time.monotonic()
        while "WESTON" not in log.read_text(errors="replace"):
            if ssh.poll() is not None or time.monotonic() - t > 300:
                raise RuntimeError("weston did not come up: %s" % log.read_text()[-300:])
            time.sleep(0.5)
        r0 = g.qmp.reads()
        g.qmp.throttle(**STEPS[step])
        cpu0, load0 = cpu_seconds(g.qemu.pid), loadavg()
        ssh.stdin.write(b"go\n")
        ssh.stdin.flush()
        t0 = time.monotonic()
        result["go"] = now6()
        say("run %d %s: go, throttle %s, reads before %d" % (index, step, STEPS[step], r0[0]))
        full = None
        blk = open(rundir / "blk.log", "w")
        last = -10
        while True:
            text = log.read_text(errors="replace")
            el = time.monotonic() - t0
            ended = "\nEND " in text or text.startswith("END ")
            if full is None and (ended or el >= CAP):
                g.qmp.throttle()
                full = el
                result["full_at"] = round(el, 1)
                say("run %d %s: full speed at %.0f s" % (index, step, el))
            if el - last >= 10:
                ops, nbytes = g.qmp.reads()
                blk.write("%.0f %d %d\n" % (el, ops - r0[0], nbytes - r0[1]))
                blk.flush()
                last = el
            if ended:
                break
            if full is not None and el - full >= HANG:
                ops, nbytes = g.qmp.reads()
                result.update(stall=True, reads=ops - r0[0], read_mb=round((nbytes - r0[1]) / 1e6),
                              qemu_cpu=round(cpu_seconds(g.qemu.pid) - cpu0), load=(load0, loadavg()))
                say("run %d %s: STALL: no end %d s after full speed; reads %d; capturing" % (
                    index, step, HANG, ops - r0[0]))
                capture(g, rundir, result)
                record.write(json.dumps(result) + "\n")
                record.flush()
                post("Loop m, #5804: a stall on Debian, run %d at %s: no end %d s after full "
                     "speed. Reads in the prefix %d requests, %d MB; futex words SAME %d, DIFF "
                     "%d; \"The explorer process failed to start\" %s. Captures in "
                     "build/runs/t5804/%s on the testing host (account toksaitov-m); the guest "
                     "is held." % (index, step, HANG, result["reads"], result["read_mb"],
                                   result["fx_same"], result["fx_diff"],
                                   "appears" if result["failmsg"] else "does not appear",
                                   rundir.name), (ISSUE, "5798"))
                say("run %d %s: held; touch %s/release to go on" % (index, step, top))
                while not (top / "release").exists():
                    time.sleep(10)
                (top / "release").rename(top / ("release.%02d" % index))
                ssh.kill()
                return result
            time.sleep(1)
        blk.close()
        ops, nbytes = g.qmp.reads()
        cpu1, load1 = cpu_seconds(g.qemu.pid), loadavg()
        t = time.monotonic()
        while "DONE" not in log.read_text(errors="replace") and time.monotonic() - t < 180:
            time.sleep(1)
        ssh.wait(timeout=120)
        text = log.read_text(errors="replace")
        end = re.search(r"^END status (-?\d+) secs ([\d.]+) made (\S+) explorer_seen (\S+) "
                        r"explorer_alive_at_end (\S+)", text, re.M)
        done = re.search(r"^DONE failmsg (\S+) nodrv (\S+) explorer_gone (\S+)", text, re.M)
        result.update(stall=False, status=int(end.group(1)), end_s=float(end.group(2)),
                      made_s=end.group(3), explorer_seen=end.group(4),
                      explorer_alive_at_end=end.group(5), reads=ops - r0[0],
                      read_mb=round((nbytes - r0[1]) / 1e6),
                      qemu_cpu=round(cpu1 - cpu0), load=(load0, load1))
        if done:
            result.update(failmsg=done.group(1) == "True", nodrv=done.group(2) == "True",
                          explorer_gone=done.group(3))
        g.run("cat /tmp/t5804-out/prefix.log; echo ===; cat /tmp/t5804-out/prefix.proton.log",
              out=rundir / "wine.log", timeout=120)
        say("run %d %s: result: %s" % (index, step, json.dumps(result)))
        return result
    finally:
        g.end()


def post(body, issues=(ISSUE,)):
    if os.environ.get("T5804_NOPOST"):
        say("post (not sent): %s" % body)
        return
    for issue in issues:
        subprocess.run(["gh", "issue", "comment", issue, "--repo", "toksaitov/rach2", "--body",
                        body], check=False, stdin=subprocess.DEVNULL)


def row(r):
    if r.get("stall"):
        return "| %d | %s | %s | stall (no end %d s after full speed at %s s) | %s / %s MB | %s | %s |" % (
            r["run"], r["step"], r["go"], HANG, r.get("full_at"), r["reads"], r["read_mb"],
            "see captures", "yes" if r.get("failmsg") else "no")
    fate = "seen %s; alive at end %s; gone %s" % (r["explorer_seen"], r["explorer_alive_at_end"],
                                                    r.get("explorer_gone"))
    return "| %d | %s | %s | made %s s, exit %s s (status %d) | %d / %d MB | %s | %s |" % (
        r["run"], r["step"], r["go"], r["made_s"], r["end_s"], r["status"], r["reads"],
        r["read_mb"], fate, "yes" if r.get("failmsg") else "no")


HEAD = ("| run | step | go (UTC+6) | prefix's end | reads in the prefix | explorer.exe "
        "(pid@first-last s after go) | \"explorer failed\" |\n|---|---|---|---|---|---|---|")


def main():
    for s in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(s, stopped)
    top = Path(sys.argv[1])
    top.mkdir(parents=True, exist_ok=True)
    plan = []
    for arg in sys.argv[2:]:
        step, count = arg.split(":")
        plan += [step] * int(count)
    claim = os.environ.get("T5804_CLAIM", "none")
    say("claim %s; plan %s" % (claim, " ".join(sys.argv[2:])))
    try:
        prepare(top)
        results = []
        with open(top / "results.jsonl", "a") as record:
            for i, step in enumerate(plan, 1):
                try:
                    r = one(i, step, BASE, top, record)
                except (RuntimeError, subprocess.SubprocessError, OSError) as error:
                    r = {"run": i, "step": step, "error": str(error)}
                    say("run %d %s: error: %s" % (i, step, error))
                if not r.get("stall"):
                    record.write(json.dumps(r) + "\n")
                    record.flush()
                results.append(r)
                if i % 5 == 0 or i == len(plan):
                    rows = "\n".join(row(x) if "error" not in x else
                                     "| %d | %s | | error: %s | | | |" % (x["run"], x["step"], x["error"])
                                     for x in results[-5:])
                    loads = "; ".join("run %d: load %s at go, %s at end, QEMU %s CPU s" % (
                        x["run"], *x.get("load", ("?", "?")), x.get("qemu_cpu", "?"))
                        for x in results[-5:])
                    post("Loop m, #5804, runs %d to %d of %d at %s (UTC+6), plain Debian 13 "
                         "(6.12 cloud kernel), KVM, 4 CPUs, 8 GiB, virgl; claim `%s` "
                         "(`cpu_millis 4000`, `memory_bytes 9663676416`).\n\n%s\n%s\n\n"
                         "Host: %s." % (i - len(results[-5:]) + 1, i, len(plan), now6(),
                                        claim, HEAD, rows, loads))
        say("result: %d runs, %d stalls" % (len(results),
                                             sum(1 for x in results if x.get("stall"))))
    finally:
        if QEMU is not None and QEMU.poll() is None:
            QEMU.terminate()
            QEMU.wait()


if __name__ == "__main__":
    main()
