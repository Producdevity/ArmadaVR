import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("fastrpc_artifact", Path(__file__).parents[1] / "tools/fastrpc-artifact.py")
artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(artifact)


class FastRPCInstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.bundle = self.directory / "bundle"
        self.prefix = self.bundle / artifact.PREFIX
        (self.prefix / "lib64").mkdir(parents=True)
        library = self.prefix / "lib64/library.so.1"
        library.write_bytes(b"library fixture")
        library.chmod(0o755)
        (self.prefix / "lib64/library.so").symlink_to("library.so.1")
        (self.bundle / "build.json").write_text("{}\n")
        self.record = {"build_sha256": artifact.digest(self.bundle / "build.json"),
                       "contents": artifact.contents(self.prefix)}
        self.root = self.directory / "root"
        self.root.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def test_install_preserves_libraries_and_links_without_services(self):
        artifact.install(self.bundle, self.root, self.record)
        artifact.verify_install(self.root, self.record)
        self.assertEqual((self.root / artifact.PREFIX / "lib64/library.so").readlink(), Path("library.so.1"))
        self.assertFalse((self.root / "etc").exists())
        with self.assertRaisesRegex(ValueError, "already supplies"):
            artifact.install(self.bundle, self.root, self.record)

    def test_modified_bundle_is_refused_before_installing(self):
        (self.prefix / "lib64/library.so.1").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "changed before"):
            artifact.install(self.bundle, self.root, self.record)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_live_root_and_destination_links_are_refused(self):
        with self.assertRaisesRegex(ValueError, "offline staging"):
            artifact.install(self.bundle, Path("/"), self.record)
        (self.root / "opt").symlink_to(self.directory, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "contains a symlink"):
            artifact.install(self.bundle, self.root, self.record)
        self.assertFalse((self.root / "usr").exists())

    def test_broken_or_escaping_library_links_are_refused(self):
        link = self.prefix / "lib64/library.so"
        link.unlink()
        for target in ("missing", "/etc/passwd", "../../../../../outside"):
            link.symlink_to(target)
            with self.assertRaisesRegex(ValueError, "escapes.*or is broken"):
                artifact.install(self.bundle, self.root, self.record)
            link.unlink()
        self.assertEqual(list(self.root.iterdir()), [])

    def test_modes_and_installed_metadata_are_verified(self):
        library = self.prefix / "lib64/library.so.1"
        library.chmod(0o4755)
        with self.assertRaisesRegex(ValueError, "type or mode"):
            artifact.install(self.bundle, self.root, self.record)
        library.chmod(0o755)
        artifact.install(self.bundle, self.root, self.record)
        (self.root / artifact.BUILD_RECORD).write_text("modified")
        with self.assertRaisesRegex(ValueError, "build record"):
            artifact.verify_install(self.root, self.record)

    def test_package_rejects_noncompiled_or_physical_claims(self):
        for report in ({}, {"status": "compiled", "hardware_verified": True, "flash_ready": False},
                       {"status": "compiled", "hardware_verified": False, "flash_ready": True}):
            (self.bundle / "build.json").write_text(json.dumps(report))
            with self.assertRaisesRegex(ValueError, "offline compiled"):
                artifact.package(self.bundle)

    def test_loader_requires_complete_symbol_resolution(self):
        for code, text in ((1, "failed"), (0, "not found"), (0, "undefined symbol: rpc"), (0, "")):
            with patch.object(artifact.subprocess, "run") as run:
                run.return_value.returncode = code
                run.return_value.stdout = text
                run.return_value.stderr = ""
                with self.assertRaisesRegex(ValueError, "loader or relocations"):
                    artifact.check_loader(self.root)
