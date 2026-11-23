#!/usr/bin/env python3
"""Accept Crysis on a trial root: the flyby, the suit's reboot and the saves.

Usage, as root, in the desktop session of the GNU root with the game in
/data/crysis, which the native case crysis.sh (tests/native) makes a private
copy of the owner's, since every command but `key` writes there:

    crysis.py prepare OUT
    crysis.py flyby ROUTE NAME [LOOPS]
    crysis.py hud NAME SAVE [ROUTE]
    crysis.py save NAME [ROUTE]
    crysis.py load NAME SAVE [ROUTE [PREFIX]]

ROUTE is direct, umu or 32: crysis.sh run, run --umu or run --32. The
desktop account, which runs the game, must be able to write /data/crysis.
`prepare` starts the desktop session, by SIGHUP when none runs, sets its
output to 1366x768 with Weston's screenshots on, and writes nothing under
/data/crysis. Each later command first writes the settings of Linux's runs
on the Lenovo (dev-log/80-games/80-2.md): Low, 1366x768, fullscreen, no
vsync, sound on. A file the script writes or removes under /data/crysis is
replaced or removed as a name, so that a symbolic link to a file of the
owner's goes, never the file. Each later command writes under OUT/NAME and
prints one result line, `crysis: result: NAME: ok` or
`crysis: result: NAME: FAILED` with what failed. With CRYSIS_HOME set, the
desktop account's commands run with HOME and the XDG data, configuration
and cache directories there, reading X's and PulseAudio's keys from its
home, as the native case runs them, in which the home is read-only.

`flyby` runs Linux's island flyby, `-devmode +map island +exec benchmark_gpu`,
and ends the game after LOOPS of its four loops; each loop's rate is judged
against GATE frames a second (24 unless CRYSIS_GATE says), beside Linux's,
but on the 32-bit game, which need only complete its loops. Each loop's
frame figures are of its 2000 frames in the HUD's dump, counted from the
load's last frame, which is loop 0's first and which `load:` reports apart.
A run whose load mark cannot fix that frame is uncut, and an uncut run, or
one whose dump does not hold a loop whole, fails (loop_windows); the frame
figures themselves carry no gate. With CRYSIS_LOAD_TRACE=N it samples every
thread from the HUD's Nth frame across the long frame that ends the level's
load, in a child process killed at 30 s, into STATE/NAME-load, and fails
unless the sampler ended by itself and the long frame came (LoadTrace).
`hud` loads a copy of SAVE, a checkpoint before the suit's reboot on the
first level, made under a name of its own so that the player's save is only
read, enters the reboot's trigger at the console and reads the HUD back 35 s
later from a screenshot, the dialog's line times and a save's GameRules. `save`
starts the first level, saves as accept_NAME once its opening cinematic, in
which the game refuses a save, has played, and records the save's SHA-256
in /data/crysis/accept.json; `load` loads SAVE through PREFIX, the game's own
prefix unless given, and requires the level loaded and the save's bytes as
recorded; it loads no save that `save` did not make.
"""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

import wm

BASE = Path("/data/crysis")
GAME = BASE / "game"
SAVES = BASE / "saves"
SAVEGAMES = SAVES / "SaveGames"
BENCH = GAME / "Game/Levels/island/benchmark_gpu.log"
RECORD = BASE / "accept.json"
STATE = Path(os.environ.get("CRYSIS_STATE", "/run/crysis-accept"))
HUD_PARENT = os.environ.get("CRYSIS_HUD_PARENT")
# CRYSIS_LOAD_TRACE=N has a flyby sample every thread across the frames that
# end the level's load, from the HUD's Nth frame (LoadTrace).
LOAD_TRACE = os.environ.get("CRYSIS_LOAD_TRACE")
# Rach2's profiling kernel's counts of system calls, page faults and sleeps.
BKL_CALLS = Path("/sys/kernel/debug/bkl/calls")
UID = 1001
HOME = os.environ.get("CRYSIS_HOME")
ENV = ("export XDG_RUNTIME_DIR=/run/user/1001 DISPLAY=:0 "
       "PULSE_SERVER=unix:/run/user/1001/pulse/native PROTON_USE_XALIA=0 "
       "MESA_SHADER_CACHE_SHOW_STATS=true "
       "WINEDEBUG=+timestamp,+pid,+loaddll,+d3d_caps,fixme-all; "
       + ("export HOME={0} XDG_DATA_HOME={0}/.local/share XDG_CONFIG_HOME={0}/.config "
          "XDG_CACHE_HOME={0}/.cache XAUTHORITY=/home/desktop/.Xauthority "
          "PULSE_COOKIE=/home/desktop/.config/pulse/cookie; ".format(HOME) if HOME else "")
       + "unset DBUS_SESSION_BUS_ADDRESS; ")
ROUTES = {"direct": [], "umu": ["--umu"], "32": ["--32"]}
# Linux's flyby on the same Lenovo, frames a second per loop, under cage at
# Low 1366x768 with sound on (dev-log/80-games/80-2.md, Crysis).
LINUX = {"direct": (45.17, 46.54, 45.97, 45.92), "umu": (45.02, 46.49, 45.78, 45.66)}
GATE = float(os.environ.get("CRYSIS_GATE", "24"))
# A run that writes nothing to its logs for this long has stopped.
SILENCE = 600
FAILED = []
# The games started and not yet ended, which the script's exit ends.
RUNNING = []


class Held:
    """Defer SIGTERM, SIGINT and SIGHUP to the end of the outermost `with
    HELD` block, so that the game's start is recorded and its end runs whole
    before such a signal ends the script. Once the script has begun to end,
    by the first such signal or by the end of main, every later one is only
    kept, for the exit status, so that no signal cuts the cleanup short."""
    depth = 0
    signum = None
    ending = False

    def __enter__(self):
        Held.depth += 1

    def __exit__(self, *_):
        Held.depth -= 1
        if Held.depth == 0 and Held.signum is not None and not Held.ending:
            signum, Held.signum = Held.signum, None
            Held.ending = True
            sys.exit(128 + signum)
        return False


HELD = Held()


def terminate(signum, _frame):
    """Exit with 128 + SIGNUM through the script's cleanup, or, inside a
    `with HELD` block, at its end; once the script is ending, keep SIGNUM."""
    if Held.depth or Held.ending:
        Held.signum = Held.signum or signum
        return
    Held.ending = True
    sys.exit(128 + signum)


def end_all():
    """End every game still in RUNNING; no signal interrupts it once it has
    set Held.ending."""
    Held.ending = True
    while RUNNING:
        RUNNING[-1].end()


def say(text):
    """Print one line of the script's output."""
    print("crysis: " + text, flush=True)


def check(name, ok, detail):
    """Print one check's line and remember a failed one."""
    say("check: %s: %s %s" % (name, "ok" if ok else "FAILED", detail))
    if not ok:
        FAILED.append(name)


def desktop(line):
    """Form LINE as a command of the desktop account in the desktop session."""
    return ["su", "-s", "/bin/bash", "desktop", "-c", ENV + line]


def out_dir(name):
    """Return OUT/NAME, made new and owned by the desktop account, OUT
    being the directory `prepare` recorded."""
    out = Path((STATE / "out").read_text().strip()) / name
    out.mkdir(parents=True)
    os.chown(out, UID, UID)
    return out


def volume():
    """Return the mixer's volume line, or None without a mixer."""
    if not Path("/dev/mixer").exists():
        return None
    return subprocess.run(["mixer", "vol"], stdout=subprocess.PIPE, text=True,
                          check=False).stdout.strip()


def set_volume():
    """Set the volume to 1, as the machine skill asks after a program that
    may set it has started."""
    if Path("/dev/mixer").exists():
        subprocess.run(["mixer", "vol", "1"], check=False, stdout=subprocess.DEVNULL)


def replace(path, text):
    """Write TEXT as a new file PATH owned by the desktop account, in place
    of whatever PATH was, a symbolic link to a read-only file among them."""
    if path.is_symlink() or path.exists():
        path.unlink()
    path.write_text(text)
    os.chown(path, UID, UID)


def settings():
    """Write Low, 1366x768, fullscreen, no vsync and sound on, in the game's
    system.cfg and the profile's files, which override it."""
    replace(GAME / "system.cfg", "sys_spec = 1\nr_Width = 1366\nr_Height = 768\n"
            "r_Fullscreen = 1\nr_vsync = 0\ns_SoundEnable = 1\n")
    cfg = SAVES / "game.cfg"
    if cfg.exists():
        text = cfg.read_text(errors="replace")
        for pattern, value in ((r"^(sys_spec_[A-Za-z]+) = .*$", r"\1 = 1"),
                               (r"^r_Width = .*$", "r_Width = 1366"),
                               (r"^r_Height = .*$", "r_Height = 768"),
                               (r"^r_Fullscreen = .*$", "r_Fullscreen = 1"),
                               (r"^r_vsync = .*$", "r_vsync = 0"),
                               (r"^s_SoundEnable = .*$", "s_SoundEnable = 1")):
            text = re.sub(pattern, value, text, flags=re.M)
        replace(cfg, text)
    attributes = SAVES / "Profiles/default/attributes.xml"
    if attributes.exists():
        replace(attributes, re.sub(r'(name="OptionCfg\.sys_spec_[A-Za-z]+" value=")[0-9]+',
                                   r"\g<1>1", attributes.read_text(errors="replace")))


def prepare(out, shm=False):
    """Set the session's output to 1366x768; record OUT for later commands. With SHM, check that the restarted
    session serves virgl to X clients and leave Weston's flags alone."""
    if not (GAME / "Bin64/Crysis.exe").exists():
        raise SystemExit("crysis: no game in %s" % GAME)
    STATE.mkdir(exist_ok=True)
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    os.chown(out, UID, UID)
    (STATE / "out").write_text(str(out) + "\n")
    output = wm.session("1366x768", debug=not shm)
    if shm:
        probe = subprocess.run(desktop("exec glxinfo -B"), stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, timeout=30)
        print(probe.stdout, end="", flush=True)
        if probe.returncode or not re.search(r"^OpenGL renderer string: virgl .*Intel",
                                             probe.stdout, re.M):
            raise SystemExit("crysis: Xwayland did not give the game virgl on Intel")
    text = wm.log_from(0)
    renderer = re.findall(r"GL renderer: (.*)", text)
    say("session: output %s, renderer %s" % (output, renderer[-1] if renderer else None))
    set_volume()
    say("volume: %s" % volume())
    print("CRYSIS PREPARED", flush=True)


class Game:
    """One start of the game as the desktop account, with its logs in OUT."""

    def __init__(self, out, route, args, prefix=None):
        self.out = out
        self.prefix = prefix or str(BASE / "compat")
        self.hud = Path(HUD_PARENT) / out.name if HUD_PARENT else out / "hud"
        self.hud.mkdir(parents=True)
        os.chown(self.hud, UID, UID)
        command = ["crysis.sh", "run"] + ROUTES[route]
        if prefix:
            command += ["--prefix", prefix]
        command += ["-devmode"] + args
        line = ("GALLIUM_HUD=frametime GALLIUM_HUD_VISIBLE=false GALLIUM_HUD_PERIOD=0 "
                "GALLIUM_HUD_DUMP_DIR=%s exec %s" % (self.hud, " ".join(
                    "'%s'" % word for word in command)))
        (out / "command.txt").write_text(line + "\n")
        # The game writes Game.log anew on each start; the last run's would
        # report its load before this one's.
        if (GAME / "Game.log").is_symlink() or (GAME / "Game.log").exists():
            (GAME / "Game.log").unlink()
        self.log = open(out / "game.out", "w")
        self.started = time.monotonic()
        # The game leads a session of its own, which no signal to the
        # script reaches; it must be in RUNNING before the script can exit.
        with HELD:
            self.proc = subprocess.Popen(desktop(line), stdout=self.log,
                                         stderr=subprocess.STDOUT,
                                         stdin=subprocess.DEVNULL, start_new_session=True)
            RUNNING.append(self)
        self.volume = volume()
        self.reset = False
        self.sizes = None
        self.still = time.monotonic()
        self.unseen = None  # the last look at Game.log that found no load
        say("started: %s" % " ".join(command))

    def tick(self):
        """Log a change of the volume, set it to 1 once 20 s into the game,
        and return whether the game's logs grew since the last tick."""
        if not self.reset and time.monotonic() - self.started > 20:
            set_volume()
            self.reset = True
        now = volume()
        if now != self.volume:
            with open(self.out / "volume.txt", "a") as f:
                f.write("%s %s\n" % (time.strftime("%T"), now))
            self.volume = now
        sizes = []
        for path in (self.out / "game.out", GAME / "Game.log", BENCH, self.hud / "frametime_(ms)"):
            try:
                sizes.append(path.stat().st_size)
            except OSError:
                sizes.append(-1)
        if sizes != self.sizes:
            self.sizes = sizes
            self.still = time.monotonic()
        return time.monotonic() - self.still < SILENCE

    def wait(self, ready, what):
        """Tick until READY() holds, the game ends or its logs stop growing;
        return whether READY() held."""
        while True:
            if ready():
                return True
            if self.proc.poll() is not None:
                say("%s: the game ended first, status %d" % (what, self.proc.returncode))
                return False
            if not self.tick():
                say("%s: no log grew for %d s" % (what, SILENCE))
                return False
            time.sleep(0.5)

    def frames(self):
        """Return the number of frames the HUD's dump holds."""
        try:
            with open(self.hud / "frametime_(ms)") as f:
                return sum(1 for line in f if line.split())
        except OSError:
            return 0

    def loaded(self):
        """Return the level's loading time from Game.log, or None, and keep
        in self.unseen the monotonic time a look that finds none began."""
        at = time.monotonic()
        try:
            text = (GAME / "Game.log").read_text(errors="replace")
        except OSError:
            text = ""
        found = re.search(r"\*LOADING: Level (\S+) loading time: ([\d.]+) seconds", text)
        if not found:
            self.unseen = at
        return found.group(2) if found else None

    def load_mark(self):
        """Return the load mark once Game.log reports the level loaded, or
        None: (COUNT, GAP), the HUD's frame count read after the log, and
        the seconds from the last look that found no load to that count's
        reading, None if no look found none (loop_windows)."""
        if self.loaded() is None:
            return None
        unseen, count = self.unseen, self.frames()
        return count, (time.monotonic() - unseen) if unseen is not None else None

    def keys(self, *actions):
        """Send ACTIONS through XTEST as the desktop account: a key name, or
        a line of text, which `--text` types and ends with Return."""
        for action in actions:
            if isinstance(action, (int, float)):
                time.sleep(action)
                continue
            argv = ["--text", action[1:]] if action.startswith("=") else [action]
            run = subprocess.run(desktop("exec python3 %s key %s" % (
                Path(__file__).resolve(), " ".join("'%s'" % a for a in argv))),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
            with open(self.out / "keys.txt", "a") as f:
                f.write(run.stdout)

    def console(self, *lines):
        """Open the game's console, type LINES and close it."""
        self.keys("grave", 1, *["=" + line for line in lines])
        self.keys(0.5, "grave", 0.5)

    def shot(self, name):
        """Capture the output with weston-screenshooter as OUT/NAME.png;
        return whether it was written."""
        shots = self.out / "shots"
        shots.mkdir(exist_ok=True)
        os.chown(shots, UID, UID)
        before = set(shots.glob("*.png"))
        subprocess.run(desktop("XDG_PICTURES_DIR=%s exec weston-screenshooter" % shots),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        new = sorted(set(shots.glob("*.png")) - before)
        if new:
            new[-1].rename(self.out / (name + ".png"))
        say("shot: %s %s" % (name, "written" if new else "missing"))
        return bool(new)

    def prompt(self):
        """Return whether the loading screen's "PRESS ANY KEY TO CONTINUE"
        shows: white text across the middle of its box, at 1366x768 a band
        from (560, 680) to (805, 696), which the level leaves dark."""
        shots = self.out / "prompt"
        shots.mkdir(exist_ok=True)
        os.chown(shots, UID, UID)
        for old in shots.glob("*.png"):
            old.unlink()
        subprocess.run(desktop("XDG_PICTURES_DIR=%s exec weston-screenshooter" % shots),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        found = sorted(shots.glob("*.png"))
        if not found:
            return False
        width, height, bpp, rows = wm.png_rows(found[-1], 696)
        if (width, height) != (1366, 768):
            return False
        white = 0
        for y in range(680, 697):
            row = rows[y]
            for x in range(560, 806):
                r, g, b = row[x * bpp:x * bpp + 3]
                white += r > 200 and g > 200 and b > 200
        return white > 150

    def go_on(self):
        """Pass the loading screen's prompt with Return once it shows, and
        return whether it showed and went; the prompt takes no key before it
        shows, nor the console a line."""
        seen = False
        for _ in range(60):
            if self.proc.poll() is not None:
                break
            if self.prompt():
                seen = True
                self.keys("Return", 2)
            elif seen:
                say("prompt: passed")
                return True
            else:
                time.sleep(2)
        say("prompt: %s" % ("still shown" if seen else "never shown"))
        return False

    def end(self):
        """End the game, its Wine server and, on umu, its container, then
        keep its logs and the kernel's lines of missing calls."""
        with HELD:
            if self.proc.poll() is None:
                subprocess.run(desktop("WINEPREFIX=%s/pfx /opt/proton/files/bin/wineserver -k" %
                                       self.prefix),
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
                for _ in range(16):
                    if self.proc.poll() is not None:
                        break
                    time.sleep(0.5)
            for pattern in ("Crysis.exe", "wineserver|pressure-vessel|pv-adverb|srt-bwrap"):
                subprocess.run(["pkill", "-9", "-u", "desktop", "-f", pattern], check=False)
            if self.proc.poll() is None:
                os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait()
            if self in RUNNING:
                RUNNING.remove(self)
            self.log.close()
            if HUD_PARENT:
                shutil.copytree(self.hud, self.out / "hud")
            for path in (BENCH, GAME / "Game.log", BASE / "logs/steam-17300.log"):
                if path.exists():
                    (self.out / path.name).write_bytes(path.read_bytes())
            dmesg = subprocess.run(["dmesg"], stdout=subprocess.PIPE, text=True, check=False).stdout
            missing = sorted(set(re.findall(r"syscall: .*not implemented.*", dmesg)))
            (self.out / "missing.txt").write_text("".join(line + "\n" for line in missing))
            for line in missing:
                say("enosys: %s" % line)
            say("ended after %.0f s, status %s" % (time.monotonic() - self.started,
                                                   self.proc.returncode))

    def renderer(self):
        """Check WineD3D's GL renderer and fsync in the launch's output."""
        text = (self.out / "game.out").read_text(errors="replace")
        proton = self.out / "steam-17300.log"
        if proton.exists():
            text += proton.read_text(errors="replace")
        check("wined3d", "Using the OpenGL renderer" in text, "WineD3D's OpenGL renderer")
        check("fsync", "fsync: up and running" in text, "Wine's fsync")


def finished():
    """Return the benchmark log's loops so far, each as (fps, play time)."""
    try:
        text = BENCH.read_text(errors="replace")
    except OSError:
        return []
    return [(float(fps), float(play)) for play, fps in re.findall(
        r"Run \d+ Finished\.\s+Play Time: ([\d.]+)s, Average FPS: ([\d.]+)", text)]


# The frames of one loop of the flyby, as the game's benchmark log counts them.
LOOP_FRAMES = 2000
# The shortest frame, in ms, that can be the one that ends the level's load.
LOAD_FRAME = 1000
# The longest a load mark may come after the last look at Game.log that found
# no load, in seconds, for the load's last frame to be found from the mark;
# the load's wait and its trace look every 0.5 s or less.
LOAD_LAG = 2.0


def loop_windows(dump, mark, loops):
    """Cut DUMP, the HUD's frame times, into LOOPS loops after the level's
    load; return (L, (A, B), windows, why).

    MARK is game.load_mark()'s (COUNT, GAP). The game logs the load's end
    after the last look that found no load and before COUNT was read, and
    the HUD writes each frame's line as it is presented. So the load ends in
    a frame that ends less than GAP before frame COUNT - 1 does, or in the
    one that ended last before those, or in frame COUNT, then being drawn:
    frames A to B, read from the whole dump after the run. L, the load's
    last frame, is the one of them of LOAD_FRAME ms or more, about 1.5 s on
    Rach2. The game counts L as loop 0's first frame, and loop k as the
    LOOP_FRAMES frames from L + k * LOOP_FRAMES; each later loop begins
    with a slower frame at that place in every run read. windows[k] is
    (start, end) of loop k's figures, which leave L out of loop 0; a window
    whose end passes the dump's is not whole. WHY says why a run is uncut,
    with L None and no windows: no mark, a mark more than LOAD_LAG after the
    last look that found no load, a dump that ends before the mark, or no
    frame, or more than one, of LOAD_FRAME ms or more in A to B.
    tools/lenovo/debian-ref/crysis-frames.py cuts Linux's dump by this
    rule."""
    if mark is None:
        return None, None, [], "no load mark"
    count, gap = mark
    if gap is None or gap > LOAD_LAG:
        return None, None, [], "the load mark came %s after the last look that found no load, " \
            "past %.1f s" % ("an unknown time" if gap is None else "%.2f s" % gap, LOAD_LAG)
    if count >= len(dump):
        return None, None, [], "the dump's %d frames end at the load mark, frame %d" % (
            len(dump), count)
    first, ago = count - 1, 0.0
    while first > 0 and ago < gap * 1000:
        ago += dump[first]
        first -= 1
    first = max(first, 0)
    long = [i for i in range(first, count + 1) if dump[i] >= LOAD_FRAME]
    if len(long) != 1:
        return None, (first, count), [], "%s of %d ms or more among frames %d to %d" % (
            "frames " + ", ".join("%d" % i for i in long) if long else "no frame",
            LOAD_FRAME, first, count)
    last = long[0]
    windows = [(last + (1 if k == 0 else k * LOOP_FRAMES), last + (k + 1) * LOOP_FRAMES)
               for k in range(loops)]
    return last, (first, count), windows, None


def frame_stats(values):
    """Return the median, the 99th percentile and the worst of VALUES, in ms,
    and how many pass 50 and 100 ms."""
    s = sorted(values)
    at = lambda q: s[min(len(s) - 1, int(q * len(s)))]
    return "median %.1f p99 %.1f max %.1f >50ms %d >100ms %d" % (
        at(0.5), at(0.99), s[-1], sum(1 for v in s if v > 50), sum(1 for v in s if v > 100))


def state_dir(name):
    """Return STATE/NAME, made new. STATE is root's, and its parent is too
    where a profile is taken, so the snapshots root writes there go through
    no name the desktop account chose, as one in OUT, which that account
    owns, could."""
    counts = STATE / name
    if counts.is_dir() and not counts.is_symlink():
        shutil.rmtree(counts)
    elif counts.exists() or counts.is_symlink():
        counts.unlink()
    counts.mkdir(mode=0o755)
    return counts


def profile_dir(name):
    """Return STATE/NAME-profile, made new, when BED_PROFILE names the reader,
    or None."""
    return state_dir(name + "-profile") if os.environ.get("BED_PROFILE") else None


def thread_rows():
    """Return one line a thread: its ID, its process's, its state, its user
    and system ticks, its wait channel, its system call's number and its
    name, read from /proc/PID/task/TID; a thread that ends meanwhile is left
    out."""
    rows = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            tids = os.listdir("/proc/%s/task" % pid)
        except OSError:
            continue
        for tid in tids:
            task = "/proc/%s/task/%s/" % (pid, tid)
            try:
                with open(task + "stat") as f:
                    stat = f.read()
                with open(task + "wchan") as f:
                    wchan = f.read().strip() or "0"
                with open(task + "syscall") as f:
                    call = (f.read().split() or ["-"])[0]
            except OSError:
                continue
            fields = stat[stat.rfind(")") + 2:].split()
            rows.append("%s %s %s %s %s %s %s %s" % (
                tid, pid, fields[0], fields[11], fields[12], wchan, call,
                stat[stat.find("(") + 1:stat.rfind(")")]))
    return rows


def hud_times(path):
    """Return the frame times of the HUD's dump at PATH so far, in ms."""
    try:
        with open(path) as f:
            return [float(line.split()[-1]) for line in f if line.split()]
    except OSError:
        return []


def stop_after(child, seconds):
    """Wait SECONDS for CHILD, a Popen, and return its exit status, or kill
    it with SIGKILL and return None. A read of /proc/PID waits on that
    process's memory map, a wait no signal ends, for as long as another
    thread holds it; the kill ends the child's work as that read returns,
    and the child, never reaped here, holds no exit of the script's."""
    try:
        return child.wait(seconds)
    except subprocess.TimeoutExpired:
        child.kill()
        return None


# The longest a load trace samples, and the longest it waits for the HUD's
# FIRST frame once the level is loaded, in seconds.
TRACE_SPAN = 30
# The longest the trace takes to copy its counts.
TRACE_END = 10


def trace_sample(where, hud, first):
    """Sample every thread into WHERE/samples, as LoadTrace's child: from
    the moment the HUD's dump HUD holds FIRST frames until it holds the
    first frame of LOAD_FRAME ms or more from FIRST on and ten after it, or
    TRACE_SPAN seconds have passed. It first zeroes the profiling kernel's
    counts and, if it can, writes WHERE/zeroed. Each sample is the line
    `sample NS BEFORE AFTER`, the monotonic time and the dump's frame counts
    before and after the walk, then thread_rows(), then `end`, flushed
    whole; a sample without its `end` was cut short. Return 0; an error
    ends the child with Python's status 1."""
    try:
        BKL_CALLS.write_text("all\n")  # any word but `thread` counts every thread
        (where / "zeroed").write_text("")
    except OSError:
        pass
    begun = time.monotonic()
    with open(where / "samples", "w") as f:
        while time.monotonic() - begun < TRACE_SPAN:
            times = hud_times(hud)
            if any(times[i] >= LOAD_FRAME for i in range(first, len(times) - 10)):
                break
            ns, rows = time.monotonic_ns(), thread_rows()
            f.write("sample %d %d %d\n" % (ns, len(times), len(hud_times(hud))))
            f.writelines(row + "\n" for row in rows)
            f.write("end\n")
            f.flush()
            time.sleep(0.05)
    return 0


def read_samples(path):
    """Return the whole samples of a `samples` file as (ns, before, after,
    rows), leaving out one that its `end` line does not close."""
    samples, head, rows = [], None, []
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError:
        return samples
    for line in lines:
        if line.startswith("sample "):
            head, rows = [int(v) for v in line.split()[1:4]], []
        elif line == "end":
            if head is not None:
                samples.append((head[0], head[1], head[2], rows))
            head = None
        elif head is not None:
            rows.append(line)
    return samples


class LoadTrace:
    """Sample every thread across the frames that end the level's load, the
    longest of which has taken 1.5 s on Rach2, every 50 ms from the HUD's
    FIRST frame until a frame of LOAD_FRAME ms or more and ten frames after
    it are in the dump. The samples are taken by a child process,
    trace_sample, which the trace kills at TRACE_SPAN seconds, however long
    one read takes, and the trace takes the load mark meanwhile. WHERE gets
    the child's `samples`; `calls`, the profiling kernel's bkl/calls over
    the trace, for every thread, when it has one, copied by a cp killed at
    TRACE_END seconds; and `summary`, which the trace writes from the whole
    samples in `samples`: the long frame by its 1-based line in the dump and
    the states, wait channels and system calls of each thread of the game's
    processes and wineserver, and of any other thread running or in a disk
    wait, in the samples whose walk lay wholly inside that frame. The check
    `load-trace` fails unless the child ended by itself with status 0 and
    the dump holds the long frame."""

    def __init__(self, game, where, first):
        self.game, self.where, self.first = game, where, first
        self.done = False
        self.mark = None  # game.load_mark(), if it came in the trace
        self.loaded_at = None  # when step() first saw the level loaded

    def step(self):
        """Run the trace once the HUD holds FIRST frames, or give it up when
        the HUD has not reached them TRACE_SPAN seconds after the level was
        seen loaded; return whether it has run or been given up."""
        if self.done:
            return True
        if self.game.frames() >= self.first:
            self.run()
            self.done = True
        elif self.game.loaded() is not None:
            if self.loaded_at is None:
                self.loaded_at = time.monotonic()
            elif time.monotonic() - self.loaded_at > TRACE_SPAN:
                text = "the HUD held no frame %d %d s after the load" % (self.first, TRACE_SPAN)
                (self.where / "summary").write_text(text + "\n")
                check("load-trace", False, text)
                self.done = True
        return self.done

    def run(self):
        """Run the sampling child for TRACE_SPAN seconds at most, taking the
        load mark meanwhile, then copy the counts and write the summary."""
        child = subprocess.Popen(
            [sys.executable, os.path.abspath(__file__), "trace-sample", str(self.where),
             str(self.game.hud / "frametime_(ms)"), str(self.first)],
            stdin=subprocess.DEVNULL)
        status = None
        try:
            end = time.monotonic() + TRACE_SPAN
            while status is None and time.monotonic() < end:
                if self.mark is None:
                    self.mark = self.game.load_mark()
                try:
                    status = child.wait(0.1)
                except subprocess.TimeoutExpired:
                    pass
        finally:
            if status is None:
                status = stop_after(child, 0)
        copied = None
        if (self.where / "zeroed").exists():
            copied = stop_after(subprocess.Popen(
                ["cp", str(BKL_CALLS), str(self.where / "calls")],
                stdin=subprocess.DEVNULL), TRACE_END)
        samples = read_samples(self.where / "samples")
        times = self.game_times()
        long_at = next((i for i in range(self.first, len(times)) if times[i] >= LOAD_FRAME),
                       None)
        line = self.summary(samples, times, long_at)
        how = ("the sampler killed at %d s" % TRACE_SPAN if status is None else
               "the sampler's status %d" % status)
        if copied is None and (self.where / "zeroed").exists():
            how += ", its counts not copied in %d s" % TRACE_END
        check("load-trace", status == 0 and long_at is not None,
              "%s; %s, %d whole samples" % (line, how, len(samples)))

    def game_times(self):
        """Return the HUD's frame times so far."""
        return hud_times(self.game.hud / "frametime_(ms)")

    def summary(self, samples, times, long_at):
        """Write the summary of what the game's threads did in the long
        frame: the samples whose walk began and ended while the HUD held
        long_at frames, that frame not yet presented. The frame is named by
        its line in the dump, counted from 1. Return its first line."""
        if long_at is None:
            text = "no frame of %d ms or more in %d samples to the dump's line %d\n" % (
                LOAD_FRAME, len(samples), len(times))
        else:
            during = [rows for ns, before, after, rows in samples
                      if before == long_at and after == long_at]
            states, ticks = {}, {}
            for rows in during:
                split = [row.split(" ", 7) for row in rows]
                game = {r[1] for r in split if r[0] == r[1] and (
                    "Crysis" in r[7] or "wineserver" in r[7] or r[7].endswith(".exe"))}
                for tid, pid, state, user, system, wchan, call, name in split:
                    if pid not in game and state not in ("R", "D"):
                        continue
                    key = "%s %s %s %s %s" % (tid, name.replace(" ", "_"), state, wchan, call)
                    states[key] = states.get(key, 0) + 1
                    ticks.setdefault(tid, []).append(int(user) + int(system))
            text = "frame %d of %.1f ms, %d samples in it\n" % (
                long_at + 1, times[long_at], len(during))
            for key, n in sorted(states.items(), key=lambda kv: -kv[1])[:40]:
                tid = key.split()[0]
                text += "%3d %s ticks %d\n" % (n, key, ticks[tid][-1] - ticks[tid][0])
        (self.where / "summary").write_text(text)
        return text.splitlines()[0]


def flyby_profile(game, counts, phase):
    """Read counters into COUNTS at a flyby boundary: PHASE is before or
    after, or stop, which ends the VM's profiler that before started. The
    stop runs in a session of its own, so that a signal to the terminal's
    process group does not end it."""
    reader = os.environ["BED_PROFILE"]
    stop = phase == "stop"
    if not stop:
        mark = counts / ("mark-" + phase)
        mark.write_text("%d %d\n" % (time.monotonic_ns(), game.frames()))
    subprocess.run(["sh", reader, counts, phase], check=not stop, start_new_session=stop)


def flyby(route, name, loops):
    """Run the flyby for LOOPS loops and judge each against GATE."""
    out = out_dir(name)
    if BENCH.is_symlink() or BENCH.exists():
        BENCH.unlink()
    settings()
    counts = profile_dir(name)
    game = Game(out, route, ["+map", "island", "+exec", "benchmark_gpu"])
    trace = LoadTrace(game, state_dir(name + "-load"), int(LOAD_TRACE)) if LOAD_TRACE else None
    ends, mark = [], []
    profiling = False

    def loaded():
        """Record the load mark at the first sight of the level loaded, by
        the load's trace while it runs, and hold the wait for the trace."""
        traced = trace is None or trace.step()
        if not mark:
            found = trace.mark if trace is not None and trace.mark is not None else None
            found = found or game.load_mark()
            if found:
                mark.append(found)
        return bool(mark) and traced

    try:
        game.wait(loaded, "load")
        say("loaded in %s s" % game.loaded())
        if counts:
            profiling = True
            flyby_profile(game, counts, "before")
        while len(ends) < loops:
            done = len(finished())
            if done > len(ends):
                ends.append(game.frames())
                if len(ends) == loops and counts:
                    flyby_profile(game, counts, "after")
                    profiling = False
                say("loop %d finished at frame %d" % (done - 1, ends[-1]))
                continue
            if not game.wait(lambda: len(finished()) > len(ends), "loop %d" % len(ends)):
                break
    finally:
        # A signal here waits for the profiler's stop and the game's end.
        with HELD:
            if profiling:
                flyby_profile(game, counts, "stop")
            game.end()
    runs = finished()[:loops]
    dump = []
    try:
        dump = [float(line.split()[-1]) for line in
                open(game.hud / "frametime_(ms)") if line.split()]
    except OSError:
        pass
    linux = LINUX.get(route, ())
    # The plan asks the 32-bit game only to complete its loop.
    gate = 0 if route == "32" else GATE
    last, span, windows, why = loop_windows(dump, mark[0] if mark else None, len(runs))
    if last is None:
        say("load: uncut: %s" % why)
    else:
        say("load: last frame %d of %.1f ms, counted from 0, the one of %d ms or more in frames "
            "%d to %d, "
            "load mark at frame %d %.2f s after the last look that found no load; "
            "loop k from frame %d + %d k, frame %d left out of loop 0" % (
                last, dump[last], LOAD_FRAME, span[0], span[1], mark[0][0], mark[0][1], last,
                LOOP_FRAMES, last))
    for i, (fps, play) in enumerate(runs):
        start, end = windows[i] if windows else (0, 0)
        whole = bool(windows) and end <= len(dump)
        segment = dump[start:end] if whole else []
        beside = (" linux %.2f" % linux[i]) if i < len(linux) else ""
        check("loop%d" % i, fps >= gate, "%.2f fps (gate %.0f)%s, play %.2f s%s" % (
            fps, gate, beside, play, (", " + frame_stats(segment)) if segment else ""))
        check("loop%d-frames" % i, whole, ("frames %d to %d" % (start, end - 1)) if whole
              else ("uncut: " + why) if why else
              "frames %d to %d, past the dump's %d" % (start, end - 1, len(dump)))
    check("loops", len(runs) == loops, "%d of %d loops finished" % (len(runs), loops))
    game.renderer()


def load_save(game, save, what):
    """Wait for the level of a `+load SAVE` start, then pass its prompt."""
    ok = game.wait(lambda: game.loaded() is not None, what)
    check(what, ok, "%s: level loaded in %s s" % (save, game.loaded()))
    if ok:
        ok = game.go_on()
        check("prompt", ok, "the loading screen's prompt passed")
    return ok


def source_copy(out, name, save):
    """Copy SAVE, a name in SaveGames, and its XML beside it as
    default_accept_src_NAME, and into OUT, and return the copy's name, so
    that no run loads or overwrites a save of the player's own."""
    stem = save[:-len(".CRYSISJMSF")] if save.endswith(".CRYSISJMSF") else save
    copy = "default_accept_src_%s" % name
    for ext in (".CRYSISJMSF", ".xml"):
        data = (SAVEGAMES / (stem + ext)).read_bytes()
        for path in (SAVEGAMES / (copy + ext), out / (stem + ext)):
            path.write_bytes(data)
            os.chown(path, UID, UID)
    say("save: %s copied as %s, sha256 %s" % (save, copy + ".CRYSISJMSF", hashlib.sha256(
        (SAVEGAMES / (copy + ".CRYSISJMSF")).read_bytes()).hexdigest()))
    return copy + ".CRYSISJMSF"


def hud(name, save, route):
    """The suit's reboot from SAVE, read back 35 s after its trigger."""
    out = out_dir(name)
    settings()
    tag = "hud_" + name
    for old in SAVEGAMES.glob("default_%s.*" % tag):
        old.unlink()
    copy = source_copy(out, name, save)
    game = Game(out, route, ["+load", copy])
    try:
        if load_save(game, copy, "load"):
            game.shot("loaded")
            # Set after the load, which a verbose log slows sixfold; each line
            # alone and until its echo shows, since one typed beside others
            # was once lost, and the dialog's lines are what `lines` reads.
            for cvar in ("log_Verbosity", "ds_LogLevel"):
                echoed = False
                for attempt in range(3):
                    game.console("%s 4" % cvar)
                    log = GAME / "Game.log"
                    text = log.read_text(errors="replace") if log.exists() else ""
                    echoed = re.search(r"\$3%s = \$64 " % cvar, text) is not None
                    if echoed:
                        break
                say("console: %s 4 %s" % (cvar, "set" if echoed else "not echoed"))
            game.console("gotoe suit_menu_and_objectives")
            game.keys(35)
            check("after", game.shot("after"), "the HUD 35 s after the trigger, to read")
            game.console("sys_flash_info 1")
            game.shot("flash")
            game.console("sys_flash_info 0")
            # A save the game refuses, as in a cinematic, writes nothing; try
            # again a bounded number of times, as `save` does.
            for attempt in range(6):
                game.console("save " + tag)
                game.keys(10)
                if any(SAVEGAMES.glob("default_%s.*" % tag)):
                    break
            game.keys(4)
    finally:
        game.end()
        for path in SAVEGAMES.glob(copy.replace(".CRYSISJMSF", ".*")):
            path.unlink()
    text = (out / "Game.log").read_text(errors="replace") if (out / "Game.log").exists() else ""
    lines = [re.findall(r"SID=\d+ Playing new line %d:.*now=([\d.]+)" % n, text) for n in (1, 2)]
    xml = SAVEGAMES / ("default_%s.xml" % tag)
    rules = re.search(r'<Extension name="GameRules"[^>]*>', xml.read_text(errors="replace")) \
        if xml.exists() else None
    flags = re.findall(r'hud(?:Show|Broken)="\d"', rules.group(0)) if rules else []
    if xml.exists():
        (out / xml.name).write_bytes(xml.read_bytes())
    for path in SAVEGAMES.glob("default_%s.*" % tag):
        path.unlink()
    if lines[0] and lines[1]:
        show = (float(lines[1][-1]) + 4 - float(lines[0][-1]) - 6) * 1000
        say("dialog: line 1 at %s s, line 2 at %s s, Show after BootSeq %.0f ms" % (
            lines[0][-1], lines[1][-1], show))
    check("lines", bool(lines[0] and lines[1]), "the reboot's two dialog lines played")
    check("gamerules", 'hudShow="1"' in flags and not any("hudBroken" in f for f in flags),
          "GameRules %s" % (" ".join(flags) or "absent"))


def record():
    """Return the saves `save` recorded, by name."""
    return json.loads(RECORD.read_text()) if RECORD.exists() else {}


def save(name, route):
    """Start the first level, save as accept_NAME and record its SHA-256."""
    out = out_dir(name)
    settings()
    tag = "accept_" + name
    for old in SAVEGAMES.glob("default_%s.*" % tag):
        old.unlink()
    game = Game(out, route, ["+map", "island"])
    path = SAVEGAMES / ("default_%s.CRYSISJMSF" % tag)
    try:
        if load_save(game, "island", "load"):
            # The level opens with its cinematic, the date card and the night
            # jump, during which the game refuses a save; try again until
            # one is written.
            game.keys(90)
            for attempt in range(12):
                game.console("save " + tag)
                game.keys(10)
                if path.exists():
                    say("save: written at attempt %d" % (attempt + 1))
                    break
            game.keys(3)
            game.shot("saved")
    finally:
        game.end()
    check("saved", path.exists(), str(path))
    if path.exists():
        saves = record()
        saves[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
        RECORD.write_text(json.dumps(saves, indent=2) + "\n")
        os.chown(RECORD, UID, UID)
        say("save: %s sha256 %s, %d bytes" % (path.name, saves[path.name], path.stat().st_size))
        os.sync()


def load(name, save_name, route, prefix):
    """Load SAVE through PREFIX and require its bytes as `save` recorded."""
    out = out_dir(name)
    settings()
    path = SAVEGAMES / save_name
    want = record().get(save_name)
    have = hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None
    check("bytes", want is not None and have == want, "%s sha256 %s, recorded %s" % (
        save_name, have, want))
    if want is None:
        return
    made = prefix and not Path(prefix, "pfx/user.reg").exists()
    game = Game(out, route, ["+load", save_name], prefix)
    try:
        if load_save(game, save_name, "load"):
            game.keys(10)
            game.shot("loaded")
    finally:
        game.end()
    text = (out / "game.out").read_text(errors="replace")
    again = "rach2-game: prefix: made" in text
    check("prefix", again == bool(made), "%s, %s" % (
        prefix or "/data/crysis/compat", "made new" if again else "used as it was"))


def key(argv):
    """Press a key named by its keysym, or type a line and Return, through
    XTEST on $DISPLAY, which reaches the window that has the focus."""
    x = ctypes.CDLL("libX11.so.6")
    t = ctypes.CDLL("libXtst.so.6")
    x.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x.XOpenDisplay.restype = ctypes.c_void_p
    x.XStringToKeysym.argtypes = [ctypes.c_char_p]
    x.XStringToKeysym.restype = ctypes.c_ulong
    x.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x.XKeysymToKeycode.restype = ctypes.c_uint
    x.XkbKeycodeToKeysym.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_int]
    x.XkbKeycodeToKeysym.restype = ctypes.c_ulong
    x.XFlush.argtypes = [ctypes.c_void_p]
    t.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
    d = x.XOpenDisplay(os.environ["DISPLAY"].encode())
    if not d:
        raise SystemExit("crysis: key: cannot open the display")

    def press(code, shift=False):
        """Press and release the key code CODE, under Shift when SHIFT."""
        held = x.XKeysymToKeycode(d, x.XStringToKeysym(b"Shift_L")) if shift else 0
        if held:
            t.XTestFakeKeyEvent(d, held, 1, 0)
        t.XTestFakeKeyEvent(d, code, 1, 0)
        x.XFlush(d)
        time.sleep(0.05)
        t.XTestFakeKeyEvent(d, code, 0, 0)
        if held:
            t.XTestFakeKeyEvent(d, held, 0, 0)
        x.XFlush(d)
        time.sleep(0.08)

    if argv[0] == "--text":
        for ch in argv[1]:
            code = x.XKeysymToKeycode(d, ord(ch))
            if not code:
                raise SystemExit("crysis: key: no key for %r" % ch)
            press(code, x.XkbKeycodeToKeysym(d, code, 0, 0) != ord(ch))
        argv = ["Return"]
    code = x.XKeysymToKeycode(d, x.XStringToKeysym(argv[0].encode()))
    if not code:
        raise SystemExit("crysis: key: no key %s" % argv[0])
    press(code)
    print("crysis: key: %s" % argv[0], flush=True)


def main():
    """Run the command the arguments name; see the module's text."""
    argv = sys.argv[1:]
    if argv and argv[0] == "key":
        key(argv[1:])
        return 0
    if os.geteuid() != 0:
        raise SystemExit("crysis: run as root")
    usage = "usage: crysis.py prepare OUT [--shm] | flyby ROUTE NAME [LOOPS] | hud NAME SAVE [ROUTE] | " \
            "save NAME [ROUTE] | load NAME SAVE [ROUTE [PREFIX]]"
    if not argv:
        raise SystemExit(usage)
    command, rest = argv[0], argv[1:]
    if command == "prepare" and (len(rest) == 1 or (len(rest) == 2 and rest[1] == "--shm")):
        prepare(rest[0], len(rest) == 2)
        return 0
    if command == "flyby" and len(rest) in (2, 3) and rest[0] in ROUTES:
        name = rest[1]
        flyby(rest[0], name, int(rest[2]) if len(rest) == 3 else 4)
    elif command == "hud" and len(rest) in (2, 3):
        name = rest[0]
        hud(name, rest[1], rest[2] if len(rest) == 3 else "umu")
    elif command == "save" and len(rest) in (1, 2):
        name = rest[0]
        save(name, rest[1] if len(rest) == 2 else "umu")
    elif command == "load" and len(rest) in (2, 3, 4):
        name = rest[0]
        load(name, rest[1], rest[2] if len(rest) >= 3 else "umu",
             rest[3] if len(rest) == 4 else None)
    else:
        raise SystemExit(usage)
    if FAILED:
        say("result: %s: FAILED %s" % (name, ", ".join(FAILED)))
        return 1
    say("result: %s: ok" % name)
    return 0


if __name__ == "__main__":
    # LoadTrace's child, which keeps the default signals and ends no game.
    if sys.argv[1:2] == ["trace-sample"] and len(sys.argv) == 5:
        sys.exit(trace_sample(Path(sys.argv[2]), Path(sys.argv[3]), int(sys.argv[4])))
    for each in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(each, terminate)
    # A signal that comes as the inner cleanup begins, before end_all has
    # set Held.ending, raises out of it after setting Held.ending itself, so
    # the outer cleanup runs whole.
    try:
        try:
            sys.exit(main())
        finally:
            end_all()
    finally:
        end_all()
        if Held.signum is not None:
            sys.exit(128 + Held.signum)
