#!/usr/bin/env python3
"""Check the acceptance's credential enumeration and log scan in a fresh home."""

import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


GUEST = Path(__file__).with_name("agents_guest.sh")
TOKEN = ".gemini/antigravity-cli/antigravity-oauth-token"
DUMMY = "rach2-dummy-antigravity-token-00000000"


class CredentialsTest(unittest.TestCase):
    """Run the machine's commands with disposable credentials and no agent."""

    def setUp(self):
        """Give each test a home and scratch directory removed on every exit."""
        self.work = tempfile.TemporaryDirectory(prefix="rach2-agents-credentials.")
        self.addCleanup(self.work.cleanup)
        self.home = Path(self.work.name)
        self.env = {"HOME": str(self.home), "TMPDIR": str(self.home),
                    "PATH": os.environ["PATH"]}

    def guest(self, command, mode, *agents, log=""):
        """Return the guest command's output without inheriting credentials."""
        return subprocess.run(["bash", str(GUEST), command, mode, *agents],
                              env=self.env, input=log, text=True,
                              capture_output=True, check=True).stdout

    def write(self, path, text):
        """Write a dummy credential below the disposable home."""
        file = self.home / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(text)
        return file

    def test_antigravity(self):
        """Hash Antigravity's OAuth token and count it only in a log containing it."""
        file = self.write(TOKEN, DUMMY)
        digest = hashlib.sha256(DUMMY.encode()).hexdigest()
        self.assertIn(f"{file} {digest}\n", self.guest("credentials", "lenovo"))
        self.assertEqual("0 of 1 strings", self.guest("secrets", "lenovo", log="clean").strip())
        self.assertEqual("1 of 1 strings", self.guest("secrets", "lenovo", log=DUMMY).strip())
        file.unlink()
        self.assertIn(f"{file} missing\n", self.guest("credentials", "lenovo"))


def proof(mode):
    """Require a private guest script that omits Antigravity's token to fail."""
    source = GUEST.read_text()
    old = '"$HOME/.gemini/antigravity-cli/antigravity-oauth-token" '
    if source.count(old) != 1:
        raise SystemExit("agents-credentials: the fault's text is not once in its source")
    if mode == "--texts":
        print("agents-credentials: texts: the fault is once in its source")
        return
    with tempfile.TemporaryDirectory(prefix="rach2-agents-credentials-proof.") as work:
        copy = Path(work)
        test = copy / Path(__file__).name
        test.write_text(Path(__file__).read_text())
        guest = copy / GUEST.name
        for phase, text, expected in [("fixed", source, True),
                                      ("omitted", source.replace(old, ""), False),
                                      ("restored", source, True)]:
            guest.write_text(text)
            result = subprocess.run([sys.executable, "-B", str(test)],
                                    text=True, capture_output=True)
            if (result.returncode == 0) != expected:
                raise SystemExit(result.stdout + result.stderr)
            if not expected and "FAIL: test_antigravity" not in result.stderr:
                raise SystemExit(result.stderr)
            print(f"agents-credentials: result: {phase}: expected status {result.returncode}")


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] in ("--texts", "--mutate"):
        proof(sys.argv[1])
    else:
        unittest.main()
