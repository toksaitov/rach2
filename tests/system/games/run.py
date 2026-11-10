#!/usr/bin/env python3
"""Run one row of the Wine probes through the game launcher under Xwayland."""
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

from wm import record, tone_windows

HERE = Path(__file__).resolve().parent
ROWS = ("prefix", "draw-64", "draw-32", "maps", "umu")
GAME_ID = "2733"
# What a refused call leaves in pressure-vessel's log: the strerror(3) text
# of an errno a kernel without the call or the permission returns.
REFUSED = re.compile(r"Operation not permitted|Function not implemented|Invalid argument|"
                     r"Permission denied|Operation not supported|Bad address|"
                     r"Inappropriate ioctl for device|No such device(?! or address)")


def refusal(line, work):
    """Return LINE of pressure-vessel's log without its time, process ids,
    the runner's work directory WORK and the six-character temporary names
    of pressure-vessel and the launcher, so that Rach2's lines compare with
    Linux's."""
    line = re.sub(r"^[0-9:.]+: ", "", line)
    line = re.sub(r"\[\d+\]", "[]", line)
    line = line.replace(str(work), "WORK")
    return re.sub(r"(tmp|\.rach2-game)[-.][A-Za-z0-9]{6}", r"\1-X", line)


# Wine 11's preloader reserves these ranges for a 64-bit process, which a
# WoW64 process is too (loader/preloader.c, preload_info): the DOS area, the
# low memory area, the 32-bit top-down area with the shared user data, and
# the 64-bit top-down area with the virtual heap.
PRELOADER = (
    ("dos", 0x000000010000, 0x00100000),
    ("low", 0x000000110000, 0x67ef0000),
    ("top32", 0x00007f000000, 0x00ff0000),
    ("top64", 0x7ffffe000000, 0x01ff0000),
)
CHILDREN = set()


def interrupted(signum, _frame):
    """Stop owned process groups before unwinding the workspace cleanup."""
    show_processes()
    for proc in list(CHILDREN):
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    raise SystemExit(128 + signum)


def say(text):
    """Print one progress or result line of the runner."""
    print("games: " + text, flush=True)


def show_processes():
    """
    Print the state and switch counts of each process of this account, so
    that a row stopped from outside shows whether its programs slept or ran.
    """
    uid = os.getuid()
    for entry in sorted(Path("/proc").glob("[0-9]*"), key=lambda p: int(p.name)):
        try:
            if entry.stat().st_uid != uid:
                continue
            text = (entry / "status").read_text(errors="replace")
        except OSError:
            continue
        field = dict(line.split(":\t", 1) for line in text.splitlines() if ":\t" in line)
        say("stopped: pid %s ppid %s %s state %s switches %s %s" % (
            entry.name, field.get("PPid"), field.get("Name"), field.get("State"),
            field.get("voluntary_ctxt_switches"), field.get("nonvoluntary_ctxt_switches")))


def start(argv, env, log):
    """Start ARGV in a session of its own with its output in LOG."""
    with log.open("wb") as dest:
        proc = subprocess.Popen(argv, env=env, stdout=dest, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, start_new_session=True)
    CHILDREN.add(proc)
    return proc


def finish(proc):
    """Wait for PROC, kill what is left of its group, and return its status."""
    try:
        return proc.wait()
    finally:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        CHILDREN.discard(proc)


def wait_for(path, pattern, proc):
    """Return the first match of PATTERN in PATH, or None once PROC has ended."""
    while True:
        text = path.read_text(errors="replace") if path.exists() else ""
        found = re.search(pattern, text, re.M)
        if found:
            return found
        if proc.poll() is not None:
            return None
        # Poll the file; elapsed time decides no verdict.
        time.sleep(0.05)


def show_logs(out):
    """Print the last lines of each launcher, umu and container log in OUT,
    which the guest keeps only until it powers off."""
    for log in sorted(out.glob("*.log")):
        if log.name.endswith(".proton.log"):
            continue
        lines = 120 if ".slr-" in log.name else 40
        for line in log.read_text(errors="replace").splitlines()[-lines:]:
            say("%s: %s" % (log.name, line))


def wait_path(path, proc):
    """Return once PATH exists; raise if PROC ends first."""
    while not path.exists():
        if proc.poll() is not None:
            raise RuntimeError("%s ended before it made %s" % (proc.args[0], path))
        # Poll the path; elapsed time decides no verdict.
        time.sleep(0.05)


def mem_mib():
    """Return MemTotal of /proc/meminfo in MiB, as the launcher computes it."""
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemTotal:"):
            return int(line.split()[1]) // 1024
    raise RuntimeError("no MemTotal")


def coverage(maps):
    """Return, for each preloader range, the bytes the MAPS text maps in it."""
    spans = []
    for line in maps.splitlines():
        lo, hi = (int(x, 16) for x in line.split()[0].split("-"))
        spans.append((lo, hi))
    result = {}
    for name, base, size in PRELOADER:
        end = base + size
        result[name] = sum(max(0, min(hi, end) - max(lo, base)) for lo, hi in spans)
    return result


def find_process(comm, uid):
    """Return the pid of the process of UID whose name is COMM, or None."""
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if entry.stat().st_uid == uid and (entry / "comm").read_text().strip() == comm[:15]:
                return int(entry.name)
        except OSError:
            continue
    return None


class Runner:
    """
    One row's workspace: Weston with Xwayland, the launcher and its logs.
    REFERENCE is true when the row records Linux's results in the Debian
    guest, which has neither Rach2's audio loopback nor its PulseAudio
    configuration.
    """

    def __init__(self, out, work, reference=False):
        self.out = out
        self.work = work
        self.reference = reference
        self.launcher = shlex.split(os.environ.get("GAMES_LAUNCHER", "rach2-game"))
        self.proton = Path(os.environ.get("GAMES_PROTON", "/opt/proton"))
        self.weston = None
        self.services = []
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(
            ("PROTON_", "WINE", "STEAM_", "DXVK_", "VKD3D_", "WAYLAND_", "LP_"))
            and k not in ("SteamGameId", "SteamAppId", "GALLIUM_DRIVER",
                          "LIBGL_ALWAYS_SOFTWARE", "DISPLAY", "XDG_DATA_HOME",
                          "XDG_CACHE_HOME")}
        run = work / "run"
        run.mkdir(mode=0o700)
        self.env.update(XDG_RUNTIME_DIR=str(run), XDG_DATA_HOME=str(work / "data"),
                        XDG_CACHE_HOME=str(work / "cache"),
                        MESA_SHADER_CACHE_DIR=str(work / "shader-cache"),
                        STEAM_COMPAT_CLIENT_INSTALL_PATH=str(work / "steam"),
                        PROTON_LOG_DIR=str(work / "logs"), WINEDEBUG="-all,err+all")
        self.prefix = work / "compat"
        self.umu = False
        self.record = {"uname": list(os.uname()), "memory_mib": mem_mib()}

    def start_weston(self):
        """
        Start a headless Weston on GL with Xwayland and export its display.
        Its shell is desktop-shell, so that the Debian guest's stock Weston,
        whose kiosk-shell dies as Wine's windows come and go, runs it too.
        """
        log = self.out / "weston.log"
        self.weston = start(["weston", "--backend=headless", "--renderer=gl", "--width=1280",
                             "--height=800", "--shell=desktop", "--idle-time=0", "--xwayland",
                             "--socket=games-wine", "--log=" + str(log)],
                            self.env, self.out / "weston.out")
        found = wait_for(log, r"xserver listening on display (:\d+)", self.weston)
        if not found:
            raise RuntimeError("weston ended without Xwayland")
        self.env["DISPLAY"] = found.group(1)
        text = log.read_text(errors="replace")
        renderer = re.search(r"GL renderer: (.*)", text)
        self.record["weston_renderer"] = renderer.group(1) if renderer else None
        self.record["display"] = found.group(1)
        say("weston: renderer %s display %s" % (self.record["weston_renderer"], found.group(1)))
        if not renderer or "virgl" not in renderer.group(1):
            raise RuntimeError("weston's GL renderer is not virgl")

    def start_services(self):
        """
        Start a D-Bus session bus and PulseAudio on the root's OSS
        configuration, as the desktop session does, with their sockets in the
        runner's runtime directory, and export their addresses, which
        pressure-vessel passes into its container. A reference run starts the
        bus alone, and only where dbus-daemon is installed.
        """
        run = Path(self.env["XDG_RUNTIME_DIR"])
        if self.reference and shutil.which("dbus-daemon") is None:
            return
        bus = start(["dbus-daemon", "--session", "--nofork", "--nopidfile",
                     "--address=unix:path=%s/bus" % run], self.env, self.out / "dbus.log")
        self.services.append(bus)
        wait_path(run / "bus", bus)
        self.env["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=%s/bus" % run
        if self.reference:
            return
        pulse = start(["pulseaudio", "-n", "--daemonize=no", "--use-pid-file=no",
                       "--exit-idle-time=-1", "--disallow-exit", "--file=/etc/pulse/rach2.pa"],
                      self.env, self.out / "pulseaudio.log")
        self.services.append(pulse)
        wait_path(run / "pulse" / "native", pulse)
        self.env["PULSE_SERVER"] = "unix:%s/pulse/native" % run

    def tone(self, name):
        """Run the tone probe as NAME while the loopback records; return its
        status, its output and the windows recorded, as wm.py sorts them: at
        440 Hz, whole at another frequency, and filled in part."""
        stop, ready = threading.Event(), threading.Event()
        samples = bytearray()
        failed = []

        def reader():
            try:
                record(stop, samples, ready)
            except Exception as error:  # reported by the row below
                failed.append(error)
                ready.set()

        thread = threading.Thread(target=reader)
        thread.start()
        try:
            ready.wait()
            if failed:
                raise RuntimeError("the loopback reader failed: %s" % failed[0])
            status, text = self.probe(name)
        finally:
            stop.set()
            thread.join()
        good, off, edges, kinds = tone_windows(bytes(samples))
        return status, text, good, len(off), edges, kinds

    def launch(self, name, program, extra=None, wait=True):
        """Run PROGRAM, a list, through the launcher as NAME, logging to NAME.log."""
        env = dict(self.env)
        env.update(extra or {})
        argv = self.launcher + ["--id", GAME_ID, "--prefix", str(self.prefix), "--"] + program
        if self.umu:
            # pressure-vessel's log, kept per launch, and the work directory
            # and the probes inside the container, where they keep their paths.
            argv.insert(len(self.launcher), "--umu")
            env.update(STEAM_LINUX_RUNTIME_LOG="1", STEAM_LINUX_RUNTIME_KEEP_LOGS="1",
                       STEAM_LINUX_RUNTIME_LOG_DIR=str(self.work / "slr"),
                       PRESSURE_VESSEL_FILESYSTEMS_RW="%s:%s" % (self.work, HERE))
        proc = start(argv, env, self.out / (name + ".log"))
        if not wait:
            return proc
        status = finish(proc)
        self.server_wait()
        for slr in sorted((self.work / "slr").glob("slr-*.log")):
            # slr-latest.log is pressure-vessel's link to the newest log.
            if slr.is_symlink():
                slr.unlink()
            else:
                shutil.move(str(slr), str(self.out / ("%s.%s" % (name, slr.name))))
        log = self.work / "logs" / ("steam-%s.log" % GAME_ID)
        if log.exists():
            shutil.copy(log, self.out / (name + ".proton.log"))
        return status

    def server_wait(self):
        """Wait for the prefix's Wine server to end, which saves the registry."""
        env = dict(self.env)
        env["WINEPREFIX"] = str(self.prefix / "pfx")
        subprocess.run([str(self.proton / "files/bin/wineserver"), "-w"], env=env, check=False)

    def make_prefix(self):
        """Make the prefix with the launcher."""
        status = self.launch("prefix", ["c:\\windows\\system32\\cmd.exe", "/c", "exit", "0"])
        text = (self.out / "prefix.log").read_text(errors="replace")
        made = re.search(r"rach2-game: prefix: made \S+", text)
        say("prefix: status %d %s" % (status, made.group(0) if made else "not made"))
        if status != 0 or not made:
            raise RuntimeError("the launcher did not make the prefix")

    def glx_mib(self):
        """Return the video memory GLX_MESA_query_renderer reports, in MiB, or None."""
        run = subprocess.run(["glxinfo", "-B"], env=self.env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, check=False)
        (self.out / "glxinfo.txt").write_text(run.stdout)
        found = re.search(r"^\s*Video memory: (\d+)MB$", run.stdout, re.M)
        return int(found.group(1)) if found else None

    def probe(self, name, extra=None):
        """Run the probe NAME.exe; return its status and its output."""
        output = self.work / (name + ".txt")
        env = {"PROBE_OUT": "Z:" + str(output)}
        env.update(extra or {})
        status = self.launch(name, [str(HERE / "probes" / (name + ".exe"))], env)
        text = output.read_text(errors="replace") if output.exists() else ""
        (self.out / (name + ".txt")).write_text(text)
        for line in text.splitlines():
            say("%s: %s" % (name, line))
        return status, text

    def row_prefix(self, results):
        """Check the new prefix and that WineD3D takes the driver's video memory."""
        self.make_prefix()
        pfx = self.prefix / "pfx"
        reg = (pfx / "user.reg").read_text(errors="replace")
        results["syswow64"] = (pfx / "drive_c/windows/syswow64").is_dir()
        results["key"] = '"VideoMemorySize"' in reg
        results["glx_mib"] = mib = self.glx_mib()
        status, text = self.probe("d3d964", {"WINEDEBUG": "-all,err+all,trace+d3d"})
        log = (self.out / "d3d964.proton.log").read_text(errors="replace")
        card = re.search(r"Card reports vendor PCI ID 0x[0-9a-f]+, device PCI ID 0x[0-9a-f]+, "
                         r"0x([0-9a-f]+) bytes of video memory", log)
        emulating = re.search(r"Emulating 0x([0-9a-f]+) bytes of video ram", log)
        results["wined3d_card"] = int(card.group(1), 16) if card else None
        results["wined3d_emulating"] = int(emulating.group(1), 16) if emulating else None
        results["probe"] = status
        say("prefix: syswow64 %s key %s glx %s MiB probe %d wined3d card %s emulating %s" % (
            results["syswow64"], results["key"], mib, status, results["wined3d_card"],
            results["wined3d_emulating"]))
        (self.out / "d3d964.proton.log").write_text(
            "\n".join(l for l in log.splitlines() if "bytes of video" in l or
                      "Emulating" in l or ":err:" in l) + "\n")
        return (results["syswow64"] and not results["key"] and mib is not None and
                results["wined3d_card"] == results["wined3d_emulating"] == mib << 20 and
                status == 0)

    def row_draw(self, bits, results, ref):
        """Run the GL and Direct3D 9 probes of BITS bits and compare their sums."""
        self.make_prefix()
        ok = True
        for name in ("gl", "d3d9"):
            status, text = self.probe(name + bits)
            crc = re.search(r"pixels crc32 ([0-9a-f]{8})", text)
            renderer = re.search(r"^(renderer|adapter): (.*)", text, re.M)
            results[name + bits] = {"exit": status, "crc32": crc.group(1) if crc else None,
                                    "renderer": renderer.group(2) if renderer else None}
            want = ref.get(name + bits) if ref is not None else None
            ok = ok and status == 0 and crc is not None and (ref is None or crc.group(1) == want)
            say("%s%s: exit %d crc32 %s linux %s" % (name, bits, status,
                                                     crc.group(1) if crc else None, want))
        if "virgl" not in (results["gl" + bits]["renderer"] or ""):
            say("gl%s: the renderer is not virgl" % bits)
            ok = False
        return ok

    def row_maps(self, results, ref):
        """Read each hold program's maps and compare the preloader's ranges."""
        self.make_prefix()
        ok = True
        for bits in ("64", "32"):
            name = "hold" + bits
            go = self.work / ("go" + bits)
            output = self.work / (name + ".txt")
            proc = self.launch(name, [str(HERE / "probes" / (name + ".exe"))],
                               {"PROBE_OUT": "Z:" + str(output), "PROBE_GO": "Z:" + str(go)},
                               wait=False)
            try:
                if not wait_for(output, r"^hold: ready", proc):
                    raise RuntimeError("%s ended before it was ready" % name)
                pid = find_process(name + ".exe", os.getuid())
                if pid is None:
                    raise RuntimeError("no process named %s.exe" % name)
                maps = Path("/proc/%d/maps" % pid).read_text()
                (self.out / (name + ".maps")).write_text(maps)
                go.touch()
                status = finish(proc)
            finally:
                if proc.poll() is None:
                    finish(proc)
            self.server_wait()
            have = coverage(maps)
            results[name] = {"exit": status, "coverage": have}
            want = ref.get(name) if ref is not None else None
            ok = ok and status == 0 and (ref is None or have == want)
            for range_name, base, size in PRELOADER:
                say("%s: %s %012x+%x mapped %d of %d, linux %s" % (
                    name, range_name, base, size, have[range_name], size,
                    want.get(range_name) if want else None))
        return ok

    @staticmethod
    def steps(name, text):
        """Print the launcher's and umu's own lines of the log text of NAME."""
        for line in text.splitlines():
            if re.match(r"(rach2-game:|INFO:|WARNING:|ERROR:|CRITICAL:)", line):
                say("%s: %s" % (name, line))

    def options(self, name):
        """Return the Options set, fsync and renderer lines of NAME's Proton log."""
        path = self.out / (name + ".proton.log")
        text = path.read_text(errors="replace") if path.exists() else ""
        found = re.search(r"^Options: (.*)$", text, re.M)
        return {"options": sorted(re.findall(r"'([^']*)'", found.group(1))) if found else None,
                "fsync": "fsync: up and running." in text,
                "opengl": "Using the OpenGL renderer." in text}

    def row_umu(self, results, ref):
        """
        Run the 64-bit Direct3D 9 probe through umu in the Steam Linux Runtime
        and directly, and compare Proton's settings and the pixels. The first
        launch unpacks the runtime; the second must download and unpack nothing.
        The 64-bit GL probe must draw on virgl inside the container, through
        the /dev/dri that pressure-vessel passes in, and the tone probe must be
        recorded through the loopback from PulseAudio's socket there, as it is
        directly; the container must have the session bus. The container's log
        must hold no refused call that the stored Linux reference's lacks. A
        reference run records the refused calls and leaves out the sound and
        the session's services, which the Debian guest does not have.
        """
        self.start_services()
        self.umu = True
        self.make_prefix()
        first = (self.out / "prefix.log").read_text(errors="replace")
        self.steps("prefix", first)
        unpacked = re.search(r"rach2-game: umu: unpacked the Steam Linux Runtime (\S+)", first)
        status, text = self.probe("d3d964", {"WINEDEBUG": "-all,err+all"})
        second = (self.out / "d3d964.log").read_text(errors="replace")
        self.steps("d3d964", second)
        crc = re.search(r"pixels crc32 ([0-9a-f]{8})", text)
        adapter = re.search(r"^adapter: (.*)", text, re.M)
        gl_status, gl_text = self.probe("gl64", {"WINEDEBUG": "-all,err+all"})
        gl_crc = re.search(r"pixels crc32 ([0-9a-f]{8})", gl_text)
        renderer = re.search(r"^renderer: (.*?) vendor ", gl_text, re.M)
        tone = self.tone("tone64") if not self.reference else (0, "", 0, 0, 0, [])
        inside = {}
        for log in sorted(self.out.glob("tone64.slr-*.log")):
            for key, value in re.findall(r"pv-adverb\[\d+\]: D: \t'(PULSE_SERVER|"
                                         r"DBUS_SESSION_BUS_ADDRESS)=([^']*)'",
                                         log.read_text(errors="replace")):
                inside[key] = value
        cache = Path(self.env["XDG_CACHE_HOME"]) / "umu"
        downloads = sorted(p.name for p in cache.glob("*")) if cache.is_dir() else []
        results["umu"] = dict(self.options("d3d964"), exit=status,
                              crc32=crc.group(1) if crc else None,
                              adapter=adapter.group(1) if adapter else None,
                              gl_exit=gl_status, gl_crc32=gl_crc.group(1) if gl_crc else None,
                              renderer=renderer.group(1) if renderer else None,
                              tone_exit=tone[0], tone_ok="TONE OK" in tone[1],
                              tone_windows=tone[2], tone_off=tone[3],
                              tone_edges=tone[4], tone_kinds=tone[5],
                              pulse=inside.get("PULSE_SERVER"),
                              bus=inside.get("DBUS_SESSION_BUS_ADDRESS"),
                              unpacked=unpacked.group(1) if unpacked else None,
                              again="unpacked" in second or "Downloading" in second or
                              "Downloading" in first, cache=downloads)
        self.server_wait()
        refused = []
        for log in sorted(self.out.glob("*.slr-*.log")):
            for line in log.read_text(errors="replace").splitlines():
                if REFUSED.search(line):
                    refused.append(refusal(line, self.work))
        results["refused"] = refused
        linux = set(ref.get("umu_refused", [])) if ref is not None else set()
        refused = [line for line in refused if line not in linux]
        for line in refused:
            say("umu: refused: %s" % line)
        self.umu = False
        self.prefix = self.work / "direct"
        self.make_prefix()
        status, text = self.probe("d3d964", {"WINEDEBUG": "-all,err+all"})
        tone = self.tone("tone64") if not self.reference else (0, "", 0, 0, 0, [])
        results["direct"] = dict(self.options("d3d964"), exit=status, tone_exit=tone[0],
                                 tone_ok="TONE OK" in tone[1], tone_windows=tone[2],
                                 tone_off=tone[3], tone_edges=tone[4], tone_kinds=tone[5])
        u, d = results["umu"], results["direct"]
        want = ref["draw"].get("d3d964") if ref is not None else None
        say("umu: runtime %s exit %d crc32 %s linux %s adapter %s options %s fsync %s opengl %s" % (
            u["unpacked"], u["exit"], u["crc32"], want, u["adapter"], u["options"], u["fsync"],
            u["opengl"]))
        gl_want = ref["draw"].get("gl64") if ref is not None else None
        say("umu: gl64 exit %d crc32 %s linux %s renderer %s" % (
            u["gl_exit"], u["gl_crc32"], gl_want, u["renderer"]))
        for route, r in (("umu", u), ("direct", d)):
            say("umu: tone %s exit %d ok %s windows %d off %d partial %d %s" % (
                route, r["tone_exit"], r["tone_ok"], r["tone_windows"], r["tone_off"],
                r["tone_edges"], r["tone_kinds"]))
        say("umu: container PULSE_SERVER %s DBUS_SESSION_BUS_ADDRESS %s" % (u["pulse"], u["bus"]))
        say("umu: direct exit %d options %s fsync %s opengl %s; second start fetched %s cache %s" % (
            d["exit"], d["options"], d["fsync"], d["opengl"], u["again"], u["cache"]))
        heard = self.reference or (all(
            r["tone_exit"] == 0 and r["tone_ok"] and r["tone_windows"] >= 20 and
            r["tone_off"] * 5 <= r["tone_windows"] for r in (u, d)) and
            u["pulse"] is not None and u["bus"] is not None)
        return (u["unpacked"] is not None and not u["again"] and not u["cache"] and
                u["exit"] == 0 and d["exit"] == 0 and u["crc32"] is not None and
                (want is None or u["crc32"] == want) and u["options"] is not None and
                u["options"] == d["options"] and "wow64" in u["options"] and
                "wined3d" in u["options"] and u["fsync"] and d["fsync"] and u["opengl"] and
                d["opengl"] and u["gl_exit"] == 0 and u["gl_crc32"] is not None and
                (gl_want is None or u["gl_crc32"] == gl_want) and
                "virgl" in (u["renderer"] or "") and heard and not refused)

    def stop(self):
        """End the Wine server, the session's services and Weston and reap them."""
        if self.prefix.exists():
            env = dict(self.env)
            env["WINEPREFIX"] = str(self.prefix / "pfx")
            server = str(self.proton / "files/bin/wineserver")
            subprocess.run([server, "-k"], env=env, check=False)
            subprocess.run([server, "-w"], env=env, check=False)
        for proc in self.services:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            finish(proc)
        if self.weston is not None:
            try:
                os.killpg(self.weston.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            finish(self.weston)


def main():
    """Run ROW in OUT; compare with ref/linux.json unless GAMES_REFERENCE is set."""
    if len(sys.argv) != 3 or sys.argv[1] not in ROWS:
        raise SystemExit("usage: run.py prefix|draw-64|draw-32|maps|umu OUT")
    row, output = sys.argv[1:]
    if os.geteuid() == 0:
        raise SystemExit("games: run as an ordinary user")
    out = Path(output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    reference = os.environ.get("GAMES_REFERENCE")
    ref = None if reference else json.loads((HERE / "ref" / "linux.json").read_text())
    work = Path(tempfile.mkdtemp(prefix="rach2-games.", dir=os.environ.get("TMPDIR", "/tmp")))
    runner = None
    results = {}
    try:
        runner = Runner(out, work, reference=bool(reference))
        runner.start_weston()
        if row == "prefix":
            ok = runner.row_prefix(results)
        elif row == "maps":
            ok = runner.row_maps(results, None if ref is None else ref["maps"])
        elif row == "umu":
            ok = runner.row_umu(results, ref)
        else:
            bits = row.split("-")[1]
            ok = runner.row_draw(bits, results, None if ref is None else ref["draw"])
        results["record"] = runner.record
        (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        if reference:
            Path(reference).write_text(json.dumps({row: results}, indent=2) + "\n")
        if not ok:
            show_logs(out)
            say("result: %s differs; see %s" % (row, out))
            return 1
        print("GAMESROW OK %s" % row, flush=True)
        return 0
    except BaseException:
        show_logs(out)
        raise
    finally:
        if runner is not None:
            runner.stop()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    for stop_signal in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(stop_signal, interrupted)
    sys.exit(main())
