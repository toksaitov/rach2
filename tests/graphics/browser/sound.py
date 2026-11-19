"""Capture three concurrent browser tabs through OSS or Debian's null-sink monitor.

The OSS route plays through the desktop session's PulseAudio, whose
supervisor starts the session on SIGHUP, since the boot starts none, and ends
one started here on SIGUSR2 at the end.
"""
import array
import fcntl
import json
import math
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import time

from run import stop


def measure(data):
    """Require all three frequencies together in ten or more 50 ms windows."""
    values = struct.unpack("<%dh" % (len(data) // 2), data)
    good = 0
    powers = []
    for start in range(0, len(values) - 2399, 2400):
        block = values[start:start + 2400]
        row = []
        for hz in (440, 660, 880):
            real = sum(v * math.cos(2 * math.pi * hz * i / 48000) for i, v in enumerate(block))
            imag = sum(v * math.sin(2 * math.pi * hz * i / 48000) for i, v in enumerate(block))
            row.append(round(2 * math.hypot(real, imag) / len(block)))
        powers.append(row)
        if min(row) >= 200:
            good += 1
    assert good >= 10, (good, powers)
    return {"samples": len(values), "mixed_windows": good, "amplitudes": powers}


SUPERVISOR = Path("/run/rach2-desktop.pid")
PULSE = Path("/run/user/1001/pulse/native")


def supervisor():
    """Wait for the desktop supervisor's pid file, whole, and give its pid."""
    while True:
        try:
            text = SUPERVISOR.read_text()
        except FileNotFoundError:
            text = ""
        if text.endswith("\n"):
            return int(text)
        time.sleep(0.1)


def session_start():
    """Start the desktop session unless its PulseAudio listens; whether it
    was started here."""
    if PULSE.is_socket():
        return False
    os.kill(supervisor(), signal.SIGHUP)
    while not PULSE.is_socket():
        time.sleep(0.1)
    return True


def main():
    """Own the capture and player, stop and reap both, then judge the captured samples."""
    route, browser, out_name = sys.argv[1:]
    out = Path(out_name).resolve()
    out.mkdir(parents=True, exist_ok=True)
    here = Path(__file__).resolve().parent
    player = pulse = recorder = None
    fd = None
    started = False
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda *_: sys.exit(1))
    with tempfile.TemporaryDirectory(prefix="browser-sound-", dir=os.environ.get("TMPDIR", "/tmp")) as tmp:
        work = Path(tmp)
        env = dict(os.environ)
        try:
            if route == "oss":
                started = session_start()
                os.chown(out, 1001, 1001)
                fd = os.open("/dev/dsp", os.O_RDONLY)
                for request, value in ((0xc0045005, 16), (0xc0045006, 1), (0xc0045002, 48000)):
                    arg = array.array("i", [value])
                    fcntl.ioctl(fd, request, arg, True)
                    assert arg[0] == value, (request, arg[0])
                argv = ["su", "-s", "/bin/sh", "desktop", "-c",
                        "export PULSE_SERVER=unix:/run/user/1001/pulse/native "
                        "GRAPHICS_CLASS=conformance GRAPHICS_DONE=1; . %s/../guest-env.sh && "
                        "sh %s/run.sh %s sound %s" % (here, here, browser, out)]
            elif route == "pulse":
                env["PULSE_SERVER"] = "unix:" + str(work / "pulse")
                with (out / "pulse.log").open("w") as log:
                    pulse = subprocess.Popen(["pulseaudio", "-n", "--daemonize=no", "--exit-idle-time=-1",
                        "--load=module-native-protocol-unix socket=%s auth-anonymous=1" % (work / "pulse"),
                        "--load=module-null-sink sink_name=acceptance"],
                        env=env, stdout=log, stderr=log, start_new_session=True)
                while not (work / "pulse").exists():
                    assert pulse.poll() is None, "PulseAudio exited"
                    time.sleep(0.1)
                recorder = subprocess.Popen(["parec", "--device=acceptance.monitor", "--format=s16le",
                    "--rate=48000", "--channels=1"], env=env, stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL, start_new_session=True)
                fd = recorder.stdout.fileno()
                argv = ["sh", str(here / "run.sh"), browser, "sound", str(out)]
            else:
                raise ValueError(route)
            player = subprocess.Popen(argv, env=env, start_new_session=True)
            samples = bytearray()
            while len(samples) < 192000:
                assert player.poll() is None, "browser sound runner exited"
                part = os.read(fd, 4800)
                assert part, "capture ended"
                if (out / "tones-ready").exists():
                    samples.extend(part)
            (out / "tones-stop").write_text("stop\n")
            assert player.wait() == 0, "browser sound runner failed"
            (out / "sound.raw").write_bytes(samples)
            result = measure(samples)
            (out / "sound.json").write_text(json.dumps(result, indent=2) + "\n")
            print("browser: result: %s sound %d mixed windows" % (browser, result["mixed_windows"]), flush=True)
        finally:
            stop(player)
            stop(recorder)
            stop(pulse)
            if recorder:
                recorder.stdout.close()
            elif fd is not None:
                os.close(fd)
            if started:
                os.kill(supervisor(), signal.SIGUSR2)


if __name__ == "__main__":
    main()
