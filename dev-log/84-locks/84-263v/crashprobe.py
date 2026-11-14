"""Read live Chromium children as crashpad does, then crash two and keep crashpad's dumps.

Run as root beside tests/graphics/browser/run.py, as sandbox.py runs:

    crashprobe.py USER chromium OUT

It compiles crashread.c, starts the sandbox case's browser as USER, and once
run.py has written process-ids.json, runs crashread against the GPU process,
a renderer, the network service and the browser, keeping each one's maps and
status. Then it makes the main thread of the GPU process and of the renderer,
one at a time, jump to an address no region holds (crashread -f), times each
until it has ended, and keeps the minidumps crashpad wrote, under the profile,
/tmp or the user's home, with a summary of their streams. The browser is
stopped at the end; the probe's record is OUT/crashprobe.json.
"""
import contextlib
import json
import os
from pathlib import Path
import pwd
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import time

from run import stop

HERE = Path(__file__).resolve().parent
TOOL = Path("/tmp/crashread")


def role(cmdline):
    """Name a Chromium process by its switches."""
    if "chrome_crashpad_handler" in cmdline.split(" ", 1)[0] or "--type=crashpad-handler" in cmdline:
        return "crashpad"
    if "--type=gpu-process" in cmdline:
        return "gpu"
    if "--type=renderer" in cmdline:
        return "renderer"
    if "NetworkService" in cmdline:
        return "network"
    if "StorageService" in cmdline:
        return "storage"
    if "--type=" not in cmdline:
        return "browser"
    return "other"


def processes():
    """Every Chromium process by role, read from /proc."""
    found = {}
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            args = (entry / "cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace")
        except OSError:
            continue
        program = os.path.basename(args.split(" ", 1)[0])
        if program not in ("chromium", "chrome", "chrome_crashpad_handler"):
            continue
        found.setdefault(role(args), []).append({"pid": int(entry.name), "argv": args})
    return found


def ended(pid):
    """Whether `pid` has exited, reaped or not."""
    try:
        state = Path("/proc/%d/stat" % pid).read_text().rsplit(")", 1)[1].split()[0]
    except (OSError, IndexError):
        return True
    return state in ("Z", "X")


def utf16(data, rva):
    """A MINIDUMP_STRING at `rva`."""
    (length,) = struct.unpack_from("<I", data, rva)
    return data[rva + 4:rva + 4 + length].decode("utf-16-le", errors="replace")


def summary(path):
    """The streams of a minidump: threads with their context and stack sizes,
    modules, memory ranges and the exception record."""
    data = path.read_bytes()
    if data[:4] != b"MDMP":
        return {"bytes": len(data), "error": "not a minidump"}
    count, rva = struct.unpack_from("<II", data, 8)
    result = {"bytes": len(data), "streams": {}}
    for i in range(count):
        kind, size, where = struct.unpack_from("<III", data, rva + 12 * i)
        result["streams"]["%#x" % kind] = size
        if kind == 3:  # ThreadListStream
            (n,) = struct.unpack_from("<I", data, where)
            threads = []
            for t in range(n):
                fields = struct.unpack_from("<IIIIQQIIII", data, where + 4 + 48 * t)
                threads.append({"stack": fields[6], "context": fields[8]})
            result["threads"] = len(threads)
            result["threads_with_stack"] = sum(1 for t in threads if t["stack"])
            result["context_sizes"] = sorted({t["context"] for t in threads})
        elif kind == 4:  # ModuleListStream
            (n,) = struct.unpack_from("<I", data, where)
            names = []
            for m in range(n):
                name_rva = struct.unpack_from("<I", data, where + 4 + 108 * m + 20)[0]
                names.append(utf16(data, name_rva))
            result["modules"] = n
            result["module_names"] = names
        elif kind == 5:  # MemoryListStream
            (n,) = struct.unpack_from("<I", data, where)
            result["memory_ranges"] = n
            result["memory_bytes"] = sum(struct.unpack_from("<I", data, where + 4 + 16 * r + 8)[0]
                                         for r in range(n))
        elif kind == 6:  # ExceptionStream
            tid, _, code, flags, _, address = struct.unpack_from("<IIIIQQ", data, where)
            result["exception"] = {"code": code, "flags": flags, "address": hex(address)}
    return result


def dumps(roots):
    """Every minidump under `roots`, by path."""
    found = {}
    for root in roots:
        if root.exists():
            for path in root.rglob("*.dmp"):
                found[str(path)] = path
    return found


def main():
    """Start the browser as USER, read its children, crash two and keep the dumps."""
    user, browser, out_name = sys.argv[1:4]
    assert os.getuid() == 0, "crashprobe needs root"
    out = Path(out_name).resolve()
    out.mkdir(parents=True, exist_ok=True)
    account = pwd.getpwnam(user)
    os.chown(out, account.pw_uid, account.pw_gid)
    record = {"kernel": os.uname().release}
    yama = Path("/proc/sys/kernel/yama/ptrace_scope")
    record["ptrace_scope"] = yama.read_text().strip() if yama.exists() else None
    # What sets AT_MINSIGSTKSZ: the processor's XSAVE features and, on Linux,
    # the kernel's own account of the frame and the state's size.
    with contextlib.suppress(OSError):
        flags = next(line for line in Path("/proc/cpuinfo").read_text().splitlines()
                     if line.startswith("flags")).split(":", 1)[1].split()
        record["cpu_flags"] = sorted(f for f in flags if f in (
            "pku", "ospke", "xsave", "xsaveopt", "xsavec", "xsaves", "avx", "avx512f", "fsgsbase"))
    log = subprocess.run(["dmesg"], capture_output=True, text=True)
    record["kernel_xstate"] = [line for line in log.stdout.splitlines()
                               if "sigframe" in line or "xstate" in line or "x86/fpu" in line]
    cc = subprocess.run([shutil.which("gcc") or "cc", "-std=c99", "-O2", "-o", str(TOOL), str(HERE / "crashread.c")],
                        capture_output=True, text=True)
    record["compile"] = cc.returncode
    (out / "crashread-compile.txt").write_text(cc.stdout + cc.stderr)
    command = ("export GRAPHICS_CLASS=conformance GRAPHICS_DONE=1; . %s && sh %s %s sandbox %s" %
               (shlex.quote(str(HERE / "../guest-env.sh")), shlex.quote(str(HERE / "run.sh")),
                shlex.quote(browser), shlex.quote(str(out))))
    player = None
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda *_: sys.exit(1))
    try:
        player = subprocess.Popen(["su", "-s", "/bin/sh", user, "-c", command], start_new_session=True)
        request = out / "process-ids.json"
        while not request.exists() and player.poll() is None:
            time.sleep(0.1)
        assert request.exists(), "the browser wrote no process list"
        found = processes()
        record["processes"] = {k: [p["pid"] for p in v] for k, v in found.items()}
        browser_args = found["browser"][0]["argv"]
        profile = Path(browser_args.split("--user-data-dir=", 1)[1].split()[0])
        record["profile"] = str(profile)
        for name in ("gpu", "renderer", "network", "browser"):
            if name not in found:
                continue
            pid = found[name][0]["pid"]
            for what in ("maps", "status"):
                try:
                    shutil.copyfile("/proc/%d/%s" % (pid, what), out / ("%s-%s.txt" % (what, name)))
                except OSError as error:
                    record.setdefault("errors", []).append("%s %s: %r" % (name, what, error))
            with (out / ("crashread-%s.txt" % name)).open("w") as log:
                started = time.monotonic()
                done = subprocess.run([str(TOOL), str(pid)], stdout=log, stderr=subprocess.STDOUT,
                                      timeout=900)
                record.setdefault("crashread", {})[name] = {
                    "status": done.returncode, "seconds": round(time.monotonic() - started, 3)}
        roots = [profile, Path("/tmp"), Path(account.pw_dir)]
        for name in ("gpu", "renderer"):
            if name not in found:
                continue
            pid = found[name][0]["pid"]
            before = dumps(roots)
            started = time.monotonic()
            sent = subprocess.run([str(TOOL), "-f", str(pid)], capture_output=True, text=True)
            while not ended(pid) and time.monotonic() - started < 180:
                time.sleep(0.05)
            gone = time.monotonic() - started
            new = {}
            while not new and time.monotonic() - started < gone + 60:
                new = {k: v for k, v in dumps(roots).items() if k not in before}
                time.sleep(0.5)
            time.sleep(2)
            new = {k: v for k, v in dumps(roots).items() if k not in before}
            kept = []
            for path in new.values():
                target = out / "dumps" / ("%s-%s" % (name, path.name))
                target.parent.mkdir(exist_ok=True)
                shutil.copyfile(path, target)
                kept.append({"path": str(path), "summary": summary(target)})
            record.setdefault("crash", {})[name] = {
                "fault": sent.stdout.strip(), "pid": pid, "ended": ended(pid), "seconds_to_end": round(gone, 3), "dumps": kept}
            time.sleep(5)
        record["after"] = {k: [p["pid"] for p in v] for k, v in processes().items()}
    finally:
        # End the browser's own session first: su gives the runner 2 s after
        # SIGTERM, too little for run.py to stop the browser itself.
        for proc in processes().get("browser", []):
            with contextlib.suppress(ProcessLookupError, PermissionError):
                os.killpg(proc["pid"], signal.SIGTERM)
        started = time.monotonic()
        while processes() and time.monotonic() - started < 10:
            time.sleep(0.1)
        for procs in processes().values():
            for proc in procs:
                with contextlib.suppress(ProcessLookupError):
                    os.kill(proc["pid"], signal.SIGKILL)
        stop(player)
        (out / "crashprobe.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    print("crashprobe: result: done", flush=True)


if __name__ == "__main__":
    main()
