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
        # Build stubs carry the marker so ovrl can recognize them as ours.
        for name in STUBS + ["ovrinject.exe"]:
            (self.build / name).write_bytes(b"MZ stub:" + name.encode()
                                            + b" OVRSTUB-BUILD-MARKER-v1")
        self.game = root / "game"
        self.game.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *argv: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            try:
                rc = cli.main(["--build", str(self.build), *argv])
            except SystemExit as e:  # sys.exit(msg) -> code 1, sys.exit(int)
                if isinstance(e.code, int) or e.code is None:
                    rc = e.code or 0
                else:
                    print(e.code, file=out)  # sys.exit("message")
                    rc = 1
        return rc, out.getvalue()

    def test_install_backs_up_and_restore_reverts(self):
        # One real DLL present, one absent.
        (self.game / "LibOVRRT64_1.dll").write_text("real runtime")

        rc, _ = self.run_cli("stubs", "install", str(self.game))
        self.assertEqual(rc, 0)
        for name in STUBS:
            self.assertIn(b"OVRSTUB-BUILD-MARKER-v1", (self.game / name).read_bytes())
        self.assertEqual((self.game / "LibOVRRT64_1.dll.ovrl-orig").read_text(),
                         "real runtime")
        self.assertFalse((self.game / "LibOVRPlatform64_1.dll.ovrl-orig").exists())

        rc, _ = self.run_cli("stubs", "restore", str(self.game))
        self.assertEqual(rc, 0)
        self.assertEqual((self.game / "LibOVRRT64_1.dll").read_text(), "real runtime")
        self.assertFalse((self.game / "LibOVRPlatform64_1.dll").exists())
        self.assertFalse((self.game / "LibOVRRT64_1.dll.ovrl-orig").exists())

    def test_real_runtime_is_placed_and_left_on_restore(self):
        real = Path(self.tmp.name) / "LibOVRRT64_1.dll"
        real.write_bytes(b"MZ genuine meta runtime")  # MZ, no marker

        rc, out = self.run_cli("stubs", "install", str(self.game),
                               "--real-runtime", str(real))
        self.assertEqual(rc, 0, out)
        # The runtime next to the exe is Meta's file, not our stub.
        self.assertEqual((self.game / "LibOVRRT64_1.dll").read_bytes(),
                         b"MZ genuine meta runtime")
        # The Platform SDK is still our stub.
        self.assertIn(b"OVRSTUB-BUILD-MARKER-v1",
                      (self.game / "LibOVRPlatform64_1.dll").read_bytes())

        rc, _ = self.run_cli("stubs", "restore", str(self.game))
        self.assertEqual(rc, 0)
        # The Platform stub is gone; the genuine runtime is left untouched.
        self.assertFalse((self.game / "LibOVRPlatform64_1.dll").exists())
        self.assertEqual((self.game / "LibOVRRT64_1.dll").read_bytes(),
                         b"MZ genuine meta runtime")

    def test_real_runtime_rejects_our_own_stub(self):
        rc, out = self.run_cli("stubs", "install", str(self.game),
                               "--real-runtime", str(self.build / "LibOVRRT64_1.dll"))
        self.assertEqual(rc, 1)
        self.assertIn("ovrstub build", out)

    def test_install_is_idempotent(self):
        (self.game / "LibOVRRT64_1.dll").write_text("real runtime")
        self.run_cli("stubs", "install", str(self.game))
        # A second install must not overwrite the backup with the stub.
        self.run_cli("stubs", "install", str(self.game))
        self.assertEqual((self.game / "LibOVRRT64_1.dll.ovrl-orig").read_text(),
                         "real runtime")


if __name__ == "__main__":
    unittest.main()
