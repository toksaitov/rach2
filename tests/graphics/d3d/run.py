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


# Wine's word that a process has no desktop, which run_proton() runs again.
LOST = b'"The explorer process failed to start."'
STALL = "dxgi.c:8040: Test marked todo: d3d10: test 2: Got unexpected present count 1, expected 2."
# Each Vulkan device's ICDs and the Linux references taken on it, of the
# x86-64 programs and of the i386 ones.
ADAPTERS = {"lavapipe": ("lvp_icd*.json", "linux.json", "linux32.json"),
            "venus": ("venus_icd*.json", "venus.json", "venus32.json")}
# Each mode of an i386 probe: Proton's WoW64, its i386 program on the x86-64
# Wine, or its default, on the i386 Wine; and the Unix half of Wine the
# probe must find itself on.
MODES = {"wow64": "x86_64", "i386": "i386"}
# Each Wine path's DLL overrides, native or builtin, and WineD3D's renderer.
PATHS = {"proton": ("n", "gl"), "wined3d": ("b", "gl"), "wined3dvk": ("b", "vulkan")}


def stall_of(expected):
    """Return the line and its count at which Linux's run of a unit stalled, or None.

    A reference's progress is a dict of the line and the count, or, in
    linux.json, the text of WineD3D's DXGI stall, whose line is STALL once.
    """
    progress = expected.get("progress")
    if isinstance(progress, dict):
        return progress["line"], progress["count"]
    return (STALL, 1) if progress else None


def run_command(argv, env, log, stop=None, lost=()):
    """Wait for the command itself, retaining output even if a descendant keeps it open.

    stop is a line and a count: the command is stopped when its output has
    had the line that many times, where Linux's run of it stalled. lost
    names the files besides the log in which LOST, Wine's word that it has
    no desktop, stops the command too. Returns the exit, whether stop
    stopped it and whether LOST did.
    """
    stopped = gone = False
    with log.open("wb") as dest:
        proc = subprocess.Popen(argv, env=env, stdout=dest,
                                stderr=subprocess.STDOUT, start_new_session=True)
        CHILDREN.add(proc)
        try:
            if stop or lost:
                marker = stop[0].encode() if stop else None
                pending = b""
                seen = 0
                watched = {path: 0 for path in (log,) + tuple(lost)}
                with log.open("rb") as source:
                    while proc.poll() is None:
                        if marker:
                            pending = pending + source.read()
                            seen += pending.count(marker)
                            if seen >= stop[1]:
                                stopped = True
                                os.killpg(proc.pid, signal.SIGKILL)
                                break
                            # Too short to hold a line already counted.
                            pending = pending[-(len(marker) - 1):]
                        if lost and any(LOST in tail for tail in tails(watched)):
                            gone = True
                            os.killpg(proc.pid, signal.SIGKILL)
                            break
                        # Poll output; elapsed time decides no verdict.
                        time.sleep(0.02)
            proc.wait()
            (log.with_suffix(log.suffix + ".exit.json")).write_text(
                json.dumps({"pid": proc.pid, "exit": proc.returncode,
                            "progress": stopped, "lost": gone}) + "\n")
            return proc.returncode, stopped, gone
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
            CHILDREN.discard(proc)


def tails(watched):
    """Yield what each watched file has gained since its last read, with a line's overlap.

    watched maps a path to the offset read to.
    """
    for path in watched:
        start = max(0, watched[path] - len(LOST))
        try:
            with path.open("rb") as source:
                source.seek(start)
                data = source.read()
        except FileNotFoundError:
            continue
        watched[path] = start + len(data)
        yield data


def run_proton(argv, env, log, out, work, stop=None):
    """Run a Proton command, starting it again when Wine loses its desktop.

    Wine starts explorer.exe for its desktop and waits 10 s for its window;
    explorer later than that leaves the command's process with no display
    driver, logging LOST, and Proton's steam.exe waiting for ever. A command
    so lost, its stderr in the log or in Proton's log in out, is stopped,
    its Wine server ended and its prefix, when it was making one, removed,
    and it runs again, three times at most. Returns run_command's exit and
    stop.
    """
    making = "WINEPREFIX" not in env
    steam = out / ("steam-%s.log" % env["SteamGameId"])
    for attempt in range(1, 4):
        # Proton removes its log at each launch; removed here first, what
        # the file then holds is this launch's.
        if steam.exists():
            steam.unlink()
        rc, stopped, gone = run_command(argv, env, log, stop, (steam,))
        if not gone:
            return rc, stopped
        print("d3d: desktop: lost on try %d; explorer.exe was late" % attempt, flush=True)
        for kept in (log, steam):
            if kept.exists():
                shutil.copy(kept, kept.with_suffix(kept.suffix + ".lost%d" % attempt))
        clean = env.copy()
        clean["WINEPREFIX"] = str(work / "compat/pfx")
        subprocess.run([str(Path(env.get("D3D_PROTON", "/opt/proton")) / "files/bin/wineserver"), "-k"],
                       env=clean, check=False)
        if making:
            shutil.rmtree(work / "compat")
            (work / "compat").mkdir()
    raise RuntimeError("Wine lost its desktop three times")


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
    alone the loader sees and whose Linux reference judges the row. A probe's
    ROW is [PATH/]N[-MODE]: the Direct3D N probe on the Wine path PATH, its
    x86-64 build, or with MODE its i386 build in that mode of MODES, judged
    by the adapter's reference of i386 programs, which must have the row.
    -R runs the row with no reference and records each unit, as the
    references are taken; it fails only when a unit cannot be run.
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
    mode = row.rpartition("/")[2].partition("-")[2] if family == "probe" else ""
    if mode and mode not in MODES:
        raise SystemExit(usage)
    if os.geteuid() == 0:
        raise SystemExit("d3d: run as an ordinary user")
    out = Path(output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    if family == "private" and row == "files":
        status, _, _ = run_command([str(HERE / "privatefilemusl"), "/tmp"],
                                   os.environ.copy(), out / "private.log")
        for line in (out / "private.log").read_text(errors="replace").splitlines():
            if not line.startswith("@etude "):
                print(line, flush=True)
        if status:
            raise SystemExit(status)
        print("D3DROW OK private files", flush=True)
        return
    pattern, reference64, reference32 = ADAPTERS[adapter]
    reference = reference32 if mode else reference64
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
    if mode == "i386":
        # Proton's default mode runs an i386 program on its i386 Wine.
        del env["PROTON_USE_WOW64"]
    icds = sorted(Path("/usr/share/vulkan/icd.d").glob(pattern))
    files = [p for p in icds if "i686" not in p.name]
    if len(files) != 1:
        raise SystemExit("d3d: expected one x86-64 %s ICD" % adapter)
    if mode == "i386":
        # The i386 loader passes over the x86-64 ICD and loads this one.
        files += [p for p in icds if "i686" in p.name]
        if len(files) != 2:
            raise SystemExit("d3d: expected one i686 %s ICD" % adapter)
    env["VK_DRIVER_FILES"] = ":".join(str(p) for p in files)
    env["VK_ICD_FILENAMES"] = env["VK_DRIVER_FILES"]
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
            "adapter": adapter, "mode": mode or None, "reference": None if record else reference,
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
            status, _ = run_proton(launch + ["run", "c:\\windows\\system32\\cmd.exe", "/c", "exit", "0"],
                                   env, out / "prefix.log", out, work)
            if status:
                raise RuntimeError("Proton prefix creation failed: %s" % status)
            print("d3d: prefix: ready", flush=True)
            env["WINEPREFIX"] = str(work / "compat/pfx")
            # A probe's row is [PATH/]N[-MODE], on Proton without PATH; a
            # unit's PATH/EXE:NAME.
            path, _, tail = row.rpartition("/") if family == "probe" else row.partition("/")
            path = path or "proton"
            if path not in PATHS:
                raise RuntimeError("unknown Wine path")
            kind, renderer = PATHS[path]
            env["WINEDLLOVERRIDES"] = ";".join(
                name + "=" + kind for name in ("d3d9", "d3d10core", "d3d11", "dxgi", "d3d12", "d3d12core"))
            env["WINE_D3D_CONFIG"] = "renderer=" + renderer
            if family == "probe":
                n = tail.partition("-")[0]
                if n not in ("9", "11", "12"):
                    raise RuntimeError("unknown probe")
                expected = ref.get("probe", {}).get(row, {})
                if mode and not record and not expected:
                    raise RuntimeError("%s has no row %s, which Linux did not run" % (reference, row))
                pixel = out / "pixels.txt"
                env["PROBE_OUT"] = "Z:" + str(pixel)
                exe = "d3d" + n + ("-32" if mode else "") + ".exe"
                rc, _ = run_proton(launch + ["runinprefix", str(HERE / "probes" / exe)],
                                   env, out / "probe.log", out, work)
                text = pixel.read_text() if pixel.exists() else ""
                named = adapter_name(text)
                lines = text.splitlines()
                # An i386 probe must run on the Unix half its mode gives.
                side = not mode or ("wine-unix: " + MODES[mode]) in lines
                # A reference that names the adapter requires the same one here.
                results[row] = {"exit": rc, "pixels": text, "adapter": named,
                                "matched": record or (rc == 0 and ("D3D" + n + " OK") in lines and side and
                                                      expected.get("adapter", named) == named)}
            else:
                unit = tail
                expected = {} if record else ref["wine"][path][unit]
                exe, name = unit.split(":")
                # Recording stops WineD3D's DXGI unit where it stalls on GL.
                stop = stall_of(expected)
                if record and path != "proton" and unit == "dxgi:dxgi":
                    stop = (STALL, 1)
                rc, stopped = run_proton(launch + ["runinprefix", str(HERE / "wine" / (exe + "_test.exe")), name],
                                         env, out / "unit.log", out, work, stop)
                text = (out / "unit.log").read_text(errors="replace")
                counts = summaries(text, WINE_SUMMARY % re.escape(name))
                lines = len(re.findall(TEST_LINE, text, re.M))
                actual = {"exit": rc, "summary": counts, "progress": stopped, "lines": lines}
                # A null exit is one that Linux's runs did not agree on.
                actual["matched"] = record or ((stopped or counts is not None) if stop else (
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
                expected = {} if record else ref["vkd3d"][name]
                stop = stall_of(expected)
                rc, stopped, _ = run_command([str(HERE / "vkd3d/d3d12")], unit_env, out / (name + ".log"), stop)
                text = (out / (name + ".log")).read_text(errors="replace")
                counts = summaries(text, VK_SUMMARY)
                if record:
                    result = {"exit": rc, "summary": counts, "begun": name + " begin" in text,
                              "ended": name + " end" in text, "matched": True}
                    print("d3d: unit: %s %s" % (name, json.dumps(result)), flush=True)
                    return name, result
                # The discovery's SIGKILLs are memory exhaustion, never flakes.
                flake = (rc in (-signal.SIGSEGV, 128 + signal.SIGSEGV) and
                         name in ref["flakes"] and name + " begin" in text)
                # Running to its end where Linux stalled is no worse.
                ended = counts is not None and name + " end" in text
                matched = (stopped or ended) if stop else flake or (counts is not None and name + " begin" in text and
                                    name + " end" in text and rc in (0, 1) and
                                    counts[1] in expected["failures"] and counts[0] >= expected["min_checks"] and
                                    counts[3] <= expected["max_skipped"] and (rc == 0) == (counts[1] == 0))
                result = {"exit": rc, "summary": counts, "linux_flake": flake, "progress": stopped, "matched": matched}
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
