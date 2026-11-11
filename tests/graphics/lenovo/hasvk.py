"""Present vkcube and Chromium WebGPU on the Lenovo, as USER, keeping evidence in OUT.

Run as root: python3 hasvk.py USER OUT. The trial owns the screen. It ends
the boot session's Weston, uses seatd for a private DRM Weston with GL and
kiosk-shell, and leaves the console when done. OUT must not exist. The
caller owns the trial boot, its notice and its return to the release.
"""
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
GRAPHICS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(GRAPHICS / "browser"))
from run import save, stop
from judge import sandbox
sys.path.insert(0, str(GRAPHICS / "runners"))
from frame import rows


def user_command(user, args):
    """Run one argv as the ordinary user, with supplementary groups and this environment."""
    return ["su", "-s", "/bin/sh", user, "-c", "exec " + shlex.join(map(str, args))]


def run(args, log, env):
    """Run a child to completion; stop its process group on any exit path."""
    child = None
    print("hasvk: action: " + shlex.join(map(str, args)), flush=True)
    with log.open("w") as output:
        try:
            child = subprocess.Popen(args, env=env, stdout=output, stderr=output,
                                     start_new_session=True)
            status = child.wait()
            print("hasvk: child: " + output.name + ": exit " + str(status), flush=True)
            assert status == 0, "%s: exit %d; read %s" % (args[0], status, log)
        finally:
            stop(child)


def end_desktop():
    """End the boot session's compositor, whose supervisor leaves the console up."""
    for proc in Path("/proc").glob("[0-9]*"):
        try:
            if (proc / "comm").read_text().strip() != "weston":
                continue
            os.kill(int(proc.name), signal.SIGTERM)
            for _ in range(100):
                stat = (proc / "stat").read_text().rsplit(")", 1)[1].split()
                if stat[0] == "Z":
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError("Weston did not leave the card: " + proc.name)
        except (FileNotFoundError, ProcessLookupError):
            pass


def judge(out):
    """Require Intel readbacks, a panel picture and Debian's sandbox policy; record rates."""
    reference = GRAPHICS / "ref/lenovo/hasvk"
    got = json.loads((out / "webgpu/result.json").read_text())
    picture = got["picture"]
    expected = json.loads((reference / "chromium-pic-blocklist.json").read_text())
    assert picture["adapter"] == expected["adapter"], picture["adapter"]
    assert picture["canvas"] == expected["canvas"], picture["canvas"]
    assert picture["frames"] == expected["frames"] == 480, picture
    width, height, pixels = rows(out / "webgpu/panel.png")
    assert [width, height] == expected["canvas"][:2], (width, height)
    for name, x, y in (("centerPixel", width // 2, round(height * 0.6)), ("cornerPixel", 2, 2)):
        wanted = expected[name]
        actual = picture[name]
        assert len(actual) == 4 and all(abs(a - b) <= 1 for a, b in zip(actual, wanted)), (name, actual, wanted)
        screen = list(pixels[y][x * 3:x * 3 + 3])
        assert all(abs(a - b) <= 1 for a, b in zip(screen, wanted[:3])), (name, screen, wanted)
    dawn = got["chrome://gpu"]
    assert re.search(r"Vulkan backend[^\n]*HD Graphics 4400", dawn), "Dawn did not name hasvk"
    baseline = GRAPHICS / "browser/ref/chromium-sandbox"
    assert sandbox(got, "chromium") == sandbox(json.loads((baseline / "result.json").read_text()), "chromium")
    switches = json.loads((out / "webgpu/switches.json").read_text())
    assert switches["version"] == json.loads((baseline / "switches.json").read_text())["version"], "different Chromium binaries"
    args = switches["argv"]
    assert "--ignore-gpu-blocklist" in args and "--enable-unsafe-webgpu" not in args, args
    assert not any(a.startswith(("--use-webgpu-adapter", "--use-angle", "--use-vulkan")) for a in args), args
    timings = json.loads((out / "cube-timing.json").read_text())
    linux = re.findall(r"host vkcube --c (300|900): exit 0 in ([0-9.]+) s", (reference / "host-cube.txt").read_text())
    assert [row["frames"] for row in timings] == [int(n) for n, _ in linux] == [300, 900, 300, 900]
    rates = {"vkcube": [], "webgpu": {"linux_fps": expected["frames"] / expected["seconds"],
                                      "rach2_fps": picture["frames"] / picture["seconds"]}}
    for index in (0, 2):
        rates["vkcube"].append({"linux_fps": 600 / (float(linux[index + 1][1]) - float(linux[index][1])),
                                "rach2_fps": 600 / (timings[index + 1]["seconds"] - timings[index]["seconds"])})
    save(out / "rates.json", rates)
    print("hasvk: rates: " + json.dumps(rates, sort_keys=True), flush=True)
    print("hasvk: result: pass: vkcube, WebGPU compute, pixels, panel and sandbox", flush=True)


def main():
    """Own a private compositor and children, removing scratch even after a failed step."""
    user, name = sys.argv[1:]
    assert os.getuid() == 0, "run as root to read sandbox state"
    account = pwd.getpwnam(user)
    assert account.pw_uid != 0, "the programs need an ordinary user"
    out = Path(name).resolve()
    assert not out.exists(), "use a fresh output directory"
    cards = [p for p in Path("/sys/class/drm").glob("card[0-9]*") if re.fullmatch(r"card\d+", p.name)]
    intel = [p for p in cards if (p / "device/vendor").exists() and
             (p / "device/vendor").read_text().strip() == "0x8086" and
             (p / "device/device").read_text().strip() == "0x0a16"]
    assert len(intel) == 1, "need the Lenovo's HD 4400"
    assert (intel[0] / "device/driver").resolve().name == "i915", "i915 is not bound"
    icds = list(Path("/usr/share/vulkan/icd.d").glob("intel_hasvk_icd*.json"))
    icds = [p for p in icds if p.name in ("intel_hasvk_icd.json", "intel_hasvk_icd.x86_64.json")]
    assert len(icds) == 1, "need exactly one x86-64 hasvk ICD"
    for key in ("MESA_VK_VERSION_OVERRIDE", "MESA_LOADER_DRIVER_OVERRIDE", "LIBGL_ALWAYS_SOFTWARE",
                "INTEL_DEBUG", "INTEL_DEVID_OVERRIDE", "MESA_DRICONF_EXECUTABLE_OVERRIDE", "MESA_DRICONF"):
        assert key not in os.environ, "unset " + key
    work = weston = None
    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sig, lambda *_: sys.exit(1))
    try:
        out.mkdir(parents=True)
        os.chown(out, account.pw_uid, account.pw_gid)
        work = Path(tempfile.mkdtemp(prefix="rach2-hasvk.", dir=os.environ.get("TMPDIR", "/tmp")))
        os.chown(work, account.pw_uid, account.pw_gid)
        runtime = work / "run"
        runtime.mkdir(mode=0o700)
        os.chown(runtime, account.pw_uid, account.pw_gid)
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), WAYLAND_DISPLAY="wayland-hasvk",
                   VK_DRIVER_FILES=str(icds[0]), MESA_SHADER_CACHE_DIR=str(work / "cache"),
                   GRAPHICS_CLASS="performance", GRAPHICS_DONE="1", PYTHONDONTWRITEBYTECODE="1")
        save(out / "machine.json", {"card": str(intel[0]), "icd": str(icds[0]),
             "cmdline": Path("/proc/cmdline").read_text(), "user": user,
             "kernel": os.uname().release})
        if shutil.which("mixer") and Path("/dev/mixer").exists():
            subprocess.run(["mixer", "vol", "1"], check=True)
        end_desktop()
        command = user_command(user, ["env", "LIBSEAT_BACKEND=seatd", "weston", "--no-config",
            "--debug", "--backend=drm", "--renderer=gl", "--shell=kiosk", "--xwayland",
            "--idle-time=0", "--socket=wayland-hasvk", "--log=" + str(out / "weston.log")])
        # Weston keeps its own shader cache, which the clients' clearing
        # below would otherwise race with as Weston writes it.
        weston_env = dict(env, MESA_SHADER_CACHE_DIR=str(work / "weston-cache"))
        with (out / "weston.out").open("w") as log:
            weston = subprocess.Popen(command, env=weston_env, stdout=log, stderr=log,
                                      start_new_session=True)
        for _ in range(600):
            assert weston.poll() is None, "Weston ended; read weston.log"
            if (runtime / "wayland-hasvk").is_socket():
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("Weston made no socket")
        log = (out / "weston.log").read_text()
        assert "HD Graphics 4400" in log, "Weston did not name Crocus"
        # The first pair starts from the empty shader cache guest-env.sh makes.
        setup = ". " + shlex.quote(str(GRAPHICS / "guest-env.sh"))
        run(user_command(user, ["sh", "-c", setup + " && graphics_env"]), out / "environment.log", env)
        assert "env MESA_SHADER_CACHE_FILES=0\n" in (out / "environment.log").read_text(), \
            "the shader cache is not empty; read environment.log"
        env["GRAPHICS_DONE"] = "0"
        timings = []
        for index, count in enumerate((300, 900, 300, 900)):
            log = out / ("cube-%d.log" % index)
            start = time.monotonic()
            command = setup + " && exec vkcube --wsi wayland --c " + str(count)
            run(user_command(user, ["sh", "-c", command]), log, env)
            seconds = time.monotonic() - start
            text = log.read_text()
            assert re.search(r"Selected GPU.*HD Graphics 4400.*IntegratedGpu", text), text
            timings.append({"frames": count, "seconds": seconds})
            save(out / "cube-timing.json", timings)
        env["GRAPHICS_DONE"] = "1"
        capture = shlex.join(["sh", str(GRAPHICS / "runners/vkcube.sh"), "-d", "-i", str(icds[0]), "-o", str(out / "cube")])
        run(user_command(user, ["sh", "-c", setup + " && " + capture]), out / "cube-capture.log", env)
        frames = (out / "cube/frames").read_text().splitlines()
        assert len(frames) == 2 and frames[0].split()[-1] != frames[1].split()[-1], "cube frames did not change"
        assert all("1366x768" in line for line in frames), frames
        run(["python3", str(GRAPHICS / "browser/sandbox.py"), user, "chromium",
             str(out / "webgpu"), "webgpu-hasvk"], out / "webgpu.log", env)
        judge(out)
    finally:
        stop(weston)
        if work is not None:
            shutil.rmtree(work)
        if out.exists():
            print("hasvk: evidence: " + str(out), flush=True)


if __name__ == "__main__":
    main()
