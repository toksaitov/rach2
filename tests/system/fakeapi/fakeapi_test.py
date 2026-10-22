#!/usr/bin/env python3
"""Check how the fake API server chooses the step a request plays, and when
a WebSocket exchange has played it.

Usage: fakeapi_test.py

It runs the tests, as `tests/run host` does, and names each test that fails
on a line of its own.
"""

import io
import os
import struct
import sys
import tempfile
import threading
import unittest

# FAKEAPI_MODULES names a directory searched before this one for fakeapi.py
# and the protocol modules, so that copies there are tested in place of these.
if os.environ.get("FAKEAPI_MODULES"):
    sys.path.insert(0, os.environ["FAKEAPI_MODULES"])
import fakeapi  # noqa: E402


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


class HoldTest(unittest.TestCase):
    """Play a held step against the requests that wait for it and release it."""

    def setUp(self):
        """Make a record for the server to write."""
        handle, self.record = tempfile.mkstemp(prefix="rach2-fakeapi-test.")
        os.close(handle)

    def tearDown(self):
        """Remove the record."""
        os.unlink(self.record)

    def test_hold(self):
        """/fakeapi/held answers once the held step's request has come, whose
        reply then waits for /fakeapi/release; neither is recorded. A reply
        that does not wait has a second to come before the release; a wait
        of a minute only ends the test of a server that never answers."""
        server = fakeapi.Server({"protocol": "plain", "steps": [step("/turn", hold=True)]},
                                self.record, None)
        turn, held, released = io.BytesIO(), io.BytesIO(), io.BytesIO()
        client = threading.Thread(target=server.serve, args=(request("/turn"), None, turn),
                                  daemon=True)
        waiter = threading.Thread(target=server.serve,
                                  args=(request("/fakeapi/held"), None, held), daemon=True)
        try:
            client.start()
            waiter.start()
            waiter.join(60)
            self.assertFalse(waiter.is_alive())
            client.join(1)
            self.assertTrue(client.is_alive())
            self.assertEqual(turn.getvalue(), b"")
            server.serve(request("/fakeapi/release"), None, released)
            client.join(60)
            self.assertFalse(client.is_alive())
            self.assertTrue(held.getvalue().endswith(b"held 1\n"))
            self.assertTrue(released.getvalue().endswith(b"released\n"))
            self.assertTrue(turn.getvalue().startswith(b"HTTP/1.1 404 "))
            self.assertEqual((server.count, server.verdict()), (1, 0))
        finally:
            server.record_file.close()


if __name__ == "__main__":
    result = unittest.main(exit=False).result
    for kind, cases in (("failed", result.failures), ("raised an error", result.errors)):
        for case, _ in cases:
            print(f"fakeapi-test: fail: {case.id().rsplit('.', 1)[1]}: {kind}", file=sys.stderr)
    sys.exit(not result.wasSuccessful())
