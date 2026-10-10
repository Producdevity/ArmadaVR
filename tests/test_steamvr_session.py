import importlib.util
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("steamvr_session", Path(__file__).parents[1] / "tools/steamvr-session.py")
session = importlib.util.module_from_spec(spec)
spec.loader.exec_module(session)


class SessionTests(unittest.TestCase):
    def client_fixture(self, root):
        client = root / ".local/share/Steam"
        runtime = client / "steamapps/common/SteamVR"
        (runtime / "bin/linux64").mkdir(parents=True)
        (runtime / "bin/linux64/vrclient.so").write_bytes(b"runtime")
        (client / "steamapps/appmanifest_250820.acf").write_text('"AppState" {}\n')
        (client / "steam.sh").write_text("#!/bin/bash\n")
        fex_root = root / "FEX-Emu/usr"
        paths = ("bin/FEX", "lib/aarch64-linux-gnu/fex-emu/HostThunks/libGL-host.so",
                 "lib/aarch64-linux-gnu/fex-emu/HostThunks/libvulkan-host.so",
                 "share/fex-emu/GuestThunks/libGL-guest.so", "share/fex-emu/GuestThunks/libvulkan-guest.so")
        for name in paths:
            p = fex_root / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(b"fixture")
        (fex_root.parent / "emulator.json").write_text("{}\n")
        bwrap = root / "bwrap"
        bwrap.write_bytes(b"\x7fELF\x02\x01" + bytes(12) + b"\xb7\x00")
        return (root / "client-run", client, fex_root / "bin/FEX", root / "rootfs",
                root / "icd.json", root / "renderD128", root / "registry.json", runtime, bwrap)

    def test_client_graphics_preserve_repeated_host_settings_and_browser_scope(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            root = Path(temporary)
            args = self.client_fixture(root)
            with mock.patch.object(session.Path, "home", return_value=root), mock.patch.object(
                    session, "FEX_BROWSER_CACHE_SHA256", hashlib.sha256(b"fixture").hexdigest()), mock.patch.dict(
                    os.environ, {"XDG_RUNTIME_DIR": temporary, "FEX_ENV": "LD_PRELOAD=wrong",
                                 "FEX_APP_DATA_LOCATION": "/wrong", "FEX_SERVERSOCKETPATH": "wrong",
                                 "FEX_ROOTFS": "/wrong", "LD_LIBRARY_PATH": "/wrong"}):
                command, env = session.prepare_steam_client(*args)
            self.assertEqual(command[:3], [str(args[2]), "/bin/bash", str(args[1] / "steam.sh")])
            self.assertNotIn("FEX_ENV", env)
            self.assertNotIn("FEX_ROOTFS", env)
            self.assertNotIn("LD_LIBRARY_PATH", env)
            self.assertNotIn("FEX_APP_DATA_LOCATION", env)
            self.assertNotIn("FEX_SERVERSOCKETPATH", env)
            pairs = json.loads(Path(env["FEX_APP_CONFIG"]).read_text(), object_pairs_hook=list)
            settings = dict(pairs)
            host = [value for key, value in settings["Config"] if key == "HostEnv"]
            self.assertEqual(len(host), 6)
            self.assertIn("VK_DRIVER_FILES=" + str(args[4]), host)
            self.assertIn("LVP_DRM_SYNC=" + str(args[5]), host)
            browser = dict(settings["AppOverrides"])["steamwebhelper"]
            self.assertEqual(browser, [("HostEnv", "MESA_LOADER_DRIVER_OVERRIDE=zink"),
                                       ("HostEnv", "LIBGL_KOPPER_DRI2=true")])
            config = args[0] / "fex"
            self.assertEqual(json.loads((config / "AppConfig/steam.json").read_text())["ThunksDB"]["GL"], "0")
            helper = json.loads((config / "AppConfig/steamwebhelper.json").read_text())
            self.assertEqual(helper["ThunksDB"]["GL"], "1")
            self.assertEqual(helper["Config"]["DynamicL1CacheDecreaseCountHeuristic"], "0")
            self.assertEqual(Path(env["PRESSURE_VESSEL_BWRAP"]).read_bytes(), args[-1].read_bytes())
            db = json.loads((config / "ThunksDB.json").read_text())["DB"]
            self.assertIn("/var/pressure-vessel/gfx/main/usr/lib/libvulkan.so.1.4.357", db["Vulkan"]["Overlay"])
            self.assertIn("/usr/lib/pressure-vessel/overrides/lib/x86_64-linux-gnu/libGL.so.1", db["GL"]["Overlay"])

    def test_client_refuses_other_profile_runtime_interpreter_and_helper(self):
        for invalid in ("home", "runtime", "interpreter", "helper"):
            with self.subTest(invalid=invalid), tempfile.TemporaryDirectory(dir="/tmp") as temporary:
                root = Path(temporary)
                args = list(self.client_fixture(root))
                expected = "Steam client"
                home = root
                if invalid == "home":
                    home = root / "other-home"
                elif invalid == "runtime":
                    other = root / "other-runtime/bin/linux64"
                    other.mkdir(parents=True)
                    (other / "vrclient.so").write_bytes(b"runtime")
                    args[7] = other.parents[1]
                elif invalid == "interpreter":
                    args[2].write_bytes(b"different")
                    expected = "verified FEX"
                else:
                    args[-1].write_bytes(b"\x7fELF\x02\x01" + bytes(12) + b"\x3e\x00")
                    expected = "native AArch64"
                with mock.patch.object(session.Path, "home", return_value=home), mock.patch.object(
                        session, "FEX_BROWSER_CACHE_SHA256", hashlib.sha256(b"fixture").hexdigest()):
                    with self.assertRaisesRegex(ValueError, expected):
                        session.prepare_steam_client(*args)
                self.assertFalse(args[0].exists())

    def test_fex_server_isolates_sessions_and_changed_rootfs(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary, mock.patch.dict(
                os.environ, {"XDG_RUNTIME_DIR": temporary}):
            root = Path(temporary)
            first = session.prepare_fex_server(root / "session-a", root / "rootfs-a")
            self.assertEqual(first, session.prepare_fex_server(root / "session-a", root / "rootfs-a"))
            for config, rootfs in (("session-b", "rootfs-a"), ("session-a", "rootfs-b")):
                other = session.prepare_fex_server(root / config, root / rootfs)
                for key in first:
                    self.assertNotEqual(first[key], other[key])
            self.assertEqual(Path(first["FEX_APP_DATA_LOCATION"]).stat().st_mode & 0o777, 0o700)

    def test_fex_server_refuses_linked_or_shared_runtime_paths(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            root = Path(temporary)
            runtime = root / "runtime"
            runtime.mkdir()
            linked = root / "linked"
            linked.symlink_to(runtime, target_is_directory=True)
            with mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": str(linked)}):
                with self.assertRaisesRegex(ValueError, "owned runtime"):
                    session.prepare_fex_server(root / "config", root / "rootfs")
            parent = runtime / "armada-vr-fex"
            parent.mkdir(mode=0o755)
            with mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": str(runtime)}):
                with self.assertRaisesRegex(ValueError, "private and owned"):
                    session.prepare_fex_server(root / "config", root / "rootfs")
            parent.rmdir()
            parent.symlink_to(root, target_is_directory=True)
            with mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": str(runtime)}):
                with self.assertRaisesRegex(ValueError, "regular owned"):
                    session.prepare_fex_server(root / "config", root / "rootfs")

    def test_fex_server_refuses_socket_path_fallback(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            runtime = Path(temporary) / ("x" * 100)
            runtime.mkdir()
            with mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": str(runtime)}):
                with self.assertRaisesRegex(ValueError, "socket limit"):
                    session.prepare_fex_server(runtime / "config", runtime / "rootfs")
            self.assertEqual(list(runtime.iterdir()), [])

    def test_browser_cache_guard_only_targets_the_verified_interpreter(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary)
            self.assertFalse(session.prepare_browser_config(config, "another-build"))
            self.assertEqual(list(config.iterdir()), [])
            self.assertTrue(session.prepare_browser_config(config, session.FEX_BROWSER_CACHE_SHA256))
            path = config / "AppConfig/vrwebhelper.json"
            self.assertEqual(json.loads(path.read_text()), {
                "Config": {"DynamicL1CacheDecreaseCountHeuristic": "0"}})
            self.assertEqual([p.name for p in path.parent.iterdir()], ["vrwebhelper.json"])

    def test_browser_cache_guard_preserves_compatible_existing_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary)
            (config / "AppConfig").mkdir()
            path = config / "AppConfig/vrwebhelper.json"
            original = b'{"ThunksDB":{"GL":0},"Config":{"DynamicL1CacheDecreaseCountHeuristic":"0","Multiblock":"1"}}\n'
            path.write_bytes(original)
            before = path.stat()
            self.assertTrue(session.prepare_browser_config(config, session.FEX_BROWSER_CACHE_SHA256))
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual(path.stat().st_mtime_ns, before.st_mtime_ns)

    def test_browser_cache_guard_refuses_conflicts_without_rewriting_files(self):
        for settings in ({"Config": {"DynamicL1CacheDecreaseCountHeuristic": "50"}},
                         {"Config": []}, [], {"ThunksDB": {"GL": 0}}):
            with self.subTest(settings=settings), tempfile.TemporaryDirectory() as temporary:
                config = Path(temporary)
                (config / "AppConfig").mkdir()
                path = config / "AppConfig/vrwebhelper.json"
                path.write_text(json.dumps(settings))
                original = path.read_bytes()
                with self.assertRaisesRegex(ValueError, "conflicts"):
                    session.prepare_browser_config(config, session.FEX_BROWSER_CACHE_SHA256)
                self.assertEqual(path.read_bytes(), original)
        with tempfile.TemporaryDirectory() as temporary, mock.patch.dict(
                os.environ, {"FEX_DYNAMICL1CACHEDECREASECOUNTHEURISTIC": "50"}):
            config = Path(temporary)
            with self.assertRaisesRegex(ValueError, "Inherited"):
                session.prepare_browser_config(config, session.FEX_BROWSER_CACHE_SHA256)
            self.assertEqual(list(config.iterdir()), [])

    def test_browser_cache_guard_refuses_linked_config_paths(self):
        for linked in ("directory", "file"):
            with self.subTest(linked=linked), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                config, external = root / "config", root / "external"
                config.mkdir()
                external.mkdir()
                if linked == "directory":
                    (config / "AppConfig").symlink_to(external, target_is_directory=True)
                else:
                    (config / "AppConfig").mkdir()
                    (config / "AppConfig/vrwebhelper.json").symlink_to(external / "untouched.json")
                with self.assertRaisesRegex(ValueError, "regular paths"):
                    session.prepare_browser_config(config, session.FEX_BROWSER_CACHE_SHA256)
                self.assertEqual(list(external.iterdir()), [])

    def test_stop_removes_descendants_after_the_group_leader_exits(self):
        for early_exit in (False, True):
            with self.subTest(early_exit=early_exit):
                child = "import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); print('ready',flush=True); time.sleep(30)"
                parent = ("import subprocess,sys,time; subprocess.Popen([sys.executable,'-c',sys.argv[1]]); "
                          + ("sys.exit(0)" if early_exit else "time.sleep(30)"))
                process = subprocess.Popen([sys.executable, "-c", parent, child], start_new_session=True,
                                           stdout=subprocess.PIPE, text=True)
                cleaned = False
                try:
                    self.assertTrue(select.select([process.stdout], [], [], 5)[0])
                    self.assertEqual(process.stdout.readline(), "ready\n")
                    if early_exit:
                        self.assertEqual(process.wait(timeout=5), 0)
                    session.stop(process)
                    self.assertIsNotNone(process.poll())
                    self.assertTrue(select.select([process.stdout], [], [], 2)[0], "Descendant kept the pipe open")
                    self.assertEqual(process.stdout.read(), "")
                    cleaned = True
                finally:
                    if not cleaned:
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    process.wait(timeout=5)
                    process.stdout.close()

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
                     "armada_virtual/resources/rendermodels/controller/controller.json",
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

    def native_presentation_fixture(self, root):
        runtime, bundle = self.native_fixture(root)
        compositor = runtime / "bin/linuxarm64/vrcompositor"
        (bundle / "steamvr-presentation.json").write_text(json.dumps({"architecture": "aarch64",
            "compositor_sha256": hashlib.sha256(compositor.read_bytes()).hexdigest()}))
        self.native_manifest(bundle)
        return runtime, bundle

    def test_native_presentation_keeps_device_profile_and_original_runtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            runtime, bundle = self.native_presentation_fixture(Path(temporary))
            settings = session.validate_native_inputs(runtime, bundle)
            original = (bundle / "steamvr-virtual.json").read_bytes()
            configured = session.prepare_native_presentation(runtime, bundle, settings)
            self.assertTrue(configured["steamvr"]["startCompositorFromAppLaunch"])
            self.assertTrue(configured["steamvr"]["enableLinuxVulkanAsync"])
            self.assertFalse(settings["steamvr"]["startCompositorFromAppLaunch"])
            self.assertFalse(configured["steamvr"]["activateMultipleDrivers"])
            self.assertTrue(configured["driver_armada_virtual"]["simulateHeadset"])
            self.assertEqual((bundle / "steamvr-virtual.json").read_bytes(), original)
            perception = runtime / "drivers/cv/bin/linuxarm64/libArcturusPerception.so"
            perception.parent.mkdir(parents=True)
            perception.write_bytes(b"preserve original")
            with self.assertRaisesRegex(ValueError, "disposable virtual-test runtime"):
                session.prepare_native_presentation(runtime, bundle, settings)
            self.assertEqual(perception.read_bytes(), b"preserve original")

    def test_native_presentation_refuses_unknown_compositor_and_unchecked_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            runtime, bundle = self.native_presentation_fixture(Path(temporary))
            settings = session.validate_native_inputs(runtime, bundle)
            (runtime / "bin/linuxarm64/vrcompositor").write_bytes(b"different compositor")
            with self.assertRaisesRegex(ValueError, "verified Frame ARM64 compositor"):
                session.prepare_native_presentation(runtime, bundle, settings)
            manifest = json.loads((bundle / "build.json").read_text())
            del manifest["sha256"]["steamvr-presentation.json"]
            (bundle / "build.json").write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "checked runtime profile"):
                session.prepare_native_presentation(runtime, bundle, settings)

    def test_native_monitor_selects_arm64_qt_without_changing_compositor_environment(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            runtime, _bundle = self.native_fixture(root)
            qt = root / "qt"
            for name in ("lib/libQt5Core.so.5", "lib/libQt5Gui.so.5", "lib/libQt5Widgets.so.5",
                         "plugins/platforms/libqxcb.so"):
                path = qt / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes((runtime / "bin/linuxarm64/vrclient.so").read_bytes())
            (runtime / "bin/linuxarm64/vrmonitor").write_text("fixture")
            helper = runtime / "bin/vrwebhelper/linuxarm64/vrwebhelper.sh"
            helper.parent.mkdir(parents=True)
            helper.write_text("fixture")
            env = {"LD_LIBRARY_PATH": "/runtime/lib", "VK_DRIVER_FILES": "/icd.json"}
            selected = session.native_monitor_environment(runtime, qt, env)
            self.assertEqual(selected["LD_LIBRARY_PATH"], f"{qt / 'lib'}:/runtime/lib")
            self.assertEqual(selected["QT_QPA_PLATFORM_PLUGIN_PATH"], str(qt / "plugins"))
            self.assertEqual(env["LD_LIBRARY_PATH"], "/runtime/lib")
            self.assertEqual(selected["VK_DRIVER_FILES"], "/icd.json")
            plugin = qt / "plugins/platforms/libqxcb.so"
            plugin.write_bytes(plugin.read_bytes()[:18] + b"\x3e\x00")
            with self.assertRaisesRegex(ValueError, "not AArch64"):
                session.native_monitor_environment(runtime, qt, env)
            plugin.unlink()
            plugin.symlink_to(runtime / "bin/linuxarm64/vrclient.so")
            with self.assertRaisesRegex(ValueError, "external native Qt"):
                session.native_monitor_environment(runtime, qt, env)

    def test_native_bundle_rejects_modified_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            runtime, bundle = self.native_fixture(Path(temporary))
            (bundle / "steamvr-probe").write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                session.validate_native_inputs(runtime, bundle)

    def test_native_bundle_requires_controller_pose_resource(self):
        with tempfile.TemporaryDirectory() as temporary:
            runtime, bundle = self.native_fixture(Path(temporary))
            (bundle / "armada_virtual/resources/rendermodels/controller/controller.json").unlink()
            self.native_manifest(bundle)
            with self.assertRaisesRegex(ValueError, "missing its checked session dependencies"):
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
