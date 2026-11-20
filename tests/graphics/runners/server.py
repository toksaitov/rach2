"""server.py: the browser runners' web server, on a free port of the
loopback address.

Usage: python3 -I server.py EVENTS PORTFILE ROOT [NAME=PATH]...

Serves ROOT at / and each PATH at /NAME, a directory's files below it,
writes the port it took to PORTFILE once it listens, and appends one JSON
object a line to EVENTS: a `fetch` for each page fetched, the run's sign of
progress, and the body of each POST to /event, which the runners' pages
send as each result is known. A POSTed object whose `threads` is true
gets, before the reply, the run and wait counts of every thread on the
machine, `threads` then mapping each thread id to [process name, thread
name, run ns, wait for a processor ns, nice value, scheduling policy, user
ms, system ms, times run] from /proc's schedstat and stat and
sched_getscheduler, `cpus` mapping each processor's number to its [busy ms,
idle ms] from /proc/stat, busy being all but idle and iowait, and `snap`
the monotonic seconds at the snapshot's start and end; the page waits for
the reply, so the snapshot falls outside what it times.
A round mark also gets Rach2's cumulative bkl/calls text and the read's
monotonic start and end in nanoseconds. The calls snapshot is last at a
round's start and first at its end to exclude the thread walks. If
PROBE_CALLS_SOCKET is set, read a fresh copy from that Unix socket.
The events file is flushed after each line, so that a browser that crashes
loses only the result it had not sent.
"""
import http.server
import json
import os
import socket
import sys
import threading
import time
import urllib.parse

events_path, port_path, root = sys.argv[1], sys.argv[2], sys.argv[3]
mounts = {}
for arg in sys.argv[4:]:
    name, _, path = arg.partition("=")
    mounts["/" + name.strip("/")] = path
lock = threading.Lock()
events = open(events_path, "a", encoding="utf-8")


def record(event):
    event["t"] = round(time.monotonic(), 3)
    with lock:
        events.write(json.dumps(event, sort_keys=True) + "\n")
        events.flush()


TICK_MS = 1000.0 / os.sysconf("SC_CLK_TCK")


def cpus():
    """Each processor's busy and idle milliseconds since boot, by its
    number, from /proc/stat's cpuN lines."""
    found = {}
    with open("/proc/stat", encoding="ascii") as f:
        for line in f:
            words = line.split()
            if not words or not words[0].startswith("cpu") or words[0] == "cpu":
                continue
            ticks = [int(w) for w in words[1:9]]
            idle = ticks[3] + ticks[4]
            found[words[0][3:]] = [round((sum(ticks) - idle) * TICK_MS), round(idle * TICK_MS)]
    return found


def threads():
    """Each thread's schedstat on the machine, by thread id, as the module's
    docstring gives it; a thread that ends during the walk is left out."""
    found = {}
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open("/proc/%s/comm" % pid, encoding="utf-8", errors="replace") as f:
                pname = f.read().strip()
            tids = os.listdir("/proc/%s/task" % pid)
        except OSError:
            continue
        for tid in tids:
            base = "/proc/%s/task/%s/" % (pid, tid)
            try:
                with open(base + "schedstat", encoding="ascii") as f:
                    run, wait, runs = f.read().split()[:3]
                with open(base + "comm", encoding="utf-8", errors="replace") as f:
                    name = f.read().strip()
                with open(base + "stat", encoding="utf-8", errors="replace") as f:
                    stat = f.read().rpartition(")")[2].split()
                nice = int(stat[16])
                user, system = int(stat[11]) * TICK_MS, int(stat[12]) * TICK_MS
            except (OSError, ValueError, IndexError):
                continue
            try:
                policy = os.sched_getscheduler(int(tid))
            except OSError:
                policy = -1
            found[tid] = [pname, name, int(run), int(wait), nice, policy, round(user), round(system), int(runs)]
    return found


def round_calls(event):
    """Record the calls sample or the error that prevented it."""
    start = time.monotonic_ns()
    try:
        path = os.environ.get("PROBE_CALLS_SOCKET")
        if path:
            with socket.socket(socket.AF_UNIX) as client:
                client.settimeout(2)
                client.connect(path)
                with client.makefile(encoding="ascii") as f:
                    event["calls"] = f.read()
        else:
            with open("/sys/kernel/debug/bkl/calls", encoding="ascii") as f:
                event["calls"] = f.read()
    except OSError as e:
        event["calls_error"] = str(e)
    event["calls_snap_ns"] = [start, time.monotonic_ns()]


class Handler(http.server.SimpleHTTPRequestHandler):
    def translate_path(self, path):
        clean = urllib.parse.urlparse(path).path
        for name, base in mounts.items():
            if clean == name and not os.path.isdir(base):
                return base
            if clean.startswith(name + "/"):
                rel = urllib.parse.unquote(clean[len(name) + 1:])
                return os.path.join(base, os.path.normpath("/" + rel).lstrip("/"))
        rel = urllib.parse.unquote(clean)
        return os.path.join(root, os.path.normpath("/" + rel).lstrip("/"))

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        clean = urllib.parse.urlparse(self.path).path
        if clean.endswith(".html"):
            record({"kind": "fetch", "path": clean})
        super().do_GET()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        try:
            event = json.loads(body)
        except ValueError:
            event = {"kind": "garbled", "body": body.decode("utf-8", "replace")[:200]}
        if not isinstance(event, dict):
            event = {"kind": "garbled", "body": str(event)[:200]}
        if event.get("threads") is True:
            round_mark = event.get("kind") == "mark" and "round" in event
            if round_mark and event.get("at") == "end":
                round_calls(event)
            t0 = time.monotonic()
            event["threads"] = threads()
            event["cpus"] = cpus()
            event["snap"] = [round(t0, 4), round(time.monotonic(), 4)]
            if round_mark and event.get("at") == "start":
                round_calls(event)
        record(event)
        self.send_response(204)
        self.end_headers()

    def log_message(self, fmt, *args):
        pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
with open(port_path + ".part", "w", encoding="ascii") as f:
    f.write("%d\n" % server.server_address[1])
os.rename(port_path + ".part", port_path)
server.serve_forever()
