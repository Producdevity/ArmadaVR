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
    def native_fixture(self, root):
        runtime, bundle = root / "runtime", root / "bundle"
        binaries = runtime / "bin/linuxarm64"
        binaries.mkdir(parents=True)
        payload = b"\x7fELF\x02\x01" + bytes(12) + b"\xb7\x00"
        for name in ("vrserver", "vrcompositor", "vrclient.so", "libopenvr_api.so"):
            (binaries / name).write_bytes(payload)
        (runtime / "steamxr_linuxarm64.json").write_text(json.dumps({
            "runtime": {"library_path": "bin/linuxarm64/vrclient.so"}}))
        for name in ("steamvr-probe", "vulkan-interop", "vulkan-external-sync", "steamvr-session.py",
                     "armada_virtual/driver.vrdrivermanifest", "armada_virtual/bin/linuxarm64/driver_armada_virtual.so"):
            path = bundle / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)
        profile = Path(__file__).parents[1] / "profiles/steamvr-native-virtual.json"
        (bundle / "steamvr-virtual.json").write_bytes(profile.read_bytes())
        self.native_manifest(bundle)
        return runtime, bundle

    def native_manifest(self, bundle):
        hashes = {p.relative_to(bundle).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in bundle.rglob("*") if p.is_file() and p.name != "build.json"}
        (bundle / "build.json").write_text(json.dumps({"target": "steamvr-virtual-aarch64", "sha256": hashes}))

    def test_native_bundle_and_selected_runtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            runtime, bundle = self.native_fixture(Path(temporary))
            settings = session.validate_native_inputs(runtime, bundle)
            self.assertTrue(settings["driver_armada_virtual"]["simulateHeadset"])
            self.assertFalse((runtime / "config").exists())

    def test_native_bundle_rejects_modified_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            runtime, bundle = self.native_fixture(Path(temporary))
            (bundle / "steamvr-probe").write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                session.validate_native_inputs(runtime, bundle)

    def test_native_bundle_rejects_external_artifact_and_wrong_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime, bundle = self.native_fixture(root)
            probe = bundle / "steamvr-probe"
            external = root / "probe"
            probe.rename(external)
            probe.symlink_to(external)
            with self.assertRaisesRegex(ValueError, "Invalid native bundle path"):
                session.validate_native_inputs(runtime, bundle)
            probe.unlink()
            probe.write_bytes(external.read_bytes())
            manifest = json.loads((bundle / "build.json").read_text())
            manifest["target"] = "steamvr-virtual-x86_64"
            (bundle / "build.json").write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "AArch64 virtual-device bundle"):
                session.validate_native_inputs(runtime, bundle)

    def test_native_bundle_rejects_physical_or_automatic_launch_profile(self):
        for section, name, value in (("steamvr", "forcedDriver", "lighthouse"),
                ("driver_armada_virtual", "simulateHeadset", False),
                ("steamvr", "startCompositorFromAppLaunch", True)):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                runtime, bundle = self.native_fixture(Path(temporary))
                settings = json.loads((bundle / "steamvr-virtual.json").read_text())
                settings[section][name] = value
                (bundle / "steamvr-virtual.json").write_text(json.dumps(settings))
                self.native_manifest(bundle)
                with self.assertRaisesRegex(ValueError, "simulated headset|automatic component launch"):
                    session.validate_native_inputs(runtime, bundle)

    def test_native_runtime_rejects_x86_client_or_external_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime, bundle = self.native_fixture(root)
            client = runtime / "bin/linuxarm64/vrclient.so"
            original = client.read_bytes()
            client.write_bytes(original[:18] + b"\x3e\x00")
            with self.assertRaisesRegex(ValueError, "not AArch64"):
                session.validate_native_inputs(runtime, bundle)
            client.write_bytes(original)
            manifest = runtime / "steamxr_linuxarm64.json"
            manifest.rename(root / "external.json")
            manifest.symlink_to(root / "external.json")
            with self.assertRaisesRegex(ValueError, "must resolve in"):
                session.validate_native_inputs(runtime, bundle)

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
