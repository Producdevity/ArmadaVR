import importlib.util
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("steamvr_session", Path(__file__).parents[1] / "tools/steamvr-session.py")
session = importlib.util.module_from_spec(spec)
spec.loader.exec_module(session)


class SessionTests(unittest.TestCase):
    def selected_runtime_fixture(self, root):
        binaries = root / "bin/linux64"
        binaries.mkdir(parents=True)
        for name in ("vrserver", "vrcompositor", "vrmonitor", "vrclient.so"):
            (binaries / name).write_text("fixture")
        (root / "steamxr_linux64.json").write_text(json.dumps({
            "runtime": {"library_path": "bin/linux64/vrclient.so"}}))
        return root

    def test_selected_runtime_rejects_manifest_link_into_another_installation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            selected = self.selected_runtime_fixture(root / "selected")
            managed = self.selected_runtime_fixture(root / "managed")
            manifest = selected / "steamxr_linux64.json"
            manifest.unlink()
            manifest.symlink_to(managed / manifest.name)
            with self.assertRaisesRegex(ValueError, "manifest must resolve"):
                session.validate_runtime_paths(selected)

    def test_selected_runtime_rejects_external_client_and_wrong_manifest_client(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            selected = self.selected_runtime_fixture(root / "selected")
            managed = self.selected_runtime_fixture(root / "managed")
            client = selected / "bin/linux64/vrclient.so"
            client.unlink()
            client.symlink_to(managed / "bin/linux64/vrclient.so")
            with self.assertRaisesRegex(ValueError, "component resolves outside"):
                session.validate_runtime_paths(selected)
            client.unlink()
            client.write_text("fixture")
            (selected / "steamxr_linux64.json").write_text(json.dumps({
                "runtime": {"library_path": str(managed / "bin/linux64/vrclient.so")}}))
            with self.assertRaisesRegex(ValueError, "does not select"):
                session.validate_runtime_paths(selected)

    def test_selected_runtime_accepts_regular_manifest_and_internal_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            selected = self.selected_runtime_fixture(Path(temporary) / "selected")
            compositor = selected / "bin/linux64/vrcompositor"
            compositor.rename(compositor.with_suffix(".original"))
            compositor.symlink_to("vrcompositor.original")
            session.validate_runtime_paths(selected)

    def runtime_fixture(self, root):
        bundle, client, directory = (root / name for name in ("bundle", "client", "session"))
        for path in (bundle, client, directory):
            path.mkdir()
        payload = b"\x7fELF\x02\x01" + bytes(12) + b"\x3e\x00"
        (client / "library.so").write_bytes(payload)
        profile = {"libraries": [{"name": "library.so", "source": "library.so",
                                  "sha256": hashlib.sha256(payload).hexdigest()}]}
        (bundle / "steamvr-runtime.json").write_text(json.dumps(profile))
        return bundle, client, directory, profile

    def test_runtime_uses_verified_library_without_copying_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            bundle, client, directory, _ = self.runtime_fixture(Path(temporary))
            libraries = session.prepare_runtime_libraries(bundle, client, directory)
            self.assertEqual((libraries / "library.so").resolve(), (client / "library.so").resolve())
            self.assertTrue((libraries / "library.so").is_symlink())

    def test_changed_or_wrong_architecture_runtime_is_rejected_before_linking(self):
        for machine in (b"\x3e\x00", b"\xb7\x00"):
            with self.subTest(machine=machine), tempfile.TemporaryDirectory() as temporary:
                bundle, client, directory, _ = self.runtime_fixture(Path(temporary))
                (client / "library.so").write_bytes(b"\x7fELF\x02\x01" + bytes(12) + machine + b"changed")
                with self.assertRaisesRegex(ValueError, "checksum mismatch|not x86-64"):
                    session.prepare_runtime_libraries(bundle, client, directory)
                self.assertFalse((directory / "libraries").exists())

    def test_runtime_manifest_cannot_escape_library_directory(self):
        for field in ("name", "source"):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                bundle, client, directory, profile = self.runtime_fixture(Path(temporary))
                profile["libraries"][0][field] = "../outside.so"
                (bundle / "steamvr-runtime.json").write_text(json.dumps(profile))
                with self.assertRaisesRegex(ValueError, "Invalid.*path"):
                    session.prepare_runtime_libraries(bundle, client, directory)
                self.assertFalse((directory / "libraries").exists())

    def virtual_fixture(self, root):
        bundle, binaries, state = (root / name for name in ("bundle", "runtime/bin/linux64", "state"))
        binaries.mkdir(parents=True)
        (binaries / "vrcompositor").write_bytes(b"fixture compositor")
        bundle.mkdir()
        profile = {"compositor_sha256": hashlib.sha256(b"fixture compositor").hexdigest()}
        (bundle / "steamvr-presentation.json").write_text(json.dumps(profile))
        (bundle / "steamvr-virtual.json").write_text(json.dumps({
            "steamvr": {"forcedDriver": "null"}, "driver_null": {"enable": True}}))
        for name in ("xpresent/libXpresent.so.1", "libvulkan-procaddr.so", "steamvr-probe", "armada_virtual/driver.vrdrivermanifest"):
            path = bundle / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture")
        return bundle, binaries, state

    def test_changed_compositor_cannot_receive_version_specific_bootstrap(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle, binaries, state = self.virtual_fixture(Path(directory))
            (binaries / "vrcompositor").write_bytes(b"another build")
            with self.assertRaisesRegex(ValueError, "verified SteamVR"):
                session.prepare_virtual_session(bundle, binaries, state)
            self.assertFalse(state.exists())

    def test_bootstrap_rejects_physical_driver_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle, binaries, state = self.virtual_fixture(Path(directory))
            (bundle / "steamvr-virtual.json").write_text('{"steamvr":{"forcedDriver":"lighthouse"}}')
            with self.assertRaisesRegex(ValueError, "null headset"):
                session.prepare_virtual_session(bundle, binaries, state)
            self.assertFalse(state.exists())

    def test_virtual_registry_is_separate_and_resets_driver_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle, binaries, state = self.virtual_fixture(Path(directory))
            registry, log = session.prepare_virtual_session(bundle, binaries, state)
            configuration = state / "steamvr-virtual/config/steamvr.vrsettings"
            configuration.write_text('{"steamvr":{"forcedDriver":"lighthouse"},"GpuSpeed":{"gpuSpeed0":1}}')
            session.prepare_virtual_session(bundle, binaries, state)
            settings = json.loads(configuration.read_text())
            self.assertEqual(settings["steamvr"]["forcedDriver"], "null")
            self.assertEqual(settings["GpuSpeed"], {"gpuSpeed0": 1})
            paths = json.loads(registry.read_text())
            self.assertEqual(paths["runtime"], [str(binaries.parent.parent)])
            self.assertEqual(paths["log"], [str(log.parent)])
            self.assertFalse((binaries.parent.parent / "config").exists())

    def test_old_completion_cannot_satisfy_new_startup(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "compositor.log"
            log.write_text("Startup Complete (1 seconds)\n")
            process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"], start_new_session=True)
            try:
                with self.assertRaises(TimeoutError):
                    session.wait_for_startup([process], log, log.stat().st_size, 0.1)
            finally:
                session.stop(process)
            self.assertIsNotNone(process.poll())

    def test_new_completion_and_process_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "compositor.log"
            program = ("import pathlib, sys, time; time.sleep(.1); "
                       "pathlib.Path(sys.argv[1]).write_text('Startup Complete (2 seconds)'); time.sleep(30)")
            process = subprocess.Popen([sys.executable, "-c", program, str(log)], start_new_session=True)
            try:
                session.wait_for_startup([process], log, 0, 5)
            finally:
                session.stop(process)
            with self.assertRaisesRegex(RuntimeError, "exited"):
                session.wait_for_startup([process], log, 0, 1)

    def test_rotated_log_is_read(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "compositor.log"
            log.write_text("Startup Complete (2 seconds)")
            process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"], start_new_session=True)
            try:
                session.wait_for_startup([process], log, 4096, 1)
            finally:
                session.stop(process)

    def test_monitor_readiness_requires_its_own_completion_message(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "monitor.log"
            marker = b"to 'SteamVRSystemState_Ready'."
            log.write_text("Startup Complete (2 seconds)")
            process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"], start_new_session=True)
            try:
                with self.assertRaises(TimeoutError):
                    session.wait_for_startup([process], log, 0, .1, marker=marker)
                log.write_bytes(b"Transition from 'SteamVRSystemState_Connecting' " + marker)
                session.wait_for_startup([process], log, 0, 1, marker=marker)
            finally:
                session.stop(process)
