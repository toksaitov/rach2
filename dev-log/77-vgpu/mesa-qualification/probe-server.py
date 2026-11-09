# probe-server.py PORT OUT: serve probe.html from this directory and write the
# first JSON posted to /result into OUT, then exit.
import http.server, os, sys
os.chdir(os.path.dirname(os.path.abspath(__file__)))
class H(http.server.SimpleHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        with open(sys.argv[2], 'wb') as f:
            f.write(body + b'\n')
        self.send_response(204)
        self.end_headers()
        self.server.done = True
    def log_message(self, *a):
        pass
s = http.server.HTTPServer(('127.0.0.1', int(sys.argv[1])), H)
s.done = False
while not s.done:
    s.handle_request()
