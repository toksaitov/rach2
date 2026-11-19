#!/usr/bin/python3
"""Start the desktop session, check it and measure browser and ALSA tones through OSS."""
import array
import fcntl
import http.server
import math
import os
from pathlib import Path
import signal
import shlex
import socketserver
import struct
import subprocess
import tempfile
import threading
import time
import wave


def command(*args):
    """Run a command and require its successful exit."""
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def desktop(line):
    """Form a command under the desktop user's groups and session environment."""
    return ["su", "-s", "/bin/bash", "desktop", "-c",
            "export XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 "
            "PULSE_SERVER=unix:/run/user/1001/pulse/native; " + line]


SEAT_CLIENT = r'''import ctypes, os, threading
os.environ["LIBSEAT_BACKEND"] = "seatd"
lib = ctypes.CDLL("libseat.so.1", use_errno=True)
callback = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)
enabled = threading.Event()
enable = callback(lambda *_: enabled.set())
disable = callback(lambda seat, _: lib.libseat_disable_seat(seat))

class Listener(ctypes.Structure):
    """The two seat state callbacks of the public libseat interface."""
    _fields_ = [("enable", callback), ("disable", callback)]

lib.libseat_open_seat.argtypes = [ctypes.POINTER(Listener), ctypes.c_void_p]
lib.libseat_open_seat.restype = ctypes.c_void_p
lib.libseat_dispatch.argtypes = [ctypes.c_void_p, ctypes.c_int]
lib.libseat_disable_seat.argtypes = [ctypes.c_void_p]
lib.libseat_open_device.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                    ctypes.POINTER(ctypes.c_int)]
lib.libseat_close_device.argtypes = [ctypes.c_void_p, ctypes.c_int]
lib.libseat_close_seat.argtypes = [ctypes.c_void_p]
listener = Listener(enable, disable)
seat = lib.libseat_open_seat(ctypes.byref(listener), None)
assert seat, ctypes.get_errno()
try:
    while not enabled.is_set():
        assert lib.libseat_dispatch(seat, -1) >= 0, ctypes.get_errno()
    fd = ctypes.c_int(-1)
    device = lib.libseat_open_device(seat, b"/dev/input/event0", ctypes.byref(fd))
    assert device >= 0 and fd.value >= 0, ctypes.get_errno()
    os.close(fd.value)
    assert lib.libseat_close_device(seat, device) == 0, ctypes.get_errno()
finally:
    assert lib.libseat_close_seat(seat) == 0, ctypes.get_errno()
'''


def session():
    """Start the compositor and check its identity, seat and input database."""
    original = os.open("/sys/class/net", os.O_PATH | os.O_DIRECTORY)
    reopened = None
    try:
        reopened = os.open("/proc/self/fd/%d" % original, os.O_PATH | os.O_DIRECTORY)
        assert os.fstat(original).st_ino == os.fstat(reopened).st_ino
        os.close(original)
        original = None
        assert os.fstat(reopened).st_mode & 0o170000 == 0o040000
    finally:
        if original is not None:
            os.close(original)
        if reopened is not None:
            os.close(reopened)
    with tempfile.TemporaryDirectory(prefix="desktop-path-", dir="/tmp") as name:
        path = Path(name) / "held"
        path.write_bytes(b"held inode")
        original = os.open(path, os.O_RDONLY)
        reopened = None
        try:
            reopened = os.open("/proc/self/fd/%d" % original, os.O_PATH)
            path.unlink()
            os.close(original)
            original = None
            assert os.fstat(reopened).st_nlink == 0
            reader = os.open("/proc/self/fd/%d" % reopened, os.O_RDONLY)
            try:
                assert os.read(reader, 32) == b"held inode"
            finally:
                os.close(reader)
        finally:
            if original is not None:
                os.close(original)
            if reopened is not None:
                os.close(reopened)
    runtime = Path("/run/user/1001")
    # The supervisor starts the session on SIGHUP, not at boot; its pid
    # file is there once the seat socket exists.
    supervisor = Path("/run/rach2-desktop.pid")
    while not supervisor.exists() or not supervisor.read_text().endswith("\n"):
        time.sleep(0.1)
    os.kill(int(supervisor.read_text()), signal.SIGHUP)
    while not (runtime / "wayland-0").is_socket():
        time.sleep(0.1)
    found = False
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                if (entry / "comm").read_text().strip() != "weston":
                    continue
                status = (entry / "status").read_text().splitlines()
                assert next(s for s in status if s.startswith("Uid:")).split()[1:] == ["1001"] * 4
                found = True
            except FileNotFoundError:
                continue
    assert found, "no Weston owned by desktop"
    assert runtime.stat().st_uid == 1001 and runtime.stat().st_mode & 0o777 == 0o700
    assert Path("/run/seatd.sock").is_socket()
    assert command("stat", "-c", "%G", "/run/seatd.sock").strip() == "seat"
    props = command("udevadm", "info", "--query=property", "--name=/dev/input/event0")
    assert "ID_INPUT=1" in props and "ID_INPUT_KEYBOARD=1" in props, props
    command(*desktop("python3 -c " + shlex.quote(SEAT_CLIENT)))
    # PulseAudio starts beside Weston, not after it.
    while not (runtime / "pulse/native").is_socket():
        time.sleep(0.1)
    command(*desktop("pactl info"))
    print("desktop: result: session", flush=True)


def record(stop, samples, ready):
    """Read mono signed 16-bit frames at 48000 Hz until the player ends."""
    fd = os.open("/dev/dsp", os.O_RDONLY)
    try:
        for request, value in [(0xc0045005, 16), (0xc0045006, 1), (0xc0045002, 48000)]:
            arg = array.array("i", [value])
            fcntl.ioctl(fd, request, arg, True)
            assert arg[0] == value, (request, arg[0])
        ready.set()
        while not stop.is_set():
            samples.extend(os.read(fd, 4800))
    finally:
        os.close(fd)


def pitch(block):
    """Give the cycles and samples of the sounding stretches of `block`.

    A stretch ends at 48 samples, 1 ms, below 200 in magnitude, which a 440 Hz
    tone of the case's level never holds; within a stretch the cycles are
    counted between its first and last rising zero crossing, and a crossing
    in the quiet samples after the stretch's last loud one is not its own.
    """
    cycles = samples = 0
    rising = []
    loud = None
    for i in range(1, len(block) + 1):
        if i < len(block) and abs(block[i]) >= 200:
            loud = i
        if loud is not None and (i == len(block) or i - loud == 48):
            rising = [r for r in rising if r <= loud]
            if len(rising) >= 2:
                cycles += len(rising) - 1
                samples += rising[-1] - rising[0]
            rising = []
            loud = None
        elif loud is not None and block[i - 1] <= 0 < block[i]:
            rising.append(i)
    return cycles, samples


def measured(data):
    """Require at least ten 50 ms windows of the requested 440 Hz tone.

    A window is judged by the pitch of the stretches of it that sound. When
    an emulated machine runs the player or the sound server late, the device
    plays silence for the frames that did not come in time, and the window
    holds the tone and a gap; the tone keeps its pitch on either side of the
    gap. A window that sounds for less than half its length holds the tone's
    start or end, or most of a gap, and is not judged. A fault in the route
    itself, such as a wrong rate, moves the pitch of every window, so one off
    window is allowed for every five good ones.
    """
    values = struct.unpack("<%dh" % (len(data) // 2), data)
    good = off = 0
    windows = []
    for at in range(0, len(values) - 2400, 2400):
        block = values[at:at + 2400]
        rms = math.sqrt(sum(x*x for x in block) / len(block))
        if rms < 200:
            continue
        cycles, samples = pitch(block)
        if samples < 1200:
            windows.append((int(rms), samples, None))
            continue
        hz = cycles * 48000 // samples
        windows.append((int(rms), samples, hz))
        if abs(hz - 440) <= 40:
            good += 1
        else:
            off += 1
    assert good >= 10 and off * 5 <= good, ("tone windows", good, "off", off, windows)
    return good


def tone(name, work, server):
    """Record one real client and judge its loopback frequency and amplitude."""
    stop, ready = threading.Event(), threading.Event()
    samples = bytearray()
    reader = threading.Thread(target=record, args=(stop, samples, ready))
    reader.start()
    player = None
    try:
        while not ready.wait(0.1):
            assert reader.is_alive(), "OSS reader failed"
        if name == "alsa":
            line = "exec aplay -q " + str(work / "tone.wav")
        elif name == "chromium":
            line = ("exec chromium-headless-shell --headless --disable-gpu-sandbox "
                    "--autoplay-policy=no-user-gesture-required --no-first-run "
                    "--user-data-dir=" + str(work / "chromium") + " " + server.url)
        else:
            profile = work / "firefox"
            profile.mkdir()
            (profile / "user.js").write_text(
                'user_pref("media.autoplay.default", 0);\n'
                'user_pref("media.autoplay.block-webaudio", false);\n'
                'user_pref("media.cubeb.backend", "pulse");\n')
            for path in (profile, profile / "user.js"):
                os.chown(path, 1001, 1001)
            line = "exec firefox-esr --headless --no-remote --profile " + str(profile) + " " + server.url
        output = work / (name + ".log")
        with output.open("w") as log:
            server.done.clear()
            player = subprocess.Popen(desktop(line), stdout=log, stderr=log, start_new_session=True)
            if name == "alsa":
                assert player.wait() == 0, ("aplay failed", output.read_text()[-4000:])
            else:
                while not server.done.wait(0.1):
                    assert player.poll() is None, (name + " exited before its tone ended",
                                                   player.returncode, output.read_text()[-4000:])
                assert server.pulse_seen, "browser did not play through PulseAudio"
    finally:
        if player is not None and player.poll() is None:
            os.killpg(player.pid, signal.SIGTERM)
            player.wait()
        stop.set()
        reader.join()
    windows = measured(samples)
    print("desktop: result: %s: %d tone windows" % (name, windows), flush=True)


class Handler(http.server.BaseHTTPRequestHandler):
    """Serve one tone page and witness its Web Audio completion."""
    def do_GET(self):
        """Serve the page or acknowledge a browser's tone witness."""
        if self.path == "/start":
            inputs = command(*desktop("pactl list short sink-inputs"))
            self.server.pulse_seen = bool(inputs.strip())
            content = b"ok"
        elif self.path == "/done":
            self.server.done.set()
            content = b"ok"
        else:
            content = b'''<!doctype html><title>Desktop tone</title><script>
(async () => {
 const ctx = new AudioContext(); await ctx.resume();
 const osc = ctx.createOscillator(); const gain = ctx.createGain();
 osc.frequency.value = 440; gain.gain.value = 0.4;
 osc.connect(gain).connect(ctx.destination); osc.start();
 await new Promise(r => setTimeout(r, 500)); await fetch('/start');
 osc.stop(ctx.currentTime + 2);
 osc.onended = async () => { await ctx.close(); await fetch('/done'); };
})();
</script>'''
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def log_message(self, *_args):
        """Keep HTTP traffic out of the case's result stream."""


def main():
    """Run the session check and three independent sound routes, cleaning up."""
    session()
    with tempfile.TemporaryDirectory(prefix="desktop-", dir="/tmp") as name:
        work = Path(name)
        os.chown(work, 1001, 1001)
        with wave.open(str(work / "tone.wav"), "wb") as wav:
            wav.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
            wav.writeframes(b"".join(struct.pack("<h", int(13000 * math.sin(2 * math.pi * 440 * i / 48000)))
                                     for i in range(96000)))
        with socketserver.TCPServer(("127.0.0.1", 0), Handler) as server:
            server.done = threading.Event()
            server.pulse_seen = False
            server.url = "http://127.0.0.1:%d/" % server.server_address[1]
            worker = threading.Thread(target=server.serve_forever)
            worker.start()
            try:
                for client in ["chromium", "firefox", "alsa"]:
                    server.pulse_seen = False
                    tone(client, work, server)
            finally:
                server.shutdown()
                worker.join()


if __name__ == "__main__":
    main()
