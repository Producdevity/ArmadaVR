import os
from pathlib import Path
import subprocess
import tempfile
import unittest


LAUNCHER = Path(__file__).parents[1] / "tools/desktop-steam.sh"


class DesktopSteamTests(unittest.TestCase):
    def launch(self, platforms):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            client = home / ".local/share/Steam"
            binary = client / "steamrtarm64/steam"
            binary.parent.mkdir(parents=True)
            binary.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
            binary.chmod(0o755)
            for platform in platforms:
                library = client / "steamapps/common/SteamVR/bin" / platform / "vrclient.so"
                library.parent.mkdir(parents=True)
                library.touch()
            subprocess.run(["bash", str(LAUNCHER), "-applaunch", "250820"],
                           env=os.environ | {"HOME": str(home)}, check=True, capture_output=True)
            return (home / ".local/state/armada-vr/steam.log").read_text().splitlines()

    def test_x86_only_runtime_skips_native_client_integration(self):
        args = self.launch(["linux64"])
        self.assertIn("-vrdisable", args)
        self.assertEqual(args[-2:], ["-applaunch", "250820"])

    def test_native_runtime_keeps_client_integration(self):
        self.assertNotIn("-vrdisable", self.launch(["linux64", "linuxarm64"]))

    def test_no_runtime_does_not_change_client_flags(self):
        self.assertNotIn("-vrdisable", self.launch([]))
