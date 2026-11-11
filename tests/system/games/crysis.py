#!/usr/bin/env python3
"""Accept Crysis on a trial root: the flyby, the suit's reboot and the saves.

Usage, as root, in the boot session of the GNU root with the game installed
in /data/crysis (crysis.sh):

    crysis.py prepare OUT
    crysis.py flyby ROUTE NAME [LOOPS]
    crysis.py hud NAME SAVE [ROUTE]
    crysis.py save NAME [ROUTE]
    crysis.py load NAME SAVE [ROUTE [PREFIX]]
    crysis.py restore

ROUTE is direct, umu or 32: crysis.sh run, run --umu or run --32. `prepare`
gives the desktop account the game's directory, recording its owner, sets
the boot session's output to 1366x768 with Weston's screenshots on, and
writes the settings of Linux's runs on the Lenovo (dev-log/80-games/80-2.md):
Low, 1366x768, fullscreen, no vsync, sound on. Each later command writes
under OUT/NAME and prints one result line, `crysis: result: NAME: ok` or
`crysis: result: NAME: FAILED` with what failed.

`flyby` runs Linux's island flyby, `-devmode +map island +exec benchmark_gpu`,
and ends the game after LOOPS of its four loops; each loop's rate is judged
against GATE frames a second (24 unless CRYSIS_GATE says), beside Linux's,
but on the 32-bit game, which need only complete its loops.
`hud` loads a copy of SAVE, a checkpoint before the suit's reboot on the
first level, made under a name of its own so that the player's save is only
read, enters the reboot's trigger at the console and reads the HUD back 35 s
later from a screenshot, the dialog's line times and a save's GameRules. `save`
starts the first level, saves as accept_NAME once its opening cinematic, in
which the game refuses a save, has played, and records the save's SHA-256
in /data/crysis/accept.json; `load` loads SAVE through PREFIX, the game's own
prefix unless given, and requires the level loaded and the save's bytes as
recorded; it loads no save that `save` did not make. `restore` gives the
game's directory back to its recorded owner.
"""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
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
OWNER = BASE / "accept-owner"
RECORD = BASE / "accept.json"
STATE = Path("/run/crysis-accept")
UID = 1001
ENV = ("export XDG_RUNTIME_DIR=/run/user/1001 DISPLAY=:0 "
       "PULSE_SERVER=unix:/run/user/1001/pulse/native PROTON_USE_XALIA=0 "
       "MESA_SHADER_CACHE_SHOW_STATS=true "
       "WINEDEBUG=+timestamp,+pid,+loaddll,+d3d_caps,fixme-all; "
       "unset DBUS_SESSION_BUS_ADDRESS; ")
ROUTES = {"direct": [], "umu": ["--umu"], "32": ["--32"]}
# Linux's flyby on the same Lenovo, frames a second per loop, under cage at
# Low 1366x768 with sound on (dev-log/80-games/80-2.md, Crysis).
LINUX = {"direct": (45.17, 46.54, 45.97, 45.92), "umu": (45.02, 46.49, 45.78, 45.66)}
GATE = float(os.environ.get("CRYSIS_GATE", "24"))
# A run that writes nothing to its logs for this long has stopped.
SILENCE = 600
FAILED = []


def say(text):
    """Print one line of the script's output."""
    print("crysis: " + text, flush=True)


def check(name, ok, detail):
    """Print one check's line and remember a failed one."""
    say("check: %s: %s %s" % (name, "ok" if ok else "FAILED", detail))
    if not ok:
        FAILED.append(name)


def desktop(line):
    """Form LINE as a command of the desktop account in the boot session."""
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


def settings():
    """Write Low, 1366x768, fullscreen, no vsync and sound on, in the game's
    system.cfg and the profile's files, which override it."""
    (GAME / "system.cfg").write_text("sys_spec = 1\nr_Width = 1366\nr_Height = 768\n"
                                     "r_Fullscreen = 1\nr_vsync = 0\ns_SoundEnable = 1\n")
    os.chown(GAME / "system.cfg", UID, UID)
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
        cfg.write_text(text)
    attributes = SAVES / "Profiles/default/attributes.xml"
    if attributes.exists():
        attributes.write_text(re.sub(r'(name="OptionCfg\.sys_spec_[A-Za-z]+" value=")[0-9]+',
                                     r"\g<1>1", attributes.read_text(errors="replace")))


def chown_tree(uid, gid):
    """Give every file of /data/crysis to UID and GID."""
    for top, dirs, files in os.walk(BASE):
        for entry in dirs + files:
            os.lchown(os.path.join(top, entry), uid, gid)
    os.lchown(BASE, uid, gid)


def prepare(out):
    """Give the game to the desktop account, set the session's output to
    1366x768 and write the game's settings; record OUT for later commands."""
    if not (GAME / "Bin64/Crysis.exe").exists():
        raise SystemExit("crysis: no game in %s" % GAME)
    STATE.mkdir(exist_ok=True)
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    os.chown(out, UID, UID)
    (STATE / "out").write_text(str(out) + "\n")
    info = BASE.stat()
    if not OWNER.exists():
        OWNER.write_text("%d %d\n" % (info.st_uid, info.st_gid))
    say("owner: %s, recorded in %s" % (OWNER.read_text().strip(), OWNER))
    chown_tree(UID, UID)
    output = wm.session("1366x768")
    text = wm.log_from(0)
    renderer = re.findall(r"GL renderer: (.*)", text)
    say("session: output %s, renderer %s" % (output, renderer[-1] if renderer else None))
    settings()
    set_volume()
    say("volume: %s" % volume())
    print("CRYSIS PREPARED", flush=True)


def restore():
    """Give /data/crysis back to the owner `prepare` recorded."""
    if not OWNER.exists():
        raise SystemExit("crysis: no recorded owner")
    uid, gid = (int(n) for n in OWNER.read_text().split())
    OWNER.unlink()
    chown_tree(uid, gid)
    say("restored: /data/crysis to %d:%d" % (uid, gid))


class Game:
    """One start of the game as the desktop account, with its logs in OUT."""

    def __init__(self, out, route, args, prefix=None):
        self.out = out
        self.prefix = prefix or str(BASE / "compat")
        self.hud = out / "hud"
        self.hud.mkdir()
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
        if (GAME / "Game.log").exists():
            (GAME / "Game.log").unlink()
        self.log = open(out / "game.out", "w")
        self.started = time.monotonic()
        self.proc = subprocess.Popen(desktop(line), stdout=self.log, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, start_new_session=True)
        self.volume = volume()
        self.reset = False
        self.sizes = None
        self.still = time.monotonic()
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
        """Return the level's loading time from Game.log, or None."""
        try:
            text = (GAME / "Game.log").read_text(errors="replace")
        except OSError:
            return None
        found = re.search(r"\*LOADING: Level (\S+) loading time: ([\d.]+) seconds", text)
        return found.group(2) if found else None

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
        self.log.close()
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


def frame_stats(values):
    """Return the median, the 99th percentile and the worst of VALUES, in ms,
    and how many pass 50 and 100 ms."""
    s = sorted(values)
    at = lambda q: s[min(len(s) - 1, int(q * len(s)))]
    return "median %.1f p99 %.1f max %.1f >50ms %d >100ms %d" % (
        at(0.5), at(0.99), s[-1], sum(1 for v in s if v > 50), sum(1 for v in s if v > 100))


def flyby(route, name, loops):
    """Run the flyby for LOOPS loops and judge each against GATE."""
    out = out_dir(name)
    if BENCH.exists():
        BENCH.unlink()
    settings()
    game = Game(out, route, ["+map", "island", "+exec", "benchmark_gpu"])
    ends = []
    try:
        game.wait(lambda: game.loaded() is not None, "load")
        say("loaded in %s s" % game.loaded())
        while len(ends) < loops:
            done = len(finished())
            if done > len(ends):
                ends.append(game.frames())
                say("loop %d finished at frame %d" % (done - 1, ends[-1]))
                continue
            if not game.wait(lambda: len(finished()) > len(ends), "loop %d" % len(ends)):
                break
    finally:
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
    previous = None
    for i, (fps, play) in enumerate(runs):
        end = ends[i] if i < len(ends) else len(dump)
        start = max(end - 2000, 0) if previous is None else previous
        previous = end
        segment = dump[start:end]
        beside = (" linux %.2f" % linux[i]) if i < len(linux) else ""
        check("loop%d" % i, fps >= gate, "%.2f fps (gate %.0f)%s, play %.2f s%s" % (
            fps, gate, beside, play, (", " + frame_stats(segment)) if segment else ""))
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
    # The dialog's log level is set at the start: a console line typed
    # beside others was once lost, and its lines are what `lines` reads.
    game = Game(out, route, ["+load", copy, "+log_verbosity", "4", "+ds_loglevel", "4"])
    try:
        if load_save(game, copy, "load"):
            game.shot("loaded")
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
    usage = "usage: crysis.py prepare OUT | flyby ROUTE NAME [LOOPS] | hud NAME SAVE [ROUTE] | " \
            "save NAME [ROUTE] | load NAME SAVE [ROUTE [PREFIX]] | restore"
    if not argv:
        raise SystemExit(usage)
    command, rest = argv[0], argv[1:]
    if command == "prepare" and len(rest) == 1:
        prepare(rest[0])
        return 0
    if command == "restore" and not rest:
        restore()
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
    sys.exit(main())
