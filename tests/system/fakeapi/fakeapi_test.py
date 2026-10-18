#!/usr/bin/env python3
"""Check how the fake API server chooses the step a request plays, and when
a WebSocket exchange has played it.

Usage: fakeapi_test.py [--mutate | --texts]

With no argument it runs the tests, as `tests/run host` does. --mutate runs
them against a private copy of fakeapi.py for each fault in FAULTS, and
requires each copy to fail its fault's test and no other: in one a repeated
step is used up by the first request that plays it, in another a
connection that ends in a WebSocket exchange is only logged, and in the
third the exchange begins only after its upgrade is sent. It then runs
them against fakeapi.py as it is, which must pass; nothing in the tree
changes. --texts checks that each fault's text is once in fakeapi.py.
"""

import io
import os
import struct
from pathlib import Path
import sys
import tempfile
import types
import unittest

import fakeapi

HERE = Path(__file__).resolve().parent
FAULTS = [
    ("test_repeat",
     '                    if not step.get("repeat"):\n',
     '                    if True:\n'),
    ("test_reset",
     '            if peer.socket is not None and not (peer.finished or peer.failed):\n',
     '            if False:\n'),
    ("test_reset",
     '        self.socket = WebSocket(self)\n'
     '        self.wfile.write(head(101, headers))\n',
     '        self.wfile.write(head(101, headers))\n'
     '        self.socket = WebSocket(self)\n'),
]


def request(path):
    """Return a GET of PATH, as the server reads one."""
    return fakeapi.Request("GET", path, "HTTP/1.1", [("Host", "fakeapi.test")], b"")


def step(path, **flags):
    """Return a plain step that answers a GET of PATH with 404."""
    return dict(flags, request={"path": path}, reply={"status": 404})


class StepTest(unittest.TestCase):
    """Play scripts of plain steps without a connection."""

    def setUp(self):
        """Make a record for the server to write."""
        handle, self.record = tempfile.mkstemp(prefix="rach2-fakeapi-test.")
        os.close(handle)

    def tearDown(self):
        """Remove the record."""
        os.unlink(self.record)

    def play(self, steps, paths):
        """Play PATHS against STEPS; return the step each played, None for a
        request none accepts, and the verdict on the steps left unplayed."""
        server = fakeapi.Server({"protocol": "plain", "steps": steps}, self.record, None)
        try:
            played = [server.take(request(path))[2] for path in paths]
            for number in played:
                if number:
                    server.finish(number)
            return played, server.verdict()
        finally:
            server.record_file.close()

    def test_unordered(self):
        """An unordered step is played once, before or after the ordered one."""
        steps = [step("/title", unordered=True), step("/turn")]
        self.assertEqual(self.play(steps, ["/turn", "/title"]), ([2, 1], 0))
        self.assertEqual(self.play(steps, ["/title", "/turn", "/title"])[0], [1, 2, None])
        self.assertEqual(self.play(steps, ["/turn"]), ([2], 1))

    def test_repeat(self):
        """A repeated step is played by any number of requests, none among them."""
        steps = [step("/log", unordered=True, repeat=True), step("/turn")]
        self.assertEqual(self.play(steps, ["/log", "/turn", "/log", "/log"]), ([1, 2, 1, 1], 0))
        self.assertEqual(self.play(steps, ["/turn"]), ([2], 0))
        self.assertEqual(self.play(steps, ["/log"]), ([1], 1))
        self.assertEqual(self.play(steps, ["/turn", "/other"])[0], [2, None])
        with self.assertRaises(ValueError):
            self.play([step("/log", repeat=True)], [])


class Wire:
    """A client's bytes for the server to read, then a reset."""

    def __init__(self, data):
        self.data = data

    def read(self, size):
        """Return the next SIZE bytes; raise ConnectionResetError at the end."""
        if not self.data:
            raise ConnectionResetError(104, "Connection reset by peer")
        part, self.data = self.data[:size], self.data[size:]
        return part


class Reset:
    """A connection the client has reset, which takes no bytes."""

    def write(self, data):
        """Raise ConnectionResetError."""
        raise ConnectionResetError(104, "Connection reset by peer")

    def flush(self):
        """Do nothing, there being nothing buffered."""


def frame(opcode, data):
    """Return a client's masked WebSocket frame of OPCODE with DATA."""
    mask = b"\x01\x02\x03\x04"
    return (struct.pack("!BB", 0x80 | opcode, 0x80 | len(data)) + mask +
            bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


class WebSocketTest(unittest.TestCase):
    """Play a WebSocket step that expects one message, without a connection."""

    def setUp(self):
        """Make a record for the server to write."""
        handle, self.record = tempfile.mkstemp(prefix="rach2-fakeapi-test.")
        os.close(handle)

    def tearDown(self):
        """Remove the record."""
        os.unlink(self.record)

    def play(self, sent, wfile=None):
        """Serve the upgrade to a client that sends SENT and then resets the
        connection, writing to WFILE, a buffer when it is None; return the
        verdict and the number of failures."""
        steps = [{"request": {"path": "/ws"},
                  "reply": {"websocket": [{"expect": "hello"}, {"send": "bye"}]}}]
        server = fakeapi.Server({"protocol": "plain", "steps": steps}, self.record, None)
        upgrade = fakeapi.Request("GET", "/ws", "HTTP/1.1",
                                  [("Host", "fakeapi.test"), ("Upgrade", "websocket"),
                                   ("Connection", "Upgrade"),
                                   ("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==")], b"")
        try:
            server.serve(upgrade, Wire(sent), wfile or io.BytesIO())
            return server.verdict(), len(server.failures)
        finally:
            server.record_file.close()

    def test_closed(self):
        """An exchange the client finishes and closes plays its step."""
        self.assertEqual(self.play(frame(0x1, b"hello") + frame(0x8, b"\x03\xe8")), (0, 0))

    def test_reset(self):
        """A reset before the expected message, the upgrade's among them,
        fails the run, and the step is not played; one in the closing
        handshake does neither."""
        self.assertEqual(self.play(b""), (1, 1))
        self.assertEqual(self.play(frame(0x1, b"hello"), Reset()), (1, 1))
        self.assertEqual(self.play(frame(0x1, b"hello")), (0, 0))


def run(module):
    """Run the tests with MODULE as fakeapi; return the result."""
    global fakeapi
    original = fakeapi
    try:
        fakeapi = module
        suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(case)
                                   for case in (StepTest, WebSocketTest))
        return unittest.TextTestRunner(stream=io.StringIO()).run(suite)
    finally:
        fakeapi = original


def mutate():
    """Require each fault to fail its test alone, and the source to pass."""
    source = (HERE / "fakeapi.py").read_text()
    for test, old, new in FAULTS:
        module = types.ModuleType("mutated_fakeapi")
        module.__file__ = str(HERE / "fakeapi.py")
        exec(compile(source.replace(old, new), module.__file__, "exec"), module.__dict__)
        result = run(module)
        failed = [t.id().rsplit(".", 1)[1] for t, _ in result.failures + result.errors]
        if failed != [test]:
            sys.exit(f"fakeapi-test: fail: the fault for {test} failed {failed or 'no test'}")
        print(f"fakeapi-test: result: the fault for {test} was caught")
    if not run(fakeapi).wasSuccessful():
        sys.exit("fakeapi-test: fail: the unchanged server did not pass")
    print("fakeapi-test: pass: the unchanged server passed")


if __name__ == "__main__":
    if sys.argv[1:] == ["--texts"]:
        source = (HERE / "fakeapi.py").read_text()
        if any(source.count(old) != 1 for _, old, _ in FAULTS):
            sys.exit("fakeapi-test: fail: a fault's text is not once in fakeapi.py")
    elif sys.argv[1:] == ["--mutate"]:
        mutate()
    else:
        unittest.main()
