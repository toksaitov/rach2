#!/usr/bin/env python3
"""Run one Direct3D row in a private workspace; retain the named evidence directory."""
import concurrent.futures
import getopt
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
WINE_SUMMARY = r"%s: (\d+) tests executed \((\d+) marked as todo, (\d+) as flaky, (\d+) failures?\), (\d+) skipped"
# A line a Wine test prints for one of its checks, the progress of a unit
# that ends before its summary.
TEST_LINE = r"^\w+\.c:\d+: "
VK_SUMMARY = r"(\d+) tests executed \((\d+) failures, (\d+) successful todo, (\d+) skipped, (\d+) todo, (\d+) bugs\)"
CHILDREN = set()


def interrupted(signum, _frame):
    """Stop owned command groups before unwinding the workspace cleanup."""
    for proc in list(CHILDREN):
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    raise SystemExit(128 + signum)


STALL = "dxgi.c:8040: Test marked todo: d3d10: test 2: Got unexpected present count 1, expected 2."
# Each Vulkan device's ICD and the Linux reference taken on it.
ADAPTERS = {"lavapipe": ("lvp_icd*.json", "linux.json"), "venus": ("venus_icd*.json", "venus.json")}
# Each Wine path's DLL overrides, native or builtin, and WineD3D's renderer.
PATHS = {"proton": ("n", "gl"), "wined3d": ("b", "gl"), "wined3dvk": ("b", "vulkan")}


def run_command(argv, env, log, stop_at=None):
    """Wait for the command itself, retaining output even if a descendant keeps it open."""
    stopped = False
    with log.open("wb") as dest:
        proc = subprocess.Popen(argv, env=env, stdout=dest,
                                stderr=subprocess.STDOUT, start_new_session=True)
        CHILDREN.add(proc)
        try:
            if stop_at:
                marker = stop_at.encode()
                pending = b""
                with log.open("rb") as source:
                    while proc.poll() is None:
                        chunk = source.read()
                        pending = pending + chunk
                        if marker in pending:
                            stopped = True
                            os.killpg(proc.pid, signal.SIGKILL)
                            break
                        pending = pending[-len(marker):]
                        # Poll output; elapsed time decides no verdict.
                        time.sleep(0.02)
            proc.wait()
            (log.with_suffix(log.suffix + ".exit.json")).write_text(
                json.dumps({"pid": proc.pid, "exit": proc.returncode,
                            "progress": stopped}) + "\n")
            return proc.returncode, stopped
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
            CHILDREN.discard(proc)


def summaries(text, expression):
    """Sum a unit's own and child summaries, returning None when no summary was written."""
    found = re.findall(expression, text)
    return [sum(int(row[i]) for row in found) for i in range(len(found[0]))] if found else None


def adapter_name(text):
    """Return the adapter a probe named, up to its vendor ID, or None when it named none."""
    found = re.search(r"^adapter: (.*?) vendor 0x", text, re.M)
    return found.group(1) if found else None


def main():
    """Run FAMILY ROW in OUT and compare each unit with the recorded Linux outcomes.

    -a names the Vulkan device, lavapipe (the default) or venus, whose ICD
    alone the loader sees and whose Linux reference judges the row. -R runs
    the row with no reference and records each unit, as the references are
    taken; it fails only when a unit cannot be run.
    """
    usage = "usage: run.py [-a lavapipe|venus] [-R] probe|wine|vkd3d|private ROW OUT"
    try:
        opts, args = getopt.getopt(sys.argv[1:], "a:R")
    except getopt.GetoptError:
        raise SystemExit(usage)
    adapter = "lavapipe"
    record = False
    for opt, value in opts:
        if opt == "-a":
            adapter = value
        else:
            record = True
    if len(args) != 3 or adapter not in ADAPTERS:
        raise SystemExit(usage)
    family, row, output = args
    if os.geteuid() == 0:
        raise SystemExit("d3d: run as an ordinary user")
    out = Path(output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    if family == "private" and row == "files":
        status, _ = run_command([str(HERE / "privatefilemusl"), "/tmp"],
                                os.environ.copy(), out / "private.log")
        for line in (out / "private.log").read_text(errors="replace").splitlines():
            if not line.startswith("@etude "):
                print(line, flush=True)
        if status:
            raise SystemExit(status)
        print("D3DROW OK private files", flush=True)
        return
    pattern, reference = ADAPTERS[adapter]
    tests = list(json.loads((HERE / "ref" / "linux.json").read_text())["vkd3d"])
    ref = {} if record else json.loads((HERE / "ref" / reference).read_text())
    env = os.environ.copy()
    for key in ("PROTON_USE_WINED3D", "VKD3D_TEST_FILTER", "VKD3D_TEST_EXCLUDE", "VKD3D_TEST_MATCH", "VKD3D_FEATURE_LEVEL", "VKD3D_SHADER_MODEL"):
        env.pop(key, None)
    env.update(GALLIUM_DRIVER="llvmpipe", LIBGL_ALWAYS_SOFTWARE="1",
               WINEDEBUG="-all,err+all,warn+winstation", DXVK_LOG_LEVEL="info", VKD3D_DEBUG="info",
               MESA_VK_IGNORE_CONFORMANCE_WARNING="1", PROTON_USE_WOW64="1",
               SteamGameId="2693", DXVK_LOG_PATH=str(out),
               PROTON_LOG="0" if family == "wine" else "1",
               PROTON_LOG_DIR=str(out))
    icds = sorted(Path("/usr/share/vulkan/icd.d").glob(pattern))
    icds = [p for p in icds if "i686" not in p.name]
    if len(icds) != 1:
        raise SystemExit("d3d: expected one x86-64 %s ICD" % adapter)
    env["VK_DRIVER_FILES"] = str(icds[0])
    env["VK_ICD_FILENAMES"] = str(icds[0])
    env.pop("MESA_VK_WSI_DEBUG", None)
    if adapter == "venus":
        # Mesa's X11 WSI presents a GPU's images only over DRI3, which Xvfb
        # lacks; sw presents through the CPU, as it does for lavapipe.
        env["MESA_VK_WSI_DEBUG"] = "sw"
    work = None
    xvfb = None
    server = None
    results = {}
    try:
        work = Path(tempfile.mkdtemp(prefix="d3d-", dir=os.environ.get("TMPDIR", "/tmp")))
        env["MESA_SHADER_CACHE_DIR"] = str(work / "shader-cache")
        env["XDG_RUNTIME_DIR"] = str(work / "runtime")
        (work / "runtime").mkdir(mode=0o700)
        env["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = str(work / "steam")
        env["STEAM_COMPAT_DATA_PATH"] = str(work / "compat")
        (work / "steam").mkdir()
        (work / "compat").mkdir()
        rd, wr = os.pipe()
        try:
            xenv = env.copy()
            xenv["LD_LIBRARY_PATH"] = str(HERE / "xvfb/lib")
            with (out / "xvfb.log").open("wb") as log:
                xvfb = subprocess.Popen([str(HERE / "xvfb/Xvfb"), "-displayfd", str(wr), "-noreset",
                                         "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
                                        pass_fds=(wr,), stdout=log, stderr=log, env=xenv)
            os.close(wr)
            wr = None
            with os.fdopen(rd) as ready:
                display = ready.readline().strip()
            if not display.isdigit():
                raise RuntimeError("Xvfb did not announce a display")
            env["DISPLAY"] = ":" + display
        finally:
            if wr is not None:
                os.close(wr)
        cpus = int(env.get("GRAPHICS_CPUS", os.cpu_count()))
        env["LP_NUM_THREADS"] = "0" if family == "wine" else str(cpus)
        context = {
            "uname": list(os.uname()), "family": family, "row": row,
            "adapter": adapter, "reference": None if record else reference,
            "environment": {k: v for k, v in env.items() if k.startswith((
                "VK", "DXVK", "PROTON", "LP_", "MESA_", "GALLIUM", "GRAPHICS"))},
            "display": "Xvfb -noreset 1280x800x24", "cache": "empty private directory",
            "iommu": Path("/proc/cmdline").read_text(),
        }
        (out / "environment.json").write_text(json.dumps(context, indent=2) + "\n")
        print("d3d: environment: " + json.dumps(context), flush=True)
        proton = Path(env.get("D3D_PROTON", "/opt/proton"))
        launch = [sys.executable, str(proton / "proton")]
        if family in ("probe", "wine"):
            server = proton / "files/bin/wineserver"
            print("d3d: prefix: creating", flush=True)
            status, _ = run_command(launch + ["run", "c:\\windows\\system32\\cmd.exe", "/c", "exit", "0"],
                                    env, out / "prefix.log")
            if status:
                raise RuntimeError("Proton prefix creation failed: %s" % status)
            print("d3d: prefix: ready", flush=True)
            env["WINEPREFIX"] = str(work / "compat/pfx")
            # A probe's row is N, on Proton, or PATH/N; a unit's PATH/EXE:NAME.
            path, _, tail = row.rpartition("/") if family == "probe" else row.partition("/")
            path = path or "proton"
            if path not in PATHS:
                raise RuntimeError("unknown Wine path")
            kind, renderer = PATHS[path]
            env["WINEDLLOVERRIDES"] = ";".join(
                name + "=" + kind for name in ("d3d9", "d3d10core", "d3d11", "dxgi", "d3d12", "d3d12core"))
            env["WINE_D3D_CONFIG"] = "renderer=" + renderer
            if family == "probe":
                if tail not in ("9", "11", "12"):
                    raise RuntimeError("unknown probe")
                pixel = out / "pixels.txt"
                env["PROBE_OUT"] = "Z:" + str(pixel)
                rc, _ = run_command(launch + ["runinprefix", str(HERE / "probes" / ("d3d" + tail + ".exe"))],
                                    env, out / "probe.log")
                text = pixel.read_text() if pixel.exists() else ""
                named = adapter_name(text)
                # A reference that names the adapter requires the same one here.
                expected = ref.get("probe", {}).get(row, {})
                results[row] = {"exit": rc, "pixels": text, "adapter": named,
                                "matched": record or (rc == 0 and ("D3D" + tail + " OK") in text.splitlines() and
                                                      expected.get("adapter", named) == named)}
            else:
                unit = tail
                expected = {} if record else ref["wine"][path][unit]
                exe, name = unit.split(":")
                # Recording stops WineD3D's DXGI unit where it stalls on GL.
                stall = expected.get("progress") or (record and path != "proton" and unit == "dxgi:dxgi")
                rc, stopped = run_command(launch + ["runinprefix", str(HERE / "wine" / (exe + "_test.exe")), name],
                                          env, out / "unit.log", STALL if stall else None)
                text = (out / "unit.log").read_text(errors="replace")
                counts = summaries(text, WINE_SUMMARY % re.escape(name))
                lines = len(re.findall(TEST_LINE, text, re.M))
                actual = {"exit": rc, "summary": counts, "progress": stopped, "lines": lines}
                # A null exit is one that Linux's runs did not agree on.
                actual["matched"] = record or (stopped if expected.get("progress") else (
                    expected["exit"] in (None, rc) and (counts is not None) == expected["summary"] and
                    (lines >= expected.get("min_lines", 0) if counts is None else
                     (counts[3] == expected["failures"] and
                      counts[0] >= expected["min_checks"] and counts[4] <= expected["max_skipped"]))))
                results[unit] = actual
        elif family == "vkd3d":
            env["LD_LIBRARY_PATH"] = str(HERE / "vkd3d") + ":" + env.get("LD_LIBRARY_PATH", "")
            names = tests
            if row.isdigit() and int(row) < 8:
                part = int(row)
                names = (["test_create_device"] if part == 0 else
                         [n for n in names if n != "test_create_device"][part - 1::7])
            elif row in names:
                names = [row]
            else:
                raise RuntimeError("unknown vkd3d part or test")
            jobs = min(4, cpus)
            def unit(name):
                unit_env = env.copy()
                unit_env["VKD3D_TEST_MATCH"] = name
                unit_env["VKD3D_SHADER_CACHE_PATH"] = "0"
                unit_env["VKD3D_TEST_DEBUG"] = "1"
                rc, _ = run_command([str(HERE / "vkd3d/d3d12")], unit_env, out / (name + ".log"))
                text = (out / (name + ".log")).read_text(errors="replace")
                counts = summaries(text, VK_SUMMARY)
                if record:
                    result = {"exit": rc, "summary": counts, "begun": name + " begin" in text,
                              "ended": name + " end" in text, "matched": True}
                    print("d3d: unit: %s %s" % (name, json.dumps(result)), flush=True)
                    return name, result
                expected = ref["vkd3d"][name]
                # The discovery's SIGKILLs are memory exhaustion, never flakes.
                flake = (rc in (-signal.SIGSEGV, 128 + signal.SIGSEGV) and
                         name in ref["flakes"] and name + " begin" in text)
                matched = flake or (counts is not None and name + " begin" in text and
                                    name + " end" in text and rc in (0, 1) and
                                    counts[1] in expected["failures"] and counts[0] >= expected["min_checks"] and
                                    counts[3] <= expected["max_skipped"] and (rc == 0) == (counts[1] == 0))
                result = {"exit": rc, "summary": counts, "linux_flake": flake, "matched": matched}
                print("d3d: unit: %s %s" % (name, json.dumps(result)), flush=True)
                return name, result
            with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
                results.update(pool.map(unit, names))
        else:
            raise RuntimeError("unknown family")
        (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        for name, result in results.items():
            if family != "vkd3d":
                print("d3d: unit: %s %s" % (name, json.dumps(result)), flush=True)
        if not results or not all(v["matched"] for v in results.values()):
            return 1
        print("D3DROW %s %s %s" % ("RECORDED" if record else "OK", family, row), flush=True)
        return 0
    finally:
        if server and work:
            clean = env.copy()
            clean["WINEPREFIX"] = str(work / "compat/pfx")
            subprocess.run([str(server), "-k"], env=clean, check=False)
            subprocess.run([str(server), "-w"], env=clean, check=False)
        if xvfb:
            xvfb.terminate()
            xvfb.wait()
        if work:
            shutil.rmtree(work)


if __name__ == "__main__":
    for stop_signal in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(stop_signal, interrupted)
    sys.exit(main())
