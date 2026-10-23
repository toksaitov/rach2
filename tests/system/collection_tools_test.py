#!/usr/bin/env python3
"""Check collection image transfers, byte offsets and command statuses."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("collection_tools", ROOT / "tools/etude/collection_tools.py")
TOOLS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOLS)


class Transfers(unittest.TestCase):
    """Exercise the host side through a local job transport."""

    def setUp(self):
        """Make a private tree and fake commands before any test writes files."""
        self.scratch = tempfile.TemporaryDirectory(prefix="rach2-collection-test.")
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.work = self.root / "work"
        self.work.mkdir()
        self.disk = self.root / "a disk.img"
        self.disk.write_bytes(b"disk bytes")
        self.job = TOOLS.Job(self.root, self.work)

    def local_job(self, args, env):
        """Execute a translated job with the same input and output layout."""
        run = self.root / "run"
        run.mkdir()
        for path in self.job.inputs:
            shutil.copytree(path, run / path.name)
        actual = dict(env)
        for index, path in enumerate(self.job.disks, 1):
            actual["ETUDE_DISK" + str(index)] = str(path)
        actual["PATH"] = str(self.root / "bin") + os.pathsep + os.environ["PATH"]
        status = subprocess.call(["sh", str(self.work / "job.sh")], cwd=run, env=actual)
        for name in self.job.outputs:
            shutil.copytree(run / name, self.work / "out" / name)
        return status

    def command(self, name, text):
        """Install a command whose assertions run inside the translated job."""
        (self.root / "bin").mkdir(exist_ok=True)
        path = self.root / "bin" / name
        path.write_text("#!/bin/sh\nset -eu\n" + text)
        path.chmod(0o755)

    def execute(self, tool, args):
        """Run the real staging and copyback code with the local transport."""
        script = TOOLS.image_command(self.job, tool, args)
        with patch.dict(os.environ, {"RACH2_QEMU_ACCEL": "whpx"}), \
                patch.object(TOOLS, "run_child", side_effect=self.local_job):
            return self.job.run(script)

    def test_offset(self):
        """An attached image keeps its offset and the guest filename verbatim."""
        self.command("mtype", '[ "$1" = -i ]; [ "$2" = "$ETUDE_DISK1@@1048576" ]; '
                     '[ "$3" = "::/a file" ]\n')
        self.assertEqual(self.execute("mtype", ["-i", str(self.disk) + "@@1048576", "::/a file"]), 0)

    def test_copy_in(self):
        """A source copied to a FAT directory keeps its original basename."""
        source = self.root / "Busy Box"
        source.write_text("input bytes")
        self.command("mcopy", '[ "$4" = ::/ ]; [ "${3##*/}" = "Busy Box" ]; '
                     '[ "$(cat "$3")" = "input bytes" ]\n')
        self.assertEqual(self.execute("mcopy", ["-i", str(self.disk), str(source), "::/"]), 0)

    def test_failed_copy_out(self):
        """A command's partial output and nonzero status both reach its caller."""
        dest = self.root / "returned bytes"
        self.command("mcopy", 'printf partial > "$4"\nexit 7\n')
        self.assertEqual(self.execute("mcopy", ["-i", str(self.disk), "::/file", str(dest)]), 7)
        self.assertEqual(dest.read_text(), "partial")

    def test_missing_output(self):
        """A failure before writing creates no output file on the host."""
        dest = self.root / "absent"
        self.command("mcopy", 'exit 1\n')
        self.assertEqual(self.execute("mcopy", ["-i", str(self.disk), "::/file", str(dest)]), 1)
        self.assertFalse(dest.exists())

    def test_block_count(self):
        """mke2fs' block count is kept after its image operand."""
        self.command("mke2fs", '[ "$1" = -F ]; [ "$2" = "$ETUDE_DISK1" ]; [ "$3" = 16384 ]\n')
        self.assertEqual(self.execute("mke2fs", ["-F", str(self.disk), "16384"]), 0)

    def test_native_goals(self):
        """Disk fixtures return from Linux, while scenario programs build natively."""
        self.assertFalse(TOOLS.native_goal("build/tests/cases/machine/disks/one.disk"))
        self.assertFalse(TOOLS.native_goal("build/x86_64/images/test.krffs"))
        self.assertTrue(TOOLS.native_goal("build/tests/cases/machine/disks_scenario"))

    def test_style_name(self):
        """The style script's short case name selects its full manifest identity."""
        directory = self.root / "tests/cases"
        directory.mkdir(parents=True)
        (directory / "host.style.json").write_text(json.dumps({"cases": [
            {"case_id": "host.style.manual", "prepare": []}]}))
        with patch.object(TOOLS, "run_child", return_value=0), \
                patch.object(self.job, "run", return_value=0) as run:
            self.assertEqual(TOOLS.style_case(self.job, "i686", "manual"), 0)
        self.assertIn("sh tests/cases/host/style.sh manual\n", run.call_args.args[0])

    def test_populated_ext2(self):
        """mke2fs receives the staged directory used to populate its image."""
        source = self.root / "ext2 files"
        source.mkdir()
        (source / "file").write_text("seed")
        self.command("mke2fs", '[ "$2" = -d ]; [ "$(cat "$3/file")" = seed ]; '
                     '[ "$4" = "$ETUDE_DISK1" ]; [ "$5" = 16384 ]\n')
        self.assertEqual(self.execute("mke2fs", ["-F", "-d", str(source), str(self.disk), "16384"]), 0)


if __name__ == "__main__":
    unittest.main()
