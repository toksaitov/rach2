"""Small synchronous clients for Chromium's CDP and Firefox's Marionette."""
import base64
import hashlib
import json
import os
import socket
import struct
from urllib.parse import urlsplit


def receive(sock, count):
    """Read exactly count bytes or fail when the browser closes its socket."""
    data = bytearray()
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part:
            raise EOFError("browser closed its protocol socket")
        data.extend(part)
    return bytes(data)


class CDP:
    """Exchange CDP messages over one local WebSocket, preserving events."""
    def __init__(self, url):
        """Connect to a browser's observed debugging URL and check its upgrade."""
        u = urlsplit(url)
        self.sock = socket.create_connection((u.hostname, u.port))
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(("GET %s HTTP/1.1\r\nHost: %s:%d\r\n"
                           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
                           % (u.path, u.hostname, u.port, key)).encode())
        header = bytearray()
        while not header.endswith(b"\r\n\r\n"):
            header.extend(receive(self.sock, 1))
        accept = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        assert header.startswith(b"HTTP/1.1 101 ") and accept in header, header
        self.serial = 0
        self.events = []

    def frame(self, data, opcode=1):
        """Send a masked client frame, including lengths above 65535 bytes."""
        n = len(data)
        length = bytes([0x80 | n]) if n < 126 else (
            b"\xfe" + struct.pack("!H", n) if n < 65536 else
            b"\xff" + struct.pack("!Q", n))
        mask = os.urandom(4)
        self.sock.sendall(bytes([0x80 | opcode]) + length + mask +
                          bytes(v ^ mask[i % 4] for i, v in enumerate(data)))

    def message(self):
        """Read a text message, joining continuations and answering ping frames."""
        data = bytearray()
        while True:
            first, second = receive(self.sock, 2)
            n = second & 127
            if n == 126:
                n = struct.unpack("!H", receive(self.sock, 2))[0]
            elif n == 127:
                n = struct.unpack("!Q", receive(self.sock, 8))[0]
            assert not second & 128, "masked server frame"
            part = receive(self.sock, n)
            op = first & 15
            if op == 8:
                raise EOFError("browser closed its WebSocket")
            if op == 9:
                self.frame(part, 10)
                continue
            if op == 10:
                continue
            assert op in (0, 1), op
            data.extend(part)
            if first & 128:
                return json.loads(data)

    def call(self, method, params=None, session=None):
        """Send one command, retain intervening events and require success."""
        self.serial += 1
        msg = {"id": self.serial, "method": method, "params": params or {}}
        if session:
            msg["sessionId"] = session
        self.frame(json.dumps(msg).encode())
        while True:
            reply = self.message()
            if reply.get("id") == self.serial:
                assert "error" not in reply, reply
                return reply.get("result", {})
            self.events.append(reply)

    def close(self):
        """Close the connection; the owner stops and reaps the browser."""
        self.sock.close()


class Marionette:
    """Send one framed Marionette command at a time, as Firefox requires."""
    def __init__(self, port):
        """Connect to the selected profile's port and require protocol 3."""
        self.sock = socket.create_connection(("127.0.0.1", port))
        self.serial = 0
        assert self.message()["marionetteProtocol"] == 3
        self.capabilities = self.call("WebDriver:NewSession", {"acceptInsecureCerts": True})["capabilities"]
        assert self.capabilities["acceptInsecureCerts"] is True
        self.call("WebDriver:SetTimeouts", {"script": None})

    def message(self):
        """Read a length-prefixed UTF-8 JSON message."""
        length = bytearray()
        while True:
            c = receive(self.sock, 1)
            if c == b":":
                break
            length.extend(c)
        return json.loads(receive(self.sock, int(length)))

    def call(self, method, params=None):
        """Send a command and check its response type, sequence and error."""
        self.serial += 1
        data = json.dumps([0, self.serial, method, params or {}]).encode()
        self.sock.sendall(str(len(data)).encode() + b":" + data)
        kind, ident, error, result = self.message()
        assert kind == 1 and ident == self.serial and error is None, (method, error)
        return result

    def close(self):
        """Close the connection; the owner stops and reaps Firefox."""
        self.sock.close()
