"""Run a browser's HTTPS pages, diagnostics, GPU, WebGPU or recovery acceptance."""
import base64
import contextlib
import http.server
import json
import os
from pathlib import Path
import re
import signal
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import time

from protocol import CDP, Marionette

HERE = Path(__file__).resolve().parent
PAYLOAD = b"rach2-file-acceptance\n"
# The renderer Crocus names on the HD 4400, in Mesa's renderer string.
CROCUS = "Mesa Intel(R) HD Graphics 4400 (HSW GT2)"


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
                        "--autoplay-policy=no-user-gesture-required"]
                # The GPU profile keeps Chromium's blocklist. Software
                # WebGL and the Lenovo's hasvk WebGPU probe lift it.
                if self.mode != "gpu":
                    args += ["--ignore-gpu-blocklist"]
                if self.mode == "webgpu":
                    args += ["--enable-unsafe-webgpu", "--use-webgpu-adapter=swiftshader"]
                elif self.mode.startswith("webgpu-venus"):
                    args += ["--enable-unsafe-webgpu", "--enable-logging=stderr", "--v=0",
                             "--vmodule=*webgpu*=3,*dawn*=3,*gpu_info*=2,*gpu_init*=2"]
                    if self.mode == "webgpu-venus-native":
                        args += ["--use-vulkan=native"]
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
                         "dom.max_script_run_time": 0,
                         "dom.max_chrome_script_run_time": 0,
                         "browser.download.folderList": 2, "browser.download.dir": str(self.downloads),
                         "browser.download.useDownloadDir": True,
                         "browser.helperApps.neverAsk.saveToDisk": "text/plain",
                         "browser.sessionstore.resume_from_crash": False,
                         "dom.events.testing.asyncClipboard": True}
                # The GPU profile keeps Firefox's own choice of WebGL and
                # WebRender, as Debian runs it; llvmpipe needs WebGL forced.
                if self.mode != "gpu":
                    prefs["webgl.force-enabled"] = True
                (profile / "user.js").write_text("".join(
                    "user_pref(%s, %s);\n" % (json.dumps(k), json.dumps(v))
                    for k, v in prefs.items()))
            env = dict(os.environ, MOZ_ENABLE_WAYLAND="1")
            save(self.out / "switches.json", {"argv": args, "prefs": prefs,
                 "version": subprocess.check_output([args[0], "--version"], text=True).strip(),
                 "certificate": "ephemeral local HTTPS certificate accepted by automation",
                 "environment": {k: v for k, v in env.items() if k.startswith(
                     ("MOZ_", "LP_", "MESA_", "GRAPHICS_", "WAYLAND_", "XDG_", "PULSE_", "VK_"))}})
            self.log = (self.out / "browser.log").open("w")
            self.child = subprocess.Popen(args, env=env, stdout=self.log, stderr=self.log,
                                          start_new_session=True)
            if self.name == "chromium":
                # Chromium makes the file before it writes the port and the
                # path into it, so wait for both lines whole.
                active = profile / "DevToolsActivePort"
                lines = self.until(lambda: active.exists() and re.fullmatch(
                    r"(\d+)\n(/devtools/browser/[0-9a-f-]{36})\n?", active.read_text()))
                self.client = CDP("ws://127.0.0.1:%s%s" % (lines[1], lines[2]))
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
        dumps = self.work / "profile/minidumps"
        if dumps.exists() and any(dumps.iterdir()):
            shutil.copytree(dumps, self.out / "minidumps", dirs_exist_ok=True)
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
    result["form"] = b.until(lambda: b.script("results.form || results.error"))
    assert result["form"] == "?value=rach2-form", result
    upload = b.work / "input.txt"
    upload.write_bytes(PAYLOAD)
    if b.name == "chromium":
        root = b.client.call("DOM.getDocument", {}, b.session)["root"]["nodeId"]
        node = b.client.call("DOM.querySelector", {"nodeId": root, "selector": "#upload"}, b.session)["nodeId"]
        b.client.call("DOM.setFileInputFiles", {"nodeId": node, "files": [str(upload)]}, b.session)
    else:
        b.client.call("WebDriver:ElementSendKeys", {"id": b.element("#upload"), "text": str(upload)})
    result["upload"] = b.until(lambda: b.script("results.upload || results.error"))
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


def diagnostics(b, url, ready="Sandboxed"):
    """Read the browsers' own sandbox diagnostics and each Chromium child's kernel state.

    chrome://gpu is read once its text holds READY. Its sandbox line comes
    with the page, but Dawn's adapter list comes later, so a case that judges
    Dawn passes "Vulkan backend", which that list has and the page lacks.
    """
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
    result["chrome://gpu"] = b.until(lambda: (lambda s: s if "Sandboxed" in s and ready in s else None)(b.script(text)))
    return result


def rows(text, names):
    """Return the first line of each named row of an about:support table's text."""
    found = {}
    for line in text.splitlines():
        name, _, value = line.partition("\t")
        if name.strip() in names and name.strip() not in found:
            found[name.strip()] = value.strip()
    return found


def gpu(b, url):
    """Read the renderer each GPU path names, its diagnostics and each process's sandbox.

    The renderer is named from WebGL's unmasked renderer, from Chromium's
    chrome://gpu, whose GPU0 line gives the PCI identity ANGLE read for the
    GPU process's device, and from Firefox's about:support. Pixels are read and kept, not judged here,
    since the fault backend draws none; browsers-gpu.sh judges them on the
    machine.
    """
    b.navigate(url + "/page.html")
    result = {"webgl": b.script("draw()")}
    result.update(diagnostics(b, url))
    if b.name == "firefox":
        # The Graphics section is filled in after the page loads.
        text = b.until(lambda: (lambda s: s if s and "Compositing\t" in s else None)(
            b.script("document.body.innerText")))
        result["graphics"] = rows(text, ("Compositing", "Description", "Vendor ID", "Device ID",
                                         "Driver Vendor", "Driver Version", "WebGL 1 Driver Renderer",
                                         "WebGL 2 Driver Renderer", "Window Protocol"))
        save(b.out / "partial.json", result)
        graphics = result["graphics"]
        assert graphics.get("Compositing") == "WebRender", graphics
        assert "crocus" in graphics.get("Driver Vendor", ""), graphics
        assert graphics.get("Device ID") == "0x0a16", graphics
        assert "llvmpipe" not in json.dumps(result["webgl"]).lower(), result["webgl"]
        return result
    # The page's text runs its table's rows and list items together.
    text = result["chrome://gpu"]
    found = re.search(r"GL_RENDERER\s*:\s*(.*?\))(?:GL_VERSION|\n)", text)
    result["gl_renderer"] = found[1] if found else None
    found = re.search(r"\bWebGL: ([^*\n]*)", text)
    result["webgl_status"] = found[1].strip() if found else None
    found = re.search(r"GPU0\s*:\s*VENDOR=\s*(0x[0-9a-f]+).*?DEVICE=\s*(0x[0-9a-f]+)", text)
    result["gpu0"] = list(found.groups()) if found else None
    save(b.out / "partial.json", result)
    assert result["gl_renderer"] and CROCUS in result["gl_renderer"], result["gl_renderer"]
    assert CROCUS in result["webgl"]["renderer"], result["webgl"]
    assert result["webgl_status"] == "Hardware accelerated", result["webgl_status"]
    assert result["gpu0"] == ["0x8086", "0x0a16"], result["gpu0"]
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


def webgpu_venus(b, url):
    """Record both adapter requests and count readbacks only on a named Venus adapter."""
    assert b.name == "chromium"
    with (b.out / "vulkaninfo.err").open("w") as err:
        vulkan = subprocess.check_output(["vulkaninfo", "--summary"], text=True, stderr=err)
    (b.out / "vulkaninfo.out").write_text(vulkan)
    assert "Virtio-GPU Venus" in vulkan, "the guest has no Venus Vulkan device"
    b.navigate(url + "/page.html")
    result = b.script((HERE / "venus.js").read_text())
    result["platform"] = "wayland"
    save(b.out / "partial.json", result)
    save(b.out / "gpu-info.json", b.client.call("SystemInfo.getInfo"))
    b.navigate("chrome://gpu/")
    text = """(() => { const read = root => Array.from(root.querySelectorAll('*')).filter(e => e.shadowRoot).map(e => e.shadowRoot.textContent + read(e.shadowRoot)).join('\\n'); return document.body.innerText + read(document); })()"""
    diagnostic = b.until(lambda: (lambda s: s if "Vulkan backend" in s else None)(b.script(text)))
    (b.out / "gpu.txt").write_text(diagnostic)
    if b.mode == "webgpu-venus-native":
        assert "Virtio-GPU Venus" in diagnostic, "Dawn did not enumerate Venus"
    print("browser: adapters: " + json.dumps(result, sort_keys=True), flush=True)
    return result


# Chromium's trace categories for the picture's first frames: each thread's
# tasks, the GPU process's and the compositor's work, Dawn's calls, Mojo's
# and Wayland's messages and the page's own marks.
TRACE_CATEGORIES = ["toplevel", "gpu", "gpu.dawn", "disabled-by-default-gpu.dawn", "viz", "cc",
                    "ui", "mojom", "ipc", "wayland", "benchmark", "blink.user_timing"]


def hasvk_picture(b, url, trace=None, swiftshader=False, picture_done=None):
    """Check hasvk's compute and render readback, then draw the picture page's 480 frames.

    With TRACE, a path, Chromium's own trace of the picture page is kept
    there, gzipped. SWIFTSHADER selects the QEMU diagnostic, which traces
    the readback too and marks every frame. PICTURE_DONE runs after the
    picture has answered, while its threads are still alive.
    """
    request = "navigator.gpu.requestAdapter()"
    check = """
        if (!adapter || adapter.info.vendor !== 'intel' ||
            adapter.info.architecture !== 'gen-7' || adapter.info.isFallbackAdapter)
            throw Error('no native Intel gen-7 adapter');"""
    if swiftshader:
        request = "navigator.gpu.requestAdapter({forceFallbackAdapter: true})"
        check = """
        if (!adapter || adapter.info.architecture !== 'swiftshader')
            throw Error('no SwiftShader adapter');"""
    timing = {"transferMode": "ReturnAsStream", "streamCompression": "gzip",
              "traceConfig": {"recordMode": "recordContinuously", "traceBufferSizeInKb": 262144,
                              "includedCategories": TRACE_CATEGORIES}}
    if trace and swiftshader:
        b.client.call("Tracing.start", timing)
    b.navigate(url + "/page.html")
    result = {"readback": b.script("""(async () => {
        const adapter = await %s;
        %s
        return await gpu(adapter);
    })()""" % (request, check))}
    save(b.out / "partial.json", result)
    got = result["readback"]
    assert got["words"] == 1024 and got["errors"] == 0 and got["pixel"] == [255, 128, 0, 255], got
    if trace and not swiftshader:
        b.client.call("Tracing.start", timing)
    b.navigate(url + "/hasvk.html" + ("?swiftshader" if swiftshader else ""))
    result["picture"] = b.until(lambda: b.script("window.result || null"))
    if picture_done:
        picture_done()
    if trace:
        b.client.call("Tracing.end")
        stream = b.client.event("Tracing.tracingComplete")["params"]["stream"]
        with open(trace, "wb") as kept:
            while True:
                part = b.client.call("IO.read", {"handle": stream, "size": 1 << 20})
                data = part.get("data", "")
                kept.write(base64.b64decode(data) if part.get("base64Encoded") else data.encode())
                if part.get("eof"):
                    break
        b.client.call("IO.close", {"handle": stream})
    save(b.out / "partial.json", result)
    assert result["picture"]["ok"], result["picture"]
    return result


def webgpu_frames(b, url, trace):
    """Draw the hasvk picture as webgpu-hasvk does and keep its frame times, with a trace if asked.

    Like webgpu-hasvk, the caller supplies a DRM Weston. Nothing is judged
    but the readback and the picture's own errors; the caller reads the times.
    """
    assert b.name == "chromium"
    return hasvk_picture(b, url, b.out / "trace.json.gz" if trace else None)


def webgpu_hasvk(b, url):
    """Read hasvk's compute and pixels, capture the panel, then read the sandboxes.

    The caller supplies a DRM Weston with its screenshot protocol enabled.
    Only the blocklist switch is lifted, as in the Lenovo's Linux reference.
    """
    assert b.name == "chromium"
    with (b.out / "vulkaninfo.err").open("w") as err:
        vulkan = subprocess.check_output(["vulkaninfo", "--summary"], text=True, stderr=err)
    (b.out / "vulkaninfo.out").write_text(vulkan)
    assert "HD Graphics 4400" in vulkan and "llvmpipe" not in vulkan, vulkan
    assert re.search(r"apiVersion\s*=\s*1\.2\.", vulkan), "hasvk is not at its default API version"
    result = hasvk_picture(b, url)
    shots = b.work / "shots"
    shots.mkdir()
    with (b.out / "screenshooter.log").open("w") as log:
        subprocess.run(["weston-screenshooter"], cwd=shots, stdout=log, stderr=log, check=True)
    pictures = list(shots.glob("*.png"))
    assert len(pictures) == 1, "screenshooter did not capture one panel"
    shutil.copyfile(pictures[0], b.out / "panel.png")
    save(b.out / "gpu-info.json", b.client.call("SystemInfo.getInfo"))
    result.update(diagnostics(b, url, "Vulkan backend"))
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
            elif mode == "gpu":
                result = gpu(b, url)
            elif mode == "recovery":
                result = recovery(b, url)
            elif mode == "webgpu":
                result = webgpu(b, url)
            elif mode in ("webgpu-venus", "webgpu-venus-native"):
                result = webgpu_venus(b, url)
            elif mode == "webgpu-hasvk":
                result = webgpu_hasvk(b, url)
            elif mode in ("webgpu-frames", "webgpu-trace"):
                result = webgpu_frames(b, url, mode == "webgpu-trace")
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
