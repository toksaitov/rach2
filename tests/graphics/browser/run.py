"""Run a browser's HTTPS pages, diagnostics, WebGPU or recovery acceptance."""
import contextlib
import http.server
import json
import os
from pathlib import Path
import signal
import socket
import ssl
import subprocess
import sys
import threading
import time

from protocol import CDP, Marionette

HERE = Path(__file__).resolve().parent
PAYLOAD = b"rach2-file-acceptance\n"


def save(path, value):
    """Publish an evidence object, flushing it before the next action."""
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def stop(child):
    """Stop and reap a private process session, including its descendants."""
    if child is not None:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(child.pid, signal.SIGTERM)
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        with contextlib.suppress(ProcessLookupError):
            os.killpg(child.pid, signal.SIGKILL)
        child.wait()


class Handler(http.server.SimpleHTTPRequestHandler):
    """Serve the acceptance files and the real form, upload and download routes."""
    def __init__(self, *args, **kwargs):
        """Restrict static paths to the prepared browser directory."""
        super().__init__(*args, directory=str(HERE), **kwargs)

    def answer(self, data, kind="text/plain", download=False):
        """Send a complete response with an explicit length."""
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        if download:
            self.send_header("Content-Disposition", 'attachment; filename="acceptance.txt"')
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        """Serve HTML and assets; echo a submitted form to its parent frame."""
        if self.path == "/download":
            self.answer(PAYLOAD, download=True)
        elif self.path.startswith("/form?"):
            self.answer(b"<script>parent.postMessage(location.search,location.origin)</script>",
                        "text/html")
        elif self.path == "/font.ttf":
            self.answer(Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf").read_bytes(),
                        "font/ttf")
        else:
            super().do_GET()

    def do_POST(self):
        """Judge the actual file bytes sent by the browser's multipart upload."""
        data = self.rfile.read(int(self.headers["Content-Length"]))
        self.answer(b"rach2-upload" if self.path == "/upload" and
                    b'filename="input.txt"' in data and PAYLOAD in data else b"bad upload")

    def log_message(self, fmt, *args):
        """Keep the served page list as evidence without mixing result lines."""
        with self.server.log_lock:
            with self.server.log.open("a") as out:
                out.write(fmt % args + "\n")


@contextlib.contextmanager
def server(work, out):
    """Run a loopback HTTPS server with an ephemeral, explicitly accepted certificate."""
    cert, key = work / "cert.pem", work / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-days", "1", "-subj", "/CN=localhost", "-keyout", str(key),
                    "-out", str(cert)], check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as srv:
        srv.log, srv.log_lock = out / "https.log", threading.Lock()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        srv.socket = context.wrap_socket(srv.socket, server_side=True)
        thread = threading.Thread(target=srv.serve_forever)
        thread.start()
        try:
            yield "https://localhost:%d" % srv.server_address[1]
        finally:
            srv.shutdown()
            thread.join()


class Browser:
    """Own one fresh browser profile, process session and automation connection."""
    def __init__(self, name, mode, out, work):
        """Set all cleanup state before starting a browser."""
        self.name, self.mode, self.out, self.work = name, mode, out, work
        self.child = self.client = self.log = None
        self.target = None
        self.downloads = work / "downloads"
        self.downloads.mkdir()

    def __enter__(self):
        """Start an ordinary sandboxed browser, and record every argument and preference."""
        profile = self.work / "profile"
        profile.mkdir()
        try:
            if self.name == "chromium":
                args = ["chromium", "--ozone-platform=wayland", "--no-first-run",
                        "--no-default-browser-check", "--password-store=basic",
                        "--start-fullscreen", "--ignore-certificate-errors",
                        "--remote-debugging-port=0", "--user-data-dir=" + str(profile),
                        "--disable-background-networking", "--disable-hang-monitor",
                        "--disable-background-timer-throttling", "--disable-renderer-backgrounding",
                        "--autoplay-policy=no-user-gesture-required", "--ignore-gpu-blocklist"]
                if self.mode == "webgpu":
                    args += ["--enable-unsafe-webgpu", "--use-webgpu-adapter=swiftshader"]
                args += ["about:blank"]
                prefs = {}
            else:
                with socket.socket() as free:
                    free.bind(("127.0.0.1", 0))
                    port = free.getsockname()[1]
                args = ["firefox-esr", "--new-instance", "--no-remote", "--kiosk",
                        "--marionette", "--remote-allow-system-access", "--profile", str(profile),
                        "about:blank"]
                prefs = {"marionette.port": port, "browser.shell.checkDefaultBrowser": False,
                         "browser.startup.homepage_override.mstone": "ignore",
                         "browser.aboutwelcome.enabled": False, "media.autoplay.default": 0,
                         "media.autoplay.block-webaudio": False, "media.cubeb.backend": "pulse",
                         "webgl.force-enabled": True, "dom.max_script_run_time": 0,
                         "dom.max_chrome_script_run_time": 0,
                         "browser.download.folderList": 2, "browser.download.dir": str(self.downloads),
                         "browser.download.useDownloadDir": True,
                         "browser.helperApps.neverAsk.saveToDisk": "text/plain",
                         "browser.sessionstore.resume_from_crash": False,
                         "dom.events.testing.asyncClipboard": True}
                (profile / "user.js").write_text("".join(
                    "user_pref(%s, %s);\n" % (json.dumps(k), json.dumps(v))
                    for k, v in prefs.items()))
            save(self.out / "switches.json", {"argv": args, "prefs": prefs,
                 "version": subprocess.check_output([args[0], "--version"], text=True).strip(),
                 "certificate": "ephemeral local HTTPS certificate accepted by automation",
                 "environment": {k: v for k, v in os.environ.items() if k.startswith(
                     ("MOZ_", "LP_", "MESA_", "GRAPHICS_", "WAYLAND_", "XDG_", "PULSE_"))}})
            self.log = (self.out / "browser.log").open("w")
            env = dict(os.environ, MOZ_ENABLE_WAYLAND="1")
            self.child = subprocess.Popen(args, env=env, stdout=self.log, stderr=self.log,
                                          start_new_session=True)
            if self.name == "chromium":
                active = profile / "DevToolsActivePort"
                self.until(lambda: active.exists())
                lines = active.read_text().splitlines()
                self.client = CDP("ws://127.0.0.1:%s%s" % (lines[0], lines[1]))
                self.client.call("Browser.setDownloadBehavior", {
                    "behavior": "allow", "downloadPath": str(self.downloads)})
                self.target = self.client.call("Target.createTarget", {"url": "about:blank"})["targetId"]
                self.attach(self.target)
            else:
                def connect():
                    """Wait for Firefox's listener while watching its process."""
                    try:
                        self.client = Marionette(port)
                        return True
                    except ConnectionRefusedError:
                        return False
                self.until(connect)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_args):
        """Close protocol sockets, stop descendants and reap the browser."""
        if self.client:
            self.client.close()
        stop(self.child)
        if self.log:
            self.log.close()

    def until(self, fn):
        """Wait for an observed condition; the enclosing case owns stall detection."""
        while True:
            assert self.child.poll() is None, ("browser exited", self.child.returncode)
            value = fn()
            if value:
                return value
            time.sleep(0.1)

    def attach(self, target):
        """Select a CDP page, retaining the browser-level connection for diagnostics."""
        self.target = target
        self.session = self.client.call("Target.attachToTarget", {
            "targetId": target, "flatten": True})["sessionId"]
        self.client.call("Page.enable", {}, self.session)
        self.client.call("Target.activateTarget", {"targetId": target})
        self.client.call("Page.bringToFront", {}, self.session)

    def navigate(self, url):
        """Navigate the selected page and wait for its complete document."""
        if self.name == "chromium":
            self.client.call("Page.navigate", {"url": url}, self.session)
            self.until(lambda: self.script("location.href === %s && document.readyState === 'complete'"
                                           % json.dumps(url)))
        else:
            self.client.call("WebDriver:Navigate", {"url": url})

    def script(self, expression):
        """Evaluate an expression and await its promise, requiring no page exception."""
        if self.name == "chromium":
            value = self.client.call("Runtime.evaluate", {"expression": expression,
                "awaitPromise": True, "returnByValue": True, "userGesture": True}, self.session)
            assert "exceptionDetails" not in value, value
            return value["result"].get("value")
        reply = self.client.call("WebDriver:ExecuteScript", {
            "script": "return (" + expression + ");",
            "args": [], "newSandbox": True, "sandbox": None})
        with (self.out / "marionette.jsonl").open("a") as log:
            log.write(json.dumps({"expression": expression, "reply": reply}) + "\n")
        return reply["value"]

    def element(self, selector):
        """Find a Firefox element by CSS selector for native input operations."""
        value = self.client.call("WebDriver:FindElement", {"using": "css selector", "value": selector})
        return value["value"]["element-6066-11e4-a52e-4f735466cecf"]

    def click(self, selector):
        """Activate a real page control, with WebDriver's input path on Firefox."""
        if self.name == "chromium":
            self.script("document.querySelector(%s).click()" % json.dumps(selector))
        else:
            self.client.call("WebDriver:ElementClick", {"id": self.element(selector)})

    def tab(self):
        """Open and select a new tab in the same browser process."""
        if self.name == "chromium":
            self.attach(self.client.call("Target.createTarget", {"url": "about:blank"})["targetId"])
        else:
            value = self.client.call("WebDriver:NewWindow", {"type": "tab"})
            self.client.call("WebDriver:SwitchToWindow", {"handle": value["handle"]})


def pages(b, url):
    """Check persistent storage, native forms and files, clipboard and a loaded font."""
    b.navigate(url + "/page.html")
    result = {"initial": b.script("storage()")}
    assert result["initial"] == {"secure": True, "font": True}, result
    b.navigate(url + "/page.html")
    result["storage"] = b.script("readStorage()")
    assert result["storage"] == {"local": "rach2-local", "session": "rach2-session",
        "indexed": "rach2-indexed", "cookie": "acceptance=rach2-cookie", "cache": "rach2-cache"}, result
    b.click("#form button")
    result["form"] = b.until(lambda: b.script("results.form"))
    assert result["form"] == "?value=rach2-form", result
    upload = b.work / "input.txt"
    upload.write_bytes(PAYLOAD)
    if b.name == "chromium":
        root = b.client.call("DOM.getDocument", {}, b.session)["root"]["nodeId"]
        node = b.client.call("DOM.querySelector", {"nodeId": root, "selector": "#upload"}, b.session)["nodeId"]
        b.client.call("DOM.setFileInputFiles", {"nodeId": node, "files": [str(upload)]}, b.session)
    else:
        b.client.call("WebDriver:ElementSendKeys", {"id": b.element("#upload"), "text": str(upload)})
    result["upload"] = b.until(lambda: b.script("results.upload"))
    assert result["upload"] == "rach2-upload", result
    b.click("#download")
    path = b.downloads / "acceptance.txt"
    b.until(lambda: path.exists() and path.read_bytes() == PAYLOAD)
    result["download"] = path.read_text()
    if b.name == "chromium":
        b.client.call("Browser.grantPermissions", {"origin": url,
                      "permissions": ["clipboardReadWrite", "clipboardSanitizedWrite"]})
        b.client.call("Page.bringToFront", {}, b.session)
    b.click("#clipboard")
    result["clipboard"] = b.until(lambda: b.script("results.clipboard || results.error"))
    assert result["clipboard"] == "rach2-clipboard", result
    result["webgl"] = b.script("draw()")
    assert result["webgl"]["pixel"] == [255, 128, 0, 255], result
    assert "llvmpipe" in result["webgl"]["renderer"].lower(), result
    return result


def diagnostics(b, url):
    """Read the browsers' own sandbox diagnostics and each Chromium child's kernel state."""
    b.navigate(url + "/page.html")
    b.script("storage()")
    b.script("draw()")
    if b.name == "firefox":
        b.navigate("about:support")
        b.until(lambda: b.script("document.querySelector('#sandbox-tbody')?.innerText"))
        return {"about:support": b.script("document.querySelector('#sandbox-tbody').innerText"),
                "application": b.script("document.querySelector('#application-box')?.innerText || navigator.userAgent")}
    children = b.client.call("SystemInfo.getProcessInfo")["processInfo"]
    part = b.out / "process-ids.tmp"
    save(part, children)
    part.replace(b.out / "process-ids.json")
    state = b.out / "process-state.json"
    b.until(state.exists)
    result = {"processes": json.loads(state.read_text())}
    assert {p["role"] for p in result["processes"]} == {"renderer", "storage", "network", "gpu"}, result
    b.navigate("chrome://sandbox/")
    result["chrome://sandbox"] = b.until(lambda: b.script("document.body.innerText"))
    b.navigate("chrome://gpu/")
    text = """(() => { const read = root => Array.from(root.querySelectorAll('*')).filter(e => e.shadowRoot).map(e => e.shadowRoot.textContent + read(e.shadowRoot)).join('\\n'); return document.body.innerText + read(document); })()"""
    result["chrome://gpu"] = b.until(lambda: (lambda s: s if "Sandboxed" in s else None)(b.script(text)))
    return result


def recovery(b, url):
    """Kill Chromium's observed GPU process and require a new PID and pixel readback."""
    b.navigate(url + "/page.html")
    before = b.script("draw()")
    pid = next(p["id"] for p in b.client.call("SystemInfo.getProcessInfo")["processInfo"]
               if p["type"] == "GPU")
    b.script("window.losses=0; document.querySelector('canvas').addEventListener('webglcontextlost', e => {e.preventDefault(); losses++;});")
    os.kill(pid, signal.SIGKILL)
    b.until(lambda: b.script("losses > 0"))
    b.until(lambda: not b.script("document.querySelector('canvas').getContext('webgl2').isContextLost()"))
    after = b.script("draw()")
    new = next(p["id"] for p in b.client.call("SystemInfo.getProcessInfo")["processInfo"]
               if p["type"] == "GPU")
    assert new != pid and before == after and after["pixel"] == [255, 128, 0, 255], (before, after, pid, new)
    return {"before": before, "after": after, "old_pid": pid, "new_pid": new, "losses": b.script("losses")}


def webgpu(b, url):
    """Read compute and render results, two CTS groups and two upstream samples."""
    b.navigate(url + "/page.html")
    result = {"readback": b.script("gpu()")}
    got = result["readback"]
    assert got["words"] == 1024 and got["errors"] == 0 and got["pixel"] == [255, 128, 0, 255], got
    assert "swiftshader" in json.dumps(got["adapter"]).lower(), got
    save(b.out / "partial.json", result)
    b.navigate(url + "/cts.html")
    b.until(lambda: b.script("window.done"))
    assert not b.script("window.error"), b.script("window.error")
    result["cts"] = b.script("window.result")
    assert result["cts"]["results"] and all(row[1]["status"] == "pass" for row in result["cts"]["results"]), result
    save(b.out / "partial.json", result)
    source = (HERE / "sample.js").read_text()
    ident = b.client.call("Page.addScriptToEvaluateOnNewDocument", {"source": source}, b.session)["identifier"]
    try:
        result["samples"] = {}
        for name in ("helloTriangle", "helloTriangleMSAA"):
            b.navigate(url + "/assets/samples/sample/" + name + "/index.html")
            b.until(lambda: b.script("window.sample && (sample.error || sample.frames >= 3)"))
            row = b.script("window.sample")
            assert not row.get("error") and row["pixel"] == [255, 0, 0, 255], row
            result["samples"][name] = row
    finally:
        b.client.call("Page.removeScriptToEvaluateOnNewDocument", {"identifier": ident}, b.session)
    return result


def sound(b, url):
    """Play three tabs together until the loopback reader has its counted samples."""
    result = []
    for index, hz in enumerate((440, 660, 880)):
        if index:
            b.tab()
        b.navigate(url + "/page.html")
        value = b.script("tone(%d)" % hz)
        assert value == {"state": "running", "hz": hz}, value
        result.append(value)
    inputs = subprocess.check_output(["pactl", "list", "short", "sink-inputs"], text=True)
    assert inputs.strip(), "browser has no PulseAudio sink input"
    (b.out / "tones-ready").write_text("ready\n")
    b.until(lambda: (b.out / "tones-stop").exists())
    return {"tabs": result, "pulse_inputs": inputs}


def main():
    """Run one small acceptance case and retain its evidence even on failure."""
    name, mode, out_name, work_name = sys.argv[1:]
    out, work = Path(out_name).resolve(), Path(work_name).resolve()
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda *_: sys.exit(1))
    with server(work, out) as url, Browser(name, mode, out, work) as b:
        print("browser: action: %s %s" % (name, mode), flush=True)
        try:
            if mode == "pages":
                result = pages(b, url)
            elif mode == "sandbox":
                result = diagnostics(b, url)
            elif mode == "recovery":
                result = recovery(b, url)
            elif mode == "webgpu":
                result = webgpu(b, url)
            elif mode == "sound":
                result = sound(b, url)
            else:
                raise ValueError("unknown browser acceptance case " + mode)
            save(out / "result.json", result)
            print("browser: result: %s %s pass" % (name, mode), flush=True)
        except BaseException as error:
            save(out / "failure.json", {"error": repr(error)})
            raise


if __name__ == "__main__":
    main()
