#!/usr/bin/env python3
"""Check the acceptance's credential enumeration and log scan in a fresh home.

AGENTS_GUEST names the guest script to run, by default the tree's
accept/agents_guest.sh.
"""

import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest


GUEST = Path(os.environ.get("AGENTS_GUEST") or
             Path(__file__).with_name("accept") / "agents_guest.sh")
TOKEN = ".gemini/antigravity-cli/antigravity-oauth-token"
DUMMY = "rach2-dummy-antigravity-token-00000000"
FILES = {"claude": ".claude/.credentials.json", "codex": ".codex/auth.json",
         "agy": TOKEN, "muse": ".config/muse/auth.json"}
GH = ".config/gh/hosts.yml"


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
        """Hash Antigravity's OAuth token and report its absence."""
        file = self.write(TOKEN, DUMMY)
        digest = hashlib.sha256(DUMMY.encode()).hexdigest()
        self.assertIn(f"{file} {digest}\n", self.guest("credentials", "lenovo"))
        file.unlink()
        self.assertIn(f"{file} missing\n", self.guest("credentials", "lenovo"))

    def test_antigravity_scan(self):
        """Detect a dummy token in the log and remove it from filtered output."""
        for mode, path in (("lenovo", TOKEN), ("qemu", "rach2-accept/keys/agy")):
            file = self.write(path, DUMMY)
            self.assertEqual("0 of 1 strings", self.guest("secrets", mode, log="clean").strip())
            self.assertEqual("1 of 1 strings", self.guest("secrets", mode, log=DUMMY).strip())
            self.assertEqual("0 of 1 strings", self.guest("filtered", mode, "agy").strip())
            self.assertEqual("[credential]\n", self.guest("redact", mode, log=DUMMY))
            file.unlink()

    def test_redact_arguments(self):
        """Keep the credential strings out of redact's process arguments."""
        self.write("rach2-accept/keys/agy", DUMMY)
        guest = subprocess.Popen(["bash", str(GUEST), "redact"], env=self.env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.addCleanup(guest.wait)
        self.addCleanup(guest.kill)
        for _ in range(300):
            rows = descendants(guest.pid)
            if any(Path(comm).name == "awk" for comm, _ in rows):
                break
            time.sleep(0.1)
        else:
            self.fail("redact started no awk within 30 seconds")
        self.assertFalse([args for _, args in rows if DUMMY in args])
        out, _ = guest.communicate(f"a log with {DUMMY} in it\n", timeout=30)
        self.assertEqual("a log with [credential] in it\n", out)

    def test_selected_credentials(self):
        """Require only selected agents' files and gh's, in both modes."""
        for mode in ("lenovo", "qemu"):
            for agent, path in FILES.items():
                with self.subTest(mode=mode, agent=agent):
                    path = path if mode == "lenovo" else f"rach2-accept/keys/{agent}"
                    file = self.write(path, DUMMY)
                    gh = self.write(GH, "gho_dummy_github_token_00000000")
                    want = {str(file), str(gh)}
                    rows = self.guest("credentials", mode, agent).splitlines()
                    self.assertEqual(want, {row.split()[0] for row in rows})
                    self.assertFalse(any(row.endswith(" missing") for row in rows))
                    file.unlink()
                    self.assertIn(f"{file} missing\n", self.guest("credentials", mode, agent))
                    gh.unlink()
                    self.assertIn(f"{gh} missing\n", self.guest("credentials", mode, agent))

    def test_selected_secrets(self):
        """Ignore unused credentials and scan gh's alongside the selected ones."""
        for mode in ("lenovo", "qemu"):
            paths = FILES if mode == "lenovo" else {
                agent: f"rach2-accept/keys/{agent}" for agent in FILES}
            for agent, path in paths.items():
                self.write(path, f"dummy_{agent}_token_00000000000000")
            self.write(GH, "gho_dummy_github_token_00000000")
            for agent in FILES:
                with self.subTest(mode=mode, agent=agent):
                    self.assertEqual("0 of 2 strings", self.guest(
                        "secrets", mode, agent, log="clean").strip())
                    self.assertEqual("0 of 2 strings", self.guest("filtered", mode, agent).strip())
                    self.assertEqual("1 of 2 strings", self.guest(
                        "secrets", mode, agent, log=f"dummy_{agent}_token_00000000000000").strip())
                    self.assertEqual("1 of 2 strings", self.guest(
                        "secrets", mode, agent, log="gho_dummy_github_token_00000000").strip())
                    unused = next(name for name in FILES if name != agent)
                    self.assertEqual("0 of 2 strings", self.guest(
                        "secrets", mode, agent, log=f"dummy_{unused}_token_00000000000000").strip())
            self.assertEqual("0 of 3 strings", self.guest(
                "secrets", mode, "claude", "agy", log="clean").strip())
            self.assertEqual("0 of 5 strings", self.guest("secrets", mode, log="clean").strip())


def descendants(root):
    """Return the name and arguments of each process below root."""
    table = subprocess.run(["ps", "-A", "-ww", "-o", "pid=,ppid=,comm=,args="],
                           text=True, capture_output=True, check=True).stdout
    parent, rows = {}, {}
    for line in table.splitlines():
        fields = line.split(None, 3)
        if len(fields) < 3:
            continue
        pid, ppid = int(fields[0]), int(fields[1])
        parent[pid] = ppid
        rows[pid] = (fields[2], fields[3] if len(fields) == 4 else "")
    found = []
    for pid in rows:
        up = parent[pid]
        while up in parent and up != root and up > 1:
            up = parent[up]
        if up == root:
            found.append(rows[pid])
    return found


if __name__ == "__main__":
    unittest.main()
