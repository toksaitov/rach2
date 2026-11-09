"""probe-server.py: serve the probe page and save each posted result as
NAME.json, NAME from ?name= or else the path's last part.

Usage: python3 -I probe-server.py ROOT OUTDIR PORT
"""
import http.server
import os
import sys
import urllib.parse

root, out, port = sys.argv[1], sys.argv[2], int(sys.argv[3])


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=root, **k)

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        # The WebGL suite's harness posts to /start and /finish with no name.
        base = os.path.basename(u.path) or "unnamed"
        name = "".join(ch for ch in q.get("name", [base])[0]
                       if ch.isalnum() or ch in "-_")
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        with open(os.path.join(out, name + ".json"), "wb") as f:
            f.write(body)
        self.send_response(204)
        self.end_headers()


http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
