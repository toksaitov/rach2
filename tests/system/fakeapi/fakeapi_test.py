#!/usr/bin/env python3
"""Check how the fake API server chooses the step a request plays.

Usage: fakeapi_test.py [--mutate | --texts]

With no argument it runs the tests, as `tests/run host` does. --mutate runs
them first against a private copy of fakeapi.py in which a repeated step is
used up by the first request that plays it, and requires that copy to fail
the test of repeated steps and no other, and then against fakeapi.py as it
is, which must pass; nothing in the tree changes. --texts checks that the
fault's text is once in fakeapi.py.
"""

import io
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest

import fakeapi

HERE = Path(__file__).resolve().parent
FAULT = ('                    if not step.get("repeat"):\n',
         '                    if True:\n')


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


def run(module):
    """Run the tests with MODULE as fakeapi; return the result."""
    global fakeapi
    original = fakeapi
    try:
        fakeapi = module
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(StepTest)
        return unittest.TextTestRunner(stream=io.StringIO()).run(suite)
    finally:
        fakeapi = original


def mutate():
    """Require the fault to fail test_repeat alone, and the source to pass."""
    source = (HERE / "fakeapi.py").read_text()
    module = types.ModuleType("mutated_fakeapi")
    module.__file__ = str(HERE / "fakeapi.py")
    exec(compile(source.replace(*FAULT), module.__file__, "exec"), module.__dict__)
    result = run(module)
    failed = [test.id().rsplit(".", 1)[1] for test, _ in result.failures + result.errors]
    if failed != ["test_repeat"]:
        sys.exit(f"fakeapi-test: fail: the fault failed {failed or 'no test'}, not test_repeat")
    print("fakeapi-test: result: a repeated step used up once was caught")
    if not run(fakeapi).wasSuccessful():
        sys.exit("fakeapi-test: fail: the unchanged server did not pass")
    print("fakeapi-test: pass: the unchanged server passed")


if __name__ == "__main__":
    if sys.argv[1:] == ["--texts"]:
        if (HERE / "fakeapi.py").read_text().count(FAULT[0]) != 1:
            sys.exit("fakeapi-test: fail: the fault's text is not once in fakeapi.py")
    elif sys.argv[1:] == ["--mutate"]:
        mutate()
    else:
        unittest.main()
