#!/usr/bin/env python3
"""t5804 guest: Weston with Xwayland as run.py starts it, then, on the host's
"go" line on standard input, one cold wineboot through rach2-game in a fresh
prefix, as run.py's make_prefix does. Prints WESTON, GO, T (each 10 s), MADE,
END and DONE lines; the host reads them as they come."""
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time

OUT = Path("/tmp/t5804-out")
FAIL = "The explorer process failed to start"


def say(text):
    print(text, flush=True)


def scan(uid, names):
    """Return {pid: comm} of this account's processes whose comm is in NAMES."""
    found = {}
    for entry in Path("/proc").glob("[0-9]*"):
        try:
            if entry.stat().st_uid != uid:
                continue
            comm = (entry / "comm").read_text().strip()
        except OSError:
            continue
        if comm in names:
            found[int(entry.name)] = comm
    return found


def main():
    OUT.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="t5804.", dir="/tmp"))
    env = {k: v for k, v in os.environ.items() if not k.startswith(
        ("PROTON_", "WINE", "STEAM_", "DXVK_", "VKD3D_", "WAYLAND_", "LP_"))
        and k not in ("SteamGameId", "SteamAppId", "GALLIUM_DRIVER", "LIBGL_ALWAYS_SOFTWARE",
                      "DISPLAY", "XDG_DATA_HOME", "XDG_CACHE_HOME")}
    run = work / "run"
    run.mkdir(mode=0o700)
    env.update(XDG_RUNTIME_DIR=str(run), XDG_DATA_HOME=str(work / "data"),
               XDG_CACHE_HOME=str(work / "cache"),
               MESA_SHADER_CACHE_DIR=str(work / "shader-cache"),
               STEAM_COMPAT_CLIENT_INSTALL_PATH=str(work / "steam"),
               PROTON_LOG_DIR=str(work / "logs"), WINEDEBUG="-all,err+all")
    log = OUT / "weston.log"
    weston = subprocess.Popen(
        ["weston", "--backend=headless", "--renderer=gl", "--width=1280", "--height=800",
         "--shell=desktop", "--idle-time=0", "--xwayland", "--socket=games-wine",
         "--log=" + str(log)], env=env, stdout=open(OUT / "weston.out", "wb"),
        stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    while True:
        text = log.read_text(errors="replace") if log.exists() else ""
        found = re.search(r"xserver listening on display (:\d+)", text)
        if found or weston.poll() is not None:
            break
        time.sleep(0.05)
    if not found:
        say("FAIL weston ended without Xwayland")
        return 1
    env["DISPLAY"] = found.group(1)
    renderer = re.search(r"GL renderer: (.*)", text)
    say("WESTON display %s renderer %s" % (found.group(1),
                                           renderer.group(1) if renderer else None))
    if sys.stdin.readline().strip() != "go":
        say("FAIL no go")
        return 1
    t0 = time.monotonic()
    say("GO")
    prefix = work / "compat"
    plog = OUT / "prefix.log"
    proc = subprocess.Popen(
        ["sh", "/usr/local/bin/rach2-game", "--id", "2733", "--prefix", str(prefix), "--",
         "c:\\windows\\system32\\cmd.exe", "/c", "exit", "0"],
        env=env, stdout=open(plog, "wb"), stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL, start_new_session=True)
    uid = os.getuid()
    seen = {}       # explorer.exe pid: [first, last] seconds after go
    made = None
    tick = 0.0
    while proc.poll() is None:
        now = time.monotonic() - t0
        for pid in scan(uid, ("explorer.exe",)):
            seen.setdefault(pid, [now, now])[1] = now
        if made is None and "rach2-game: prefix: made" in plog.read_text(errors="replace"):
            made = now
            say("MADE %.1f" % made)
        if now >= tick:
            alive = scan(uid, ("explorer.exe",))
            say("T %.0f explorer %s" % (now, ",".join(map(str, sorted(alive))) or "none"))
            tick += 10
        time.sleep(0.25)
    end = time.monotonic() - t0
    alive = scan(uid, ("explorer.exe",))
    text = plog.read_text(errors="replace")
    if made is None and "rach2-game: prefix: made" in text:
        made = end
    say("END status %d secs %.1f made %s explorer_seen %s explorer_alive_at_end %s" % (
        proc.returncode, end, "%.1f" % made if made is not None else "none",
        ";".join("%d@%.1f-%.1f" % (p, a, b) for p, (a, b) in sorted(seen.items())) or "never",
        ",".join(map(str, sorted(alive))) or "none"))
    wenv = dict(env, WINEPREFIX=str(prefix / "pfx"))
    server = "/opt/proton/files/bin/wineserver"
    gone = None
    for _ in range(600):
        if not scan(uid, ("explorer.exe",)):
            gone = time.monotonic() - t0
            break
        time.sleep(0.1)
    proton = work / "logs" / "steam-2733.log"
    if proton.exists():
        (OUT / "prefix.proton.log").write_bytes(proton.read_bytes())
        text += proton.read_text(errors="replace")
    say("DONE failmsg %s nodrv %s explorer_gone %s" % (
        FAIL in text, "no driver could be loaded" in text,
        "%.1f" % gone if gone is not None else "no"))
    subprocess.run([server, "-k"], env=wenv, check=False)
    subprocess.run([server, "-w"], env=wenv, check=False)
    os.killpg(weston.pid, signal.SIGTERM)
    weston.wait()
    return 0


if __name__ == "__main__":
    sys.exit(main())
