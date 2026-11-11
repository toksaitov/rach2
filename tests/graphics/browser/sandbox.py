"""Read sandboxed children's proc files as root while the ordinary browser lives."""
import json
import os
from pathlib import Path
import pwd
import shlex
import signal
import subprocess
import sys
import time

from run import stop


def snapshot(children):
    """Read process state, comparing namespace identities with the browser parent's."""
    parent = next(p["id"] for p in children if p["type"].lower() == "browser")
    result = []
    for child in children:
        pid = child["id"]
        proc = Path("/proc/%d" % pid)
        try:
            args = (proc / "cmdline").read_bytes().replace(b"\0", b" ").decode()
            role = ("storage" if "StorageService" in args else "network" if "NetworkService" in args
                    else "renderer" if "--type=renderer" in args else "gpu" if "--type=gpu-process" in args else None)
            if role is None:
                continue
            status = dict(line.split(":", 1) for line in (proc / "status").read_text().splitlines() if ":" in line)
            result.append({"role": role, "pid": pid, "argv": args,
                "seccomp": int(status["Seccomp"].strip()),
                "namespaces": {name: os.readlink(proc / "ns" / name) != os.readlink(
                    Path("/proc/%d/ns" % parent) / name) for name in ("user", "pid", "net", "mnt")}})
        except FileNotFoundError:
            continue
    return result


def main():
    """Start the browser as the selected ordinary user and answer one snapshot request.

    The arguments are the user, the browser, the output directory and the
    runner's mode, `sandbox` when it is left out, `gpu` for the GPU
    profile, or `webgpu-hasvk` on the caller's DRM Weston.
    """
    user, browser, out_name = sys.argv[1:4]
    mode = sys.argv[4] if len(sys.argv) > 4 else "sandbox"
    assert mode in ("sandbox", "gpu", "webgpu-hasvk"), mode
    assert os.getuid() == 0, "sandbox proc reader needs root"
    out = Path(out_name).resolve()
    out.mkdir(parents=True, exist_ok=True)
    account = pwd.getpwnam(user)
    os.chown(out, account.pw_uid, account.pw_gid)
    here = Path(__file__).resolve().parent
    kind = "performance" if mode == "webgpu-hasvk" else "conformance"
    command = ("export GRAPHICS_CLASS=%s GRAPHICS_DONE=1; . %s && sh %s %s %s %s" %
               (kind, shlex.quote(str(here / "../guest-env.sh")),
                shlex.quote(str(here / "run.sh")), shlex.quote(browser),
                shlex.quote(mode), shlex.quote(str(out))))
    player = None
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda *_: sys.exit(1))
    try:
        player = subprocess.Popen(["su", "-s", "/bin/sh", user, "-c", command], start_new_session=True)
        request = out / "process-ids.json"
        while not request.exists() and player.poll() is None:
            time.sleep(0.1)
        if request.exists():
            result = snapshot(json.loads(request.read_text()))
            part = out / "process-state.tmp"
            part.write_text(json.dumps(result))
            part.replace(out / "process-state.json")
        assert player.wait() == 0, "sandbox runner failed"
    finally:
        stop(player)


if __name__ == "__main__":
    main()
