"""Tests for `ovrl stubs` install/restore, with a fake build directory."""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from ovrl import cli

STUBS = ["LibOVRRT64_1.dll", "LibOVRPlatform64_1.dll"]


class StubsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.build = root / "build"
        self.build.mkdir()
        for name in STUBS + ["ovrinject.exe"]:
            (self.build / name).write_text(f"stub:{name}")
        self.game = root / "game"
        self.game.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *argv: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            rc = cli.main(["--build", str(self.build), *argv])
        return rc, out.getvalue()

    def test_install_backs_up_and_restore_reverts(self):
        # One real DLL present, one absent.
        (self.game / "LibOVRRT64_1.dll").write_text("real runtime")

        rc, _ = self.run_cli("stubs", "install", str(self.game))
        self.assertEqual(rc, 0)
        for name in STUBS:
            self.assertEqual((self.game / name).read_text(), f"stub:{name}")
        self.assertEqual((self.game / "LibOVRRT64_1.dll.ovrl-orig").read_text(),
                         "real runtime")
        self.assertFalse((self.game / "LibOVRPlatform64_1.dll.ovrl-orig").exists())

        rc, _ = self.run_cli("stubs", "restore", str(self.game))
        self.assertEqual(rc, 0)
        self.assertEqual((self.game / "LibOVRRT64_1.dll").read_text(), "real runtime")
        self.assertFalse((self.game / "LibOVRPlatform64_1.dll").exists())
        self.assertFalse((self.game / "LibOVRRT64_1.dll.ovrl-orig").exists())

    def test_install_is_idempotent(self):
        (self.game / "LibOVRRT64_1.dll").write_text("real runtime")
        self.run_cli("stubs", "install", str(self.game))
        # A second install must not overwrite the backup with the stub.
        self.run_cli("stubs", "install", str(self.game))
        self.assertEqual((self.game / "LibOVRRT64_1.dll.ovrl-orig").read_text(),
                         "real runtime")


if __name__ == "__main__":
    unittest.main()
