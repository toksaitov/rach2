"""cdp.py: run Chromium with its DevTools protocol on a pipe and keep its
media players' events, so that a run names the decoder Chromium chose.

Usage: python3 -I cdp.py EVENTS URL CHROMIUM [ARG]...

Starts CHROMIUM with --remote-debugging-pipe, which reads the protocol's
messages, each ended by a NUL, from descriptor 3 and writes its own to
descriptor 4. It attaches to each page and frame as it appears, enables the
Media domain there, opens URL in the first page once Media is on, so that no
player's first events are missed, and appends each Media event to EVENTS as
one JSON object a line: playerPropertiesChanged carries kVideoDecoderName
and kIsPlatformVideoDecoder. Exits with Chromium's status when it ends.
"""
import json
import os
import subprocess
import sys
import threading

if len(sys.argv) < 4:
    sys.exit("cdp: usage: cdp.py EVENTS URL CHROMIUM [ARG]...")
events_path, url = sys.argv[1], sys.argv[2]
argv = sys.argv[3:] + ["--remote-debugging-pipe", "about:blank"]

to_chromium_r, to_chromium_w = os.pipe()
from_chromium_r, from_chromium_w = os.pipe()


def descriptors():
    # The pipe's ends may already sit at 3 or 4; move them clear first.
    a = os.dup(to_chromium_r)
    b = os.dup(from_chromium_w)
    hi_a, hi_b = 100, 101
    os.dup2(a, hi_a)
    os.dup2(b, hi_b)
    os.dup2(hi_a, 3)
    os.dup2(hi_b, 4)


child = subprocess.Popen(argv, preexec_fn=descriptors, pass_fds=(3, 4))
os.close(to_chromium_r)
os.close(from_chromium_w)
out = os.fdopen(to_chromium_w, "wb", buffering=0)
events = open(events_path, "a", encoding="utf-8")
lock = threading.Lock()
serial = [0]
attached = set()
navigated = [False]


def send(method, params=None, session=None):
    with lock:
        serial[0] += 1
        message = {"id": serial[0], "method": method, "params": params or {}}
        if session is not None:
            message["sessionId"] = session
        try:
            out.write(json.dumps(message).encode() + b"\0")
        except OSError:
            pass
        return serial[0]


def attach(info):
    if info.get("type") in ("page", "iframe") and info["targetId"] not in attached:
        attached.add(info["targetId"])
        send("Target.attachToTarget", {"targetId": info["targetId"], "flatten": True})


pending = {}
send("Target.setDiscoverTargets", {"discover": True})
buf = b""
reader = os.fdopen(from_chromium_r, "rb", buffering=0)
while True:
    chunk = reader.read(65536)
    if not chunk:
        break
    buf += chunk
    while b"\0" in buf:
        raw, buf = buf.split(b"\0", 1)
        try:
            msg = json.loads(raw)
        except ValueError:
            continue
        method = msg.get("method", "")
        params = msg.get("params", {})
        if method in ("Target.targetCreated", "Target.targetInfoChanged"):
            attach(params["targetInfo"])
        elif method == "Target.attachedToTarget":
            session = params["sessionId"]
            ident = send("Media.enable", session=session)
            if params["targetInfo"].get("type") == "page" and not navigated[0]:
                navigated[0] = True
                pending[ident] = session
            send("Runtime.runIfWaitingForDebugger", session=session)
        elif msg.get("id") in pending:
            session = pending.pop(msg["id"])
            send("Page.navigate", {"url": url}, session=session)
        elif method.startswith("Media."):
            events.write(json.dumps({"method": method, "params": params}) + "\n")
            events.flush()
sys.exit(child.wait())
