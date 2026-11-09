#!/usr/bin/python3
"""Drive the boot session on a display: its output, GL, modes, death and input.

Each subcommand prints `desktop: result: ...` lines that the desktop family's
scenario judges, and `desktop: ready: ...` where the scenario must act on the
machine, by QMP, before the subcommand goes on.
"""
import http.server
import os
from pathlib import Path
import re
import signal
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

SERVICES = Path("/var/log/services")
INI = Path("/etc/xdg/weston/weston.ini")
SUPERVISOR = Path("/run/rach2-desktop.pid")
ENV = ("export XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 "
       "PULSE_SERVER=unix:/run/user/1001/pulse/native XDG_SESSION_TYPE=wayland; ")


def desktop(line):
    """Form a command under the desktop user's groups and session environment."""
    return ["su", "-s", "/bin/bash", "desktop", "-c", ENV + line]


def westons():
    """Give the process ids of the running compositors."""
    found = []
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                if (entry / "comm").read_text().strip() == "weston":
                    found.append(int(entry.name))
            except (FileNotFoundError, ProcessLookupError):
                continue
    return found


def log_from(offset):
    """Give the services log from byte `offset`."""
    with SERVICES.open("rb") as f:
        f.seek(offset)
        return f.read().decode(errors="replace")


def log_size():
    """Give the services log's length, the offset of what comes next."""
    return SERVICES.stat().st_size


def drm_output(offset):
    """Wait for the compositor started past `offset` to light an output on DRM.

    Give its renderer, output name and current mode from Weston's log.
    """
    while True:
        text = log_from(offset)
        renderer = re.search(r"Using (Pixman|GL) renderer", text)
        if renderer is None and "Chosen EGL config details" in text:
            renderer = re.search(r"(GL)", "GL")
        output = re.search(r"Output (\S+) \(crtc \d+\) video modes:\n((?:.*\n)*?)\S", text)
        if renderer and output:
            current = re.search(r"^\s*(\d+x\d+)@[\d.]+.*\bcurrent\b", output.group(2), re.M)
            if current:
                return renderer.group(1), output.group(1), current.group(1)
        failed = re.search(r"fatal: .*|failed to create backend.*|Segmentation fault.*", text)
        assert failed is None, text[-4000:]
        time.sleep(0.2)


def sockets():
    """Wait until the compositor's socket accepts a connection."""
    path = "/run/user/1001/wayland-0"
    while True:
        if Path(path).is_socket() and westons():
            with socket.socket(socket.AF_UNIX) as s:
                try:
                    s.connect(path)
                    return
                except OSError:
                    pass
        time.sleep(0.2)


def console_back(count):
    """Wait until the kernel log names the console's take-back `count` times."""
    while True:
        log = subprocess.check_output(["dmesg"], text=True, errors="replace")
        found = re.findall(r"console: card\d+: \d+x\d+, the font at \d+x", log)
        if len(found) >= count:
            return found[-1]
        time.sleep(0.2)


def takebacks():
    """Count the console's take-backs so far."""
    log = subprocess.check_output(["dmesg"], text=True, errors="replace")
    return len(re.findall(r"console: card\d+: \d+x\d+, the font at \d+x", log))


def ended(offset):
    """Wait for the supervisor to record the session's end past `offset`."""
    while "rach2-desktop: session: ended" not in log_from(offset):
        time.sleep(0.2)


def restart():
    """Start the session again through the supervisor and wait for its output."""
    offset = log_size()
    os.kill(int(SUPERVISOR.read_text()), signal.SIGHUP)
    result = drm_output(offset)
    sockets()
    return result


def kill():
    """Kill Weston with SIGKILL and wait for the console to take the screen."""
    before = takebacks()
    offset = log_size()
    pids = westons()
    assert len(pids) == 1, pids
    os.kill(pids[0], signal.SIGKILL)
    ended(offset)
    return console_back(before + 1)


def cmd_drm():
    """Report the boot session's output on DRM."""
    renderer, output, mode = drm_output(0)
    sockets()
    print("desktop: result: drm: %s renderer, output %s, mode %s" % (renderer.lower(), output,
                                                                    mode), flush=True)


def cmd_kill():
    """Kill the compositor and wait for the console to take the screen."""
    line = kill()
    print("desktop: result: killed: %s" % line, flush=True)


def capture(program):
    """Give the CRC-32 of the frame the first CRTC scans out, as drmcapture
    reads it."""
    line = subprocess.check_output([program], text=True).strip()
    found = re.search(r"crc32 (0x[0-9a-f]+)", line)
    assert found, line
    return found.group(1), line


def cmd_console(program):
    """Compare the console's frame before Weston starts and after its SIGKILL.

    The session started at boot is killed first, so that the console has the
    screen; the session is started again and killed again. The console
    mirrors the serial line, so nothing is printed until the end, and the
    console's text is the same at both captures.
    """
    if westons():
        kill()
    time.sleep(1)
    before, line = capture(program)
    renderer, output, mode = restart()
    time.sleep(1)
    weston, wline = capture(program)
    back = kill()
    time.sleep(1)
    after, aline = capture(program)
    print("desktop: console before: %s" % line, flush=True)
    print("desktop: weston: %s" % wline, flush=True)
    print("desktop: console after: %s" % aline, flush=True)
    assert weston != before, "Weston's frame is the console's"
    assert after == before, "the console's frame changed across Weston"
    print("desktop: result: console: frame %s before Weston and after its SIGKILL, %s" %
          (before, back), flush=True)


def cmd_restart():
    """Start the session again and report its output."""
    renderer, output, mode = restart()
    print("desktop: result: restart: %s renderer, output %s, mode %s" % (renderer.lower(), output,
                                                                        mode), flush=True)


def cmd_mode(mode):
    """Set `mode` on the first output in weston.ini and start the session again."""
    text = INI.read_text()
    renderer, output, _ = drm_output(0)
    text = re.sub(r"\n\[output\]\n(?:[^\[].*\n?)*", "\n", text).rstrip("\n") + "\n"
    text += "\n[output]\nname=%s\nmode=%s\n" % (output, mode)
    INI.write_text(text)
    renderer, name, current = restart()
    assert name == output and current == mode, (name, current, mode)
    print("desktop: result: mode: output %s, mode %s" % (name, current), flush=True)


def cmd_egl():
    """Run weston-simple-egl in the session and require frames drawn with GL."""
    with tempfile.TemporaryDirectory(prefix="desktop-egl-", dir="/tmp") as name:
        os.chown(name, 1001, 1001)
        out = Path(name) / "egl.log"
        line = ("EGL_LOG_LEVEL=info exec timeout -s INT 20 weston-simple-egl -b > %s 2>&1"
                % out)
        status = subprocess.call(desktop(line))
        text = out.read_text(errors="replace")
        frames = [int(n) for n in re.findall(r"(\d+) frames in", text)]
        assert status in (0, 124, 130) and frames and sum(frames) > 0, (status, text[-4000:])
        print("desktop: result: egl: %d frames" % sum(frames), flush=True)


PAGE = b'''<!doctype html><title>Desktop input</title>
<style>html, body { margin: 0; width: 100%; height: 100%; background: #204060; }</style>
<body><script>
const say = path => fetch(path, {cache: "no-store"});
document.addEventListener("keydown", e => say("/key?k=" + encodeURIComponent(e.key)));
document.addEventListener("mousedown", e => say("/click?b=" + e.button));
(function ready() {
 if (document.hasFocus()) say("/ready"); else setTimeout(ready, 100);
})();
</script></body>'''


class Handler(http.server.BaseHTTPRequestHandler):
    """Serve the input page and record what it reports."""
    def do_GET(self):
        """Serve the page or record one of its reports."""
        url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(url.query)
        content = b"ok"
        if url.path == "/ready":
            self.server.ready.set()
        elif url.path == "/key":
            self.server.keys += query["k"][0]
            print("desktop: key: %s" % query["k"][0], flush=True)
        elif url.path == "/click":
            self.server.clicks += 1
            print("desktop: click: button %s" % query["b"][0], flush=True)
        else:
            content = PAGE
        if self.server.keys.endswith("rach2") and self.server.clicks:
            self.server.done.set()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def log_message(self, *_args):
        """Keep HTTP traffic out of the result stream."""


def cmd_browser(name):
    """Show the input page in browser `name` and wait for the injected input."""
    with tempfile.TemporaryDirectory(prefix="desktop-browser-", dir="/tmp") as work, \
            socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler) as server:
        os.chown(work, 1001, 1001)
        server.daemon_threads = True
        server.ready, server.done = threading.Event(), threading.Event()
        server.keys, server.clicks = "", 0
        url = "http://127.0.0.1:%d/" % server.server_address[1]
        if name == "chromium":
            line = ("exec chromium --ozone-platform=wayland --kiosk --no-first-run "
                    "--user-data-dir=%s/profile %s" % (work, url))
        else:
            profile = Path(work) / "profile"
            profile.mkdir()
            (profile / "user.js").write_text('user_pref("browser.shell.checkDefaultBrowser", false);\n')
            for path in (profile, profile / "user.js"):
                os.chown(path, 1001, 1001)
            line = "MOZ_ENABLE_WAYLAND=1 exec firefox-esr --kiosk --no-remote --profile %s %s" % (
                profile, url)
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        log = Path(work) / "browser.log"
        player = None
        try:
            with log.open("w") as out:
                player = subprocess.Popen(desktop(line), stdout=out, stderr=out,
                                          start_new_session=True)
            while not server.ready.wait(0.2):
                assert player.poll() is None, (name + " exited", log.read_text()[-4000:])
            print("desktop: ready: %s" % name, flush=True)
            while not server.done.wait(0.2):
                assert player.poll() is None, (name + " exited", log.read_text()[-4000:])
            print("desktop: result: %s: keys %s, clicks %d" % (name, server.keys, server.clicks),
                  flush=True)
        finally:
            if player is not None and player.poll() is None:
                os.killpg(player.pid, signal.SIGTERM)
                player.wait()
            server.shutdown()
            worker.join()


GL_VERTEX = b"""attribute vec2 at; void main() { gl_Position = vec4(at, 0.0, 1.0); }"""
GL_FRAGMENT = b"""precision highp float;
void main() { gl_FragColor = vec4(floor(gl_FragCoord.xy) / 255.0, 1.0, 1.0); }"""


def cmd_gl():
    """Draw a known pattern on llvmpipe off screen and compare its pixels.

    A 64x64 framebuffer is cleared to black, and a square whose edges lie on
    pixel boundaries, from 16 to 48 on each axis, is drawn by a shader that
    writes each pixel's own coordinates as red and green. The image read
    back must be that pattern, byte for byte, which needs no reference: the
    edges cover whole pixels and x/255 converts back to x exactly.
    """
    import ctypes
    import hashlib
    os.environ["EGL_PLATFORM"] = "surfaceless"
    os.environ["LIBGL_ALWAYS_SOFTWARE"] = "1"
    os.environ["GALLIUM_DRIVER"] = "llvmpipe"
    egl = ctypes.CDLL("libEGL.so.1")
    gles = ctypes.CDLL("libGLESv2.so.2")
    vp = ctypes.c_void_p
    egl.eglGetPlatformDisplay.restype = vp
    egl.eglGetPlatformDisplay.argtypes = [ctypes.c_uint, vp, vp]
    egl.eglInitialize.argtypes = [vp, vp, vp]
    egl.eglBindAPI.argtypes = [ctypes.c_uint]
    egl.eglCreateContext.restype = vp
    egl.eglCreateContext.argtypes = [vp, vp, vp, ctypes.POINTER(ctypes.c_int)]
    egl.eglMakeCurrent.argtypes = [vp, vp, vp, vp]
    gles.glGetString.restype = ctypes.c_char_p
    gles.glCreateShader.restype = ctypes.c_uint
    gles.glCreateProgram.restype = ctypes.c_uint
    display = egl.eglGetPlatformDisplay(0x31DD, None, None)  # EGL_PLATFORM_SURFACELESS_MESA
    assert display and egl.eglInitialize(display, None, None), "eglInitialize failed"
    assert egl.eglBindAPI(0x30A0)  # EGL_OPENGL_ES_API
    attrs = (ctypes.c_int * 3)(0x3098, 2, 0x3038)  # EGL_CONTEXT_CLIENT_VERSION 2
    context = egl.eglCreateContext(display, None, None, attrs)
    assert context and egl.eglMakeCurrent(display, None, None, context), "no GLES 2 context"
    renderer = gles.glGetString(0x1F01).decode()
    assert "llvmpipe" in renderer, renderer
    size = 64
    names = (ctypes.c_uint * 1)()
    gles.glGenRenderbuffers(1, names)
    gles.glBindRenderbuffer(0x8D41, names[0])
    gles.glRenderbufferStorage(0x8D41, 0x8058, size, size)  # GL_RGBA8
    fbo = (ctypes.c_uint * 1)()
    gles.glGenFramebuffers(1, fbo)
    gles.glBindFramebuffer(0x8D40, fbo[0])
    gles.glFramebufferRenderbuffer(0x8D40, 0x8CE0, 0x8D41, names[0])
    assert gles.glCheckFramebufferStatus(0x8D40) == 0x8CD5, "framebuffer incomplete"
    program = gles.glCreateProgram()
    for kind, source in ((0x8B31, GL_VERTEX), (0x8B30, GL_FRAGMENT)):
        shader = gles.glCreateShader(kind)
        text = ctypes.c_char_p(source)
        gles.glShaderSource(shader, 1, ctypes.byref(text), None)
        gles.glCompileShader(shader)
        ok = ctypes.c_int()
        gles.glGetShaderiv(shader, 0x8B81, ctypes.byref(ok))
        assert ok.value, "shader did not compile"
        gles.glAttachShader(program, shader)
    gles.glBindAttribLocation(program, 0, b"at")
    gles.glLinkProgram(program)
    gles.glUseProgram(program)
    square = (ctypes.c_float * 12)(-0.5, -0.5, 0.5, -0.5, 0.5, 0.5,
                                   -0.5, -0.5, 0.5, 0.5, -0.5, 0.5)
    gles.glViewport(0, 0, size, size)
    gles.glClearColor(ctypes.c_float(0), ctypes.c_float(0), ctypes.c_float(0), ctypes.c_float(1))
    gles.glClear(0x4000)
    gles.glVertexAttribPointer(0, 2, 0x1406, 0, 0, square)
    gles.glEnableVertexAttribArray(0)
    gles.glDrawArrays(4, 0, 6)
    pixels = (ctypes.c_ubyte * (size * size * 4))()
    gles.glReadPixels(0, 0, size, size, 0x1908, 0x1401, pixels)
    assert gles.glGetError() == 0, "GL error"
    want = bytearray()
    for y in range(size):
        for x in range(size):
            inside = 16 <= x < 48 and 16 <= y < 48
            want += bytes((x, y, 255, 255)) if inside else bytes((0, 0, 0, 255))
    got = hashlib.sha256(bytes(pixels)).hexdigest()
    assert bytes(pixels) == bytes(want), ("pixels differ", got, hashlib.sha256(want).hexdigest())
    print("desktop: result: gl: %s, sha256 %s" % (renderer, got), flush=True)


def main():
    """Run the subcommand the arguments name."""
    command, args = sys.argv[1], sys.argv[2:]
    {"drm": cmd_drm, "kill": cmd_kill, "restart": cmd_restart, "mode": cmd_mode,
     "egl": cmd_egl, "gl": cmd_gl, "console": cmd_console, "browser": cmd_browser}[command](*args)


if __name__ == "__main__":
    main()
