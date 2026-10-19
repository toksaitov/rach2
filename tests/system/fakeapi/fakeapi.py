#!/usr/bin/env python3
"""fakeapi.py: a scripted HTTPS server that stands in for a vendor's API.

A suite that runs a program which calls a service tells the program, by the
program's documented settings, to trust a test certificate authority and to
reach this server. The server answers from a script of expected requests and
replies, records every request, and fails the run on a request the script does
not expect. It speaks HTTP/1.1 over TLS, with server-sent events and WebSocket.

Usage:

    fakeapi.py ca DIR NAME...
        Make DIR, then a certificate authority and a server certificate for
        the NAMEs, each a DNS name or an IP address, in it: ca.pem is what the
        client is told to trust, server.pem and server.key are what the
        server presents. Each is valid for two days from now. The caller
        removes DIR.

    fakeapi.py serve SCRIPT RECORD CERTDIR [PORT]
        Play SCRIPT on 127.0.0.1:PORT, a free port when PORT is 0 or left
        out, with the server certificate in CERTDIR, appending each request
        to RECORD. Standard error has `fakeapi: listening on 127.0.0.1:PORT`
        once connections are taken, a line for each request, and on SIGTERM
        or SIGINT the verdict: `fakeapi: result: pass: ...`, with exit status
        0, or `fakeapi: result: fail: ...`, with status 1. A script that
        cannot be read ends the server at once with status 2.

    fakeapi.py relay PORT
        Copy standard input to 127.0.0.1:PORT and what comes back to
        standard output. QEMU's user network starts a program for each
        connection to a `guestfwd=tcp:ADDRESS:PORT-cmd:COMMAND` address, with
        the connection as its standard input and output; this relay is that
        program, so a guest whose route out is closed (`restrict=on`) still
        reaches the server.

The script is a JSON object:

    {"protocol": "plain", "steps": [STEP, ...]}

The steps are played in turn. Each request is checked against the first step
not yet taken, in the order the requests arrive, whatever connection each
comes on. When the step's protocol accepts the request, the step is taken and
the server answers as it says. The step is played once the reply has ended,
or, for a WebSocket, once the server starts to close it; a connection that
ends in a WebSocket exchange before then fails the run, since the client has
not sent, or not been sent, all that the step requires. Any other request is
unexpected: it is answered with status 400 and a JSON error, recorded with
the reason, and the run fails, while the step waits for the next request. A
step with "unordered": true is taken by the first request it accepts,
whenever that comes: a client that sends two requests at once, such as a
title for the conversation beside its first turn, sends them in either order.
A request is checked against the step whose turn it is, then against each
unordered step not yet taken. An unordered step with "repeat": true as well
is never used up: any number of requests play it, none among them, for a
request that a client repeats on a timer of its own, such as a flush of usage
logs. A step may name its own "protocol"; otherwise it has the script's.

A step with "hold": true is held: once a request has taken it, the reply
waits until a request for /fakeapi/release has come. In a script with a held
step, a request for /fakeapi/held is answered with 200 and `held N` once the
held step's request N has come, and one for /fakeapi/release with 200 and
`released`; neither is numbered, recorded or checked against the script. A
test that inspects the client while it waits for an answer holds that answer,
asks for /fakeapi/held, inspects the client and asks for /fakeapi/release.

A protocol is a module `proto_NAME.py` in this directory with two functions:

    check(step, request)
        None when `request` is the one `step` expects, otherwise a sentence
        that says why not.
    reply(step, request, peer)
        Answer `request` as `step` says, through `peer`.

and, if it has one, `validate(step)`, which the server calls on each of the
protocol's steps before it listens: None for a step it can play, otherwise
the reason. `request` is a `Request`, and `peer` a `Peer`, whose methods send
a response, a stream of server-sent events, or take the connection over as a
WebSocket, and record or fail what happens there. `subset` compares JSON
values for the protocols' checks. Each protocol has a file of its own, so a
new one adds a file and changes nothing else; `proto_plain.py` writes each
request and reply out literally, and its comment describes its steps.

The record has one JSON object per line: for each request, `n`, its number
from 1, `method`, `path`, `query`, `headers` as [name, value] pairs, `body` as
text, and `step`, the number from 1 of the step it played, or `unexpected`,
the reason it was refused; for each WebSocket message, `n` and `received` or
`sent`, and `received_close` or `sent_close` with the code; and for a failure
after a request was matched, `n` and `failed`, or `connection` and `failed`
for a connection that sent something that is not a request.
"""

import base64
import hashlib
import http
import importlib
import ipaddress
import json
import os
import re
import signal
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import threading

sys.dont_write_bytecode = True

# The protocols import this file as `fakeapi`; run as a program, it is
# `__main__`, and a second copy would have a second set of names.
if __name__ == "__main__":
    sys.modules["fakeapi"] = sys.modules["__main__"]

WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
LINE_LIMIT = 65536
WEBSOCKET_LIMIT = 64 * 1024 * 1024


def log(text):
    """Write `fakeapi: TEXT` to standard error."""
    print("fakeapi: " + text, file=sys.stderr, flush=True)


def subset(expected, actual, where="$"):
    """Compare a JSON value with an expected one that may leave parts out.

    An object matches when it has each of the expected object's members and
    each matches; an array matches an array of the same length whose elements
    match in order; anything else must be equal, a boolean only to a boolean.
    Return None for a match, otherwise a sentence naming the first difference
    by its path, as `$.messages[0].role`.
    """
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            return f"{where} is {json.dumps(actual)}, not an object"
        for key, value in expected.items():
            if key not in actual:
                return f"{where} has no member {json.dumps(key)}"
            reason = subset(value, actual[key], f"{where}.{key}")
            if reason:
                return reason
        return None
    if isinstance(expected, list):
        if not isinstance(actual, list) or len(actual) != len(expected):
            return f"{where} is {json.dumps(actual)}, not an array of {len(expected)}"
        for i, value in enumerate(expected):
            reason = subset(value, actual[i], f"{where}[{i}]")
            if reason:
                return reason
        return None
    if expected != actual or isinstance(expected, bool) != isinstance(actual, bool):
        return f"{where} is {json.dumps(actual)}, not {json.dumps(expected)}"
    return None


class Request:
    """One HTTP request: its method, target and headers, and the whole body."""

    def __init__(self, method, target, version, headers, body):
        self.method = method
        self.target = target
        self.version = version
        self.headers = headers
        self.body = body
        self.path, _, self.query = target.partition("?")

    def header(self, name):
        """Return the first value of the header NAME, in any case, or None."""
        name = name.lower()
        return next((v for k, v in self.headers if k.lower() == name), None)

    def text(self):
        """Return the body as text, with an undecodable byte replaced."""
        return self.body.decode("utf-8", "replace")

    def json(self):
        """Return the body parsed as JSON; raise ValueError if it is not."""
        return json.loads(self.body)

    def describe(self):
        """Return the request line's method and target, as in log lines."""
        return f"{self.method} {self.target}"


def read_line(rfile):
    """Read one line of a request's head, without its line ending."""
    line = rfile.readline(LINE_LIMIT + 1)
    if len(line) > LINE_LIMIT:
        raise ValueError("a line of the request's head is too long")
    return line.decode("latin-1").rstrip("\r\n") if line else None


def read_request(rfile, wfile):
    """Read one request from a connection; None when it closes between two.

    The body is read whole, by its Content-Length or its chunks, and a client
    that waits with `Expect: 100-continue` is told to go on. ValueError means
    a request this server cannot read.
    """
    line = read_line(rfile)
    while line == "":
        line = read_line(rfile)
    if line is None:
        return None
    parts = line.split(" ")
    if len(parts) != 3 or not parts[2].startswith("HTTP/1."):
        raise ValueError(f"a malformed request line: {line!r}")
    headers = []
    while True:
        field = read_line(rfile)
        if field is None:
            raise ValueError("the connection ended in a request's head")
        if field == "":
            break
        name, colon, value = field.partition(":")
        if not colon or not name or name != name.strip():
            raise ValueError(f"a malformed header: {field!r}")
        headers.append((name, value.strip()))
    request = Request(parts[0], parts[1], parts[2], headers, b"")
    if (request.header("expect") or "").lower() == "100-continue":
        wfile.write(b"HTTP/1.1 100 Continue\r\n\r\n")
        wfile.flush()
    if "chunked" in (request.header("transfer-encoding") or "").lower():
        chunks = []
        while True:
            size = read_line(rfile)
            if size is None:
                raise ValueError("the connection ended in a chunked body")
            size = int(size.split(";")[0], 16)
            if size == 0:
                break
            chunks.append(rfile.read(size))
            rfile.readline(3)
        while read_line(rfile):
            pass
        request.body = b"".join(chunks)
    elif request.header("content-length") is not None:
        length = int(request.header("content-length"))
        request.body = rfile.read(length)
        if len(request.body) != length:
            raise ValueError("the connection ended in a request's body")
    return request


def head(status, headers):
    """Return a response's status line and headers as bytes."""
    try:
        phrase = http.HTTPStatus(status).phrase
    except ValueError:
        phrase = "Status"
    lines = [f"HTTP/1.1 {status} {phrase}"]
    lines += [f"{name}: {value}" for name, value in headers]
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1")


def pairs(headers):
    """Return headers given as a dict or as pairs as a list of pairs."""
    return list(headers.items()) if isinstance(headers, dict) else list(headers)


def has(headers, name):
    """Whether the pairs HEADERS have one named NAME, in any case."""
    return any(k.lower() == name.lower() for k, _ in headers)


class Peer:
    """The client's end of one request, through which a protocol replies.

    A protocol calls one of `respond`, `events` or `websocket` once for each
    request. `keep` is false once the connection is not to be used again.
    """

    def __init__(self, server, request, n, number, rfile, wfile):
        self.server = server
        self.request = request
        self.n = n
        self.number = number      # the step the request takes, from 1, or None
        self.rfile = rfile
        self.wfile = wfile
        self.keep = (request.version == "HTTP/1.1" and
                     (request.header("connection") or "").lower() != "close")
        self.socket = None        # the WebSocket, once the request is upgraded
        self.failed = False
        self.finished = False

    def note(self, **fields):
        """Add a line about this request to the record."""
        self.server.record(dict(n=self.n, **fields))

    def fail(self, reason):
        """Fail the run for REASON, in the log and the record."""
        self.failed = True
        self.server.fail(f"request {self.n}", reason, self.n)

    def finish(self):
        """Count the request's step played, unless its reply failed the run.

        The server calls it when the reply ends, and `WebSocket.close` before
        the closing handshake, so a client that drops the connection then has
        still played the step. A second call does nothing.
        """
        if not self.failed and not self.finished:
            self.finished = True
            self.server.finish(self.number)

    def respond(self, status, headers=(), body=b""):
        """Send a response with a whole body, text or bytes."""
        if isinstance(body, str):
            body = body.encode()
        headers = pairs(headers) + [("Content-Length", str(len(body)))]
        if not self.keep:
            headers.append(("Connection", "close"))
        self.wfile.write(head(status, headers) + body)
        self.wfile.flush()

    def events(self, events, status=200, headers=()):
        """Send server-sent events, in a chunked body that ends after them.

        Each event is an object with any of `event`, `id` and `data`; `data`
        is sent as it is when it is a string and as compact JSON otherwise,
        and a string of several lines as one `data:` field each.
        """
        headers = pairs(headers)
        if not has(headers, "content-type"):
            headers.append(("Content-Type", "text/event-stream"))
        headers += [("Cache-Control", "no-cache"), ("Transfer-Encoding", "chunked")]
        if not self.keep:
            headers.append(("Connection", "close"))
        self.wfile.write(head(status, headers))
        self.wfile.flush()
        for event in events:
            text = ""
            for field in ("event", "id"):
                if field in event:
                    text += f"{field}: {event[field]}\n"
            data = event.get("data", "")
            if not isinstance(data, str):
                data = json.dumps(data, separators=(",", ":"))
            text += "".join(f"data: {line}\n" for line in data.split("\n")) + "\n"
            chunk = text.encode()
            self.wfile.write(b"%x\r\n%s\r\n" % (len(chunk), chunk))
            self.wfile.flush()
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def websocket(self, protocol=None):
        """Accept the request's WebSocket upgrade and return the WebSocket.

        PROTOCOL, when given, is the subprotocol the server answers with. No
        extension is accepted, so the client sends its messages uncompressed.
        The exchange begins before the upgrade is sent, so a connection that
        ends while it is sent fails the step.
        """
        key = self.request.header("sec-websocket-key") or ""
        accept = base64.b64encode(hashlib.sha1((key + WEBSOCKET_GUID).encode()).digest())
        headers = [("Upgrade", "websocket"), ("Connection", "Upgrade"),
                   ("Sec-WebSocket-Accept", accept.decode())]
        if protocol:
            headers.append(("Sec-WebSocket-Protocol", protocol))
        self.keep = False
        self.socket = WebSocket(self)
        self.wfile.write(head(101, headers))
        self.wfile.flush()
        return self.socket


class WebSocket:
    """The server's end of a WebSocket (RFC 6455), its messages recorded.

    `receive` answers a ping and skips a pong while it waits for a message,
    and joins a fragmented one.
    """

    def __init__(self, peer):
        self.peer = peer
        self.closed = False

    def _frame(self):
        rfile = self.peer.rfile
        first = rfile.read(2)
        if len(first) < 2:
            return None, None, None
        fin, opcode = first[0] & 0x80, first[0] & 0x0F
        length = first[1] & 0x7F
        if length == 126:
            length = struct.unpack("!H", rfile.read(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", rfile.read(8))[0]
        if not first[1] & 0x80:
            raise ValueError("a client's WebSocket frame is not masked")
        if length > WEBSOCKET_LIMIT:
            raise ValueError(f"a WebSocket frame of {length} bytes")
        mask = rfile.read(4)
        data = rfile.read(length)
        if len(data) != length:
            raise ValueError("the connection ended in a WebSocket frame")
        return fin, opcode, bytes(b ^ mask[i % 4] for i, b in enumerate(data))

    def _send(self, opcode, data):
        length = len(data)
        if length < 126:
            header = struct.pack("!BB", 0x80 | opcode, length)
        elif length < 65536:
            header = struct.pack("!BBH", 0x80 | opcode, 126, length)
        else:
            header = struct.pack("!BBQ", 0x80 | opcode, 127, length)
        self.peer.wfile.write(header + data)
        self.peer.wfile.flush()

    def receive(self):
        """Return the next message, text as str and binary as bytes.

        None means the client closed the WebSocket, or the connection ended.
        """
        parts = []
        kind = None
        while True:
            fin, opcode, data = self._frame()
            if opcode is None:
                self.closed = True
                return None
            if opcode == 0x8:
                if not self.closed:
                    self._send(0x8, data[:2])
                    self.closed = True
                self.peer.note(received_close=struct.unpack("!H", data[:2])[0]
                               if len(data) >= 2 else None)
                return None
            if opcode == 0x9:
                self._send(0xA, data)
                continue
            if opcode == 0xA:
                continue
            if opcode in (0x1, 0x2):
                kind = opcode
            parts.append(data)
            if fin:
                break
        message = b"".join(parts)
        if kind == 0x1:
            message = message.decode()
            self.peer.note(received=message)
        else:
            self.peer.note(received_binary=base64.b64encode(message).decode())
        return message

    def send(self, message):
        """Send a message: text for a str, binary for bytes."""
        if isinstance(message, str):
            self.peer.note(sent=message)
            self._send(0x1, message.encode())
        else:
            self.peer.note(sent_binary=base64.b64encode(message).decode())
            self._send(0x2, message)

    def close(self, code=1000):
        """Close the WebSocket with CODE and wait for the client's close."""
        self.peer.finish()
        if self.closed:
            return
        self._send(0x8, struct.pack("!H", code))
        self.closed = True
        self.peer.note(sent_close=code)
        while True:
            _, opcode, data = self._frame()
            if opcode is None:
                return
            if opcode == 0x8:
                self.peer.note(received_close=struct.unpack("!H", data[:2])[0]
                               if len(data) >= 2 else None)
                return


def protocol(name):
    """Import and return the module of the protocol NAME."""
    if not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", name):
        raise ValueError(f"{json.dumps(name)} is not a protocol's name")
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    return importlib.import_module("proto_" + name)


class Server:
    """A script being played: its steps, its record and its failures."""

    def __init__(self, script, record_path, context):
        self.steps = script["steps"]
        self.default = script.get("protocol", "plain")
        self.context = context
        self.taken = set()
        self.played = set()
        self.count = 0
        self.failures = []
        self.lock = threading.Lock()
        self.hold = threading.Condition()
        self.held = None          # the number of the request a held step waits with
        self.released = False
        self.holds = any(isinstance(step, dict) and step.get("hold") for step in self.steps)
        self.record_file = open(record_path, "a", encoding="utf-8")
        for i, step in enumerate(self.steps, 1):
            if not isinstance(step, dict):
                raise ValueError(f"step {i} is not an object")
            module = self.module(step)
            reason = module.validate(step) if hasattr(module, "validate") else None
            if reason:
                raise ValueError(f"step {i}: {reason}")
            if step.get("repeat") and not step.get("unordered"):
                raise ValueError(f"step {i}: a repeated step is not unordered")

    def module(self, step):
        """Return the protocol module that plays STEP."""
        return protocol(step.get("protocol", self.default))

    def record(self, entry):
        """Append ENTRY to the record as one line, at once."""
        with self.lock:
            self.record_file.write(json.dumps(entry) + "\n")
            self.record_file.flush()

    def fail(self, where, reason, n=None):
        """Fail the run for REASON, met by request N or the connection WHERE."""
        with self.lock:
            self.failures.append(f"{where}: {reason}")
        self.record({"n": n, "failed": reason} if n else {"connection": where, "failed": reason})
        log(f"{where}: fail: {reason}")

    def candidates(self):
        """Return the numbers from 0 of the steps a request may play now.

        The first is the ordered step whose turn it is, when one is left; the
        unordered steps not yet played follow, the repeated ones among them.
        """
        left = [i for i in range(len(self.steps)) if i not in self.taken]
        ordered = [i for i in left if not self.steps[i].get("unordered")]
        return ordered[:1] + [i for i in left if self.steps[i].get("unordered")]

    def take(self, request):
        """Number REQUEST and check it against the steps it may play.

        Return the request's number, the step and its number when the
        request plays it, and otherwise the reason it is unexpected, which
        is the first step's. A step other than a repeated one is taken, so
        that no other request plays it; `finish` counts it played.
        """
        with self.lock:
            self.count += 1
            n = self.count
            reasons = []
            for i in self.candidates():
                step = self.steps[i]
                try:
                    reason = self.module(step).check(step, request)
                except Exception as error:  # a fault of the script or its protocol
                    reason = f"the check failed: {type(error).__name__}: {error}"
                if not reason:
                    if not step.get("repeat"):
                        self.taken.add(i)
                    return n, step, i + 1, None
                reasons.append(f"step {i + 1}: {reason}")
            if not reasons:
                return n, None, None, "the script has no step left"
            return n, None, None, reasons[0]

    def finish(self, number):
        """Count the step NUMBER, from 1, played, its reply having ended."""
        with self.lock:
            if not self.steps[number - 1].get("repeat"):
                self.played.add(number - 1)

    def wait_release(self, n):
        """Make the request N, which took a held step, wait for the release."""
        log(f"{n}: held")
        with self.hold:
            self.held = n
            self.hold.notify_all()
            self.hold.wait_for(lambda: self.released)
        log(f"{n}: released")

    def control(self, request, rfile, wfile):
        """Answer a request for /fakeapi/held or /fakeapi/release.

        The first waits until a held step's request has come; the second
        lets every held reply go, now or when its request comes. Return
        whether the connection may take another request.
        """
        peer = Peer(self, request, None, None, rfile, wfile)
        with self.hold:
            if request.path == "/fakeapi/release":
                self.released = True
                self.hold.notify_all()
                text = "released\n"
            else:
                self.hold.wait_for(lambda: self.held is not None)
                text = f"held {self.held}\n"
        log(f"{request.describe()}: {text.strip()}")
        peer.respond(200, [("Content-Type", "text/plain")], text)
        return peer.keep

    def serve(self, request, rfile, wfile):
        """Answer one request; return whether its connection may take another."""
        if self.holds and request.path in ("/fakeapi/held", "/fakeapi/release"):
            return self.control(request, rfile, wfile)
        n, step, number, reason = self.take(request)
        entry = {"n": n, "method": request.method, "path": request.path,
                 "query": request.query, "headers": request.headers,
                 "body": request.text()}
        peer = Peer(self, request, n, number, rfile, wfile)
        if step is None:
            entry["unexpected"] = reason
            self.record(entry)
            with self.lock:
                self.failures.append(f"request {n}: {request.describe()}: {reason}")
            log(f"{n}: {request.describe()}: unexpected: {reason}")
            body = json.dumps({"error": {"type": "unexpected_request",
                                         "message": f"fakeapi: {reason}"}})
            peer.respond(400, [("Content-Type", "application/json")], body)
            return peer.keep
        entry["step"] = number
        self.record(entry)
        log(f"{n}: {request.describe()}: step {number}")
        if step.get("hold"):
            self.wait_release(n)
        try:
            self.module(step).reply(step, request, peer)
        except OSError as error:
            if peer.socket is not None and not (peer.finished or peer.failed):
                peer.fail(f"the connection ended in the WebSocket exchange: {error}")
            else:
                log(f"{n}: the connection ended in the reply: {error}")
                peer.finish()
            return False
        except Exception as error:  # a fault of the script or its protocol
            peer.fail(f"the reply failed: {type(error).__name__}: {error}")
            return False
        peer.finish()
        return peer.keep

    def verdict(self):
        """Log the run's result and return its exit status."""
        owed = [i for i in range(len(self.steps)) if not self.steps[i].get("repeat")]
        left = [i for i in owed if i not in self.played]
        faults = []
        if self.failures:
            faults.append(f"{len(self.failures)} failed, the first {self.failures[0]}")
        if left:
            step = json.dumps(self.steps[left[0]])
            if len(step) > 160:
                step = step[:157] + "..."
            faults.append(f"{len(left)} of {len(owed)} steps not played, "
                          f"the first step {left[0] + 1}: {step}")
        if faults:
            log("result: fail: " + "; ".join(faults))
            return 1
        log(f"result: pass: {len(owed)} steps played by {self.count} requests")
        return 0


class Connection(socketserver.BaseRequestHandler):
    """One client connection: the TLS handshake, then requests in turn."""

    def handle(self):
        fake = self.server.fake
        peer = f"{self.client_address[0]}:{self.client_address[1]}"
        try:
            sock = fake.context.wrap_socket(self.request, server_side=True)
        except (ssl.SSLError, OSError) as error:
            log(f"tls: {peer}: the handshake failed: {error}")
            return
        rfile = sock.makefile("rb")
        wfile = sock.makefile("wb")
        try:
            while True:
                try:
                    request = read_request(rfile, wfile)
                except ValueError as error:
                    fake.fail(peer, f"a request that cannot be read: {error}")
                    wfile.write(head(400, [("Content-Length", "0"), ("Connection", "close")]))
                    wfile.flush()
                    return
                if request is None or not fake.serve(request, rfile, wfile):
                    return
        except (OSError, ValueError) as error:
            log(f"{peer}: the connection failed: {error}")
        finally:
            for f in (rfile, wfile):
                try:
                    f.close()
                except OSError:
                    pass
            try:
                sock.close()
            except OSError:
                pass


class Listener(socketserver.ThreadingTCPServer):
    """The listening socket, a thread for each connection."""

    daemon_threads = True
    allow_reuse_address = True


def make_ca(directory, names):
    """Make the test CA and a server certificate for NAMES in DIRECTORY."""
    os.makedirs(directory, mode=0o700)
    alt = []
    for name in names:
        try:
            ipaddress.ip_address(name)
            alt.append("IP:" + name)
        except ValueError:
            alt.append("DNS:" + name)
    config = os.path.join(directory, "openssl.cnf")
    with open(config, "w", encoding="ascii") as f:
        f.write("[req]\ndistinguished_name = dn\nprompt = no\n[dn]\n"
                "[ca]\nbasicConstraints = critical, CA:TRUE\n"
                "keyUsage = critical, keyCertSign, cRLSign\n"
                "subjectKeyIdentifier = hash\n"
                "[server]\nbasicConstraints = critical, CA:FALSE\n"
                "keyUsage = critical, digitalSignature\n"
                "extendedKeyUsage = serverAuth\n"
                f"subjectAltName = {', '.join(alt)}\n"
                "subjectKeyIdentifier = hash\nauthorityKeyIdentifier = keyid\n")

    def path(name):
        return os.path.join(directory, name)

    key = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes"]
    commands = [
        ["req", "-config", config, "-x509", "-extensions", "ca", *key, "-days", "2",
         "-subj", "/CN=Rach2 fakeapi test CA", "-keyout", path("ca.key"), "-out", path("ca.pem")],
        ["req", "-config", config, "-new", *key, "-subj", "/CN=" + names[0],
         "-keyout", path("server.key"), "-out", path("server.csr")],
        ["x509", "-req", "-in", path("server.csr"), "-CA", path("ca.pem"),
         "-CAkey", path("ca.key"), "-set_serial", str(int.from_bytes(os.urandom(8), "big")),
         "-days", "2", "-extfile", config, "-extensions", "server", "-out", path("server.pem")],
    ]
    for command in commands:
        result = subprocess.run(["openssl", *command], capture_output=True, text=True,
                                check=False)
        if result.returncode != 0:
            log(f"ca: openssl {command[0]} failed: {result.stderr.strip()}")
            return 1
    log(f"ca: made {path('ca.pem')} and a server certificate for {', '.join(names)}")
    return 0


def serve(script_path, record_path, certdir, port):
    """Play the script until a signal; return the verdict's exit status."""
    try:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(os.path.join(certdir, "server.pem"),
                                os.path.join(certdir, "server.key"))
        context.set_alpn_protocols(["http/1.1"])
    except OSError as error:
        log(f"result: refused: the server certificate in {certdir}: {error}")
        return 2
    try:
        with open(script_path, encoding="utf-8") as f:
            script = json.load(f)
        fake = Server(script, record_path, context)
    except (OSError, ValueError, KeyError, TypeError, ImportError) as error:
        log(f"result: refused: {script_path}: {error}")
        return 2
    listener = Listener(("127.0.0.1", port), Connection)
    listener.fake = fake

    def stop(signum, frame):
        threading.Thread(target=listener.shutdown).start()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    log(f"listening on 127.0.0.1:{listener.server_address[1]}")
    listener.serve_forever(poll_interval=0.2)
    listener.server_close()
    return fake.verdict()


def relay(port):
    """Copy standard input to the server and its answers to standard output."""
    devnull = os.open(os.devnull, os.O_WRONLY)
    os.dup2(devnull, 2)
    upstream = socket.create_connection(("127.0.0.1", port))

    def outward():
        while data := os.read(0, 65536):
            upstream.sendall(data)
        upstream.shutdown(socket.SHUT_WR)

    def inward():
        while data := upstream.recv(65536):
            view = memoryview(data)
            while view:
                view = view[os.write(1, view):]
        try:
            socket.socket(fileno=os.dup(1)).shutdown(socket.SHUT_WR)
        except OSError:
            os.close(1)

    threads = [threading.Thread(target=f, daemon=True) for f in (outward, inward)]
    for thread in threads:
        thread.start()
    threads[1].join()
    return 0


def main(argv):
    """Run the subcommand ARGV names."""
    if len(argv) >= 3 and argv[0] == "ca":
        return make_ca(argv[1], argv[2:])
    if len(argv) in (4, 5) and argv[0] == "serve":
        return serve(argv[1], argv[2], argv[3], int(argv[4]) if len(argv) == 5 else 0)
    if len(argv) == 2 and argv[0] == "relay":
        return relay(int(argv[1]))
    print("usage: fakeapi.py ca DIR NAME... | serve SCRIPT RECORD CERTDIR [PORT] | relay PORT",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
