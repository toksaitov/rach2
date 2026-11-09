"""server.py: the browser runners' web server, on a free port of the
loopback address.

Usage: python3 -I server.py EVENTS PORTFILE ROOT [NAME=PATH]...

Serves ROOT at / and each PATH at /NAME, a directory's files below it,
writes the port it took to PORTFILE once it listens, and appends one JSON
object a line to EVENTS: a `fetch` for each page fetched, the run's sign of
progress, and the body of each POST to /event, which the runners' pages
send as each result is known.
The events file is flushed after each line, so that a browser that crashes
loses only the result it had not sent.
"""
import http.server
import json
import os
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
