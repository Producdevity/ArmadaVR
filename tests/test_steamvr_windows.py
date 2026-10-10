import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


REPO = Path(__file__).parents[1]
spec = importlib.util.spec_from_file_location('windows', REPO / 'tools/run-steamvr-windows.py')
windows = importlib.util.module_from_spec(spec)
spec.loader.exec_module(windows)
ARM = b'\x7fELF\x02\x01' + bytes(12) + b'\xb7\x00'
X86 = b'\x7fELF\x02\x01' + bytes(12) + b'\x3e\x00'


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


class WindowsTests(unittest.TestCase):
    def session_fixture(self, root, native=True):
        runtime, bundle, process = root / 'runtime', root / 'bundle', root / 'process'
        for name in ('vrserver', 'vrcompositor', 'vrclient.so', 'libopenvr_api.so'):
            write(runtime / 'bin/linuxarm64' / name, ARM)
        write(runtime / 'bin/linux64/vrclient.so', X86)
        for arch in ('linux64', 'linuxarm64'):
            write(runtime / f'steamxr_{arch}.json', json.dumps({'runtime': {
                'library_path': f'bin/{arch}/vrclient.so'}}).encode())
        for name in ('steamvr-probe', 'vulkan-interop', 'vulkan-external-sync', 'steamvr-session.py',
                     'armada_virtual/driver.vrdrivermanifest', 'armada_virtual/bin/linuxarm64/driver_armada_virtual.so',
                     'armada_virtual/resources/rendermodels/controller/controller.json'):
            write(bundle / name, ARM)
        write(bundle / 'steamvr-virtual.json', (REPO / 'profiles/steamvr-native-virtual.json').read_bytes())
        write(bundle / 'steamvr-presentation.json', json.dumps({'architecture': 'aarch64',
            'compositor_sha256': hashlib.sha256(ARM).hexdigest(),
            'client_x86_sha256': hashlib.sha256(X86).hexdigest()}).encode())
        hashes = {p.relative_to(bundle).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in bundle.rglob('*') if p.is_file()}
        write(bundle / 'build.json', json.dumps({'target': 'steamvr-virtual-aarch64', 'sha256': hashes}).encode())
        state = root / '.local/state/armada-vr' / ('native-fixture' if native else 'steamvr-virtual')
        state.mkdir(parents=True, mode=0o700)
        registry = state / 'openvrpaths.vrpath'
        write(registry, json.dumps({'runtime': [str(runtime)], 'config': [str(state / 'config')],
            'external_drivers': [str(bundle / 'armada_virtual')]}).encode())
        settings = json.loads((bundle / 'steamvr-virtual.json').read_text())
        settings['steamvr'].update(enableLinuxVulkanAsync=True, startCompositorFromAppLaunch=True)
        if not native:
            settings['steamvr']['forcedDriver'] = 'null'
            settings['driver_null'] = {'enable': True}
        write(state / 'config/steamvr.vrsettings', json.dumps(settings).encode())
        env = {'HOME': str(root), 'VR_PATHREG_OVERRIDE': str(registry), 'VR_CONFIG_PATH': str(state / 'config'),
               'LD_LIBRARY_PATH': '/runtime/bin/linuxarm64', 'FEX_ENV': 'LD_PRELOAD=/old.so',
               'LVP_VIRTUAL_DISPLAY': '1', 'VK_DRIVER_FILES': '/mesa/icd.json', 'DISPLAY': ':89'}
        write(process / 'environ', b'\0'.join(f'{k}={v}'.encode() for k, v in env.items()))
        executable = runtime / 'bin/linuxarm64/vrcompositor' if native else write(root / 'FEX', ARM)
        (process / 'exe').symlink_to(executable)
        return runtime, bundle, process, state

    def test_native_session_selects_x86_client_and_scopes_display_flag(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            runtime, bundle, process, _state = self.session_fixture(root)
            with mock.patch.object(windows.Path, 'home', return_value=root):
                fex, manifest, env = windows.session_details(process, bundle)
            self.assertIsNone(fex)
            self.assertEqual(manifest, runtime / 'steamxr_linux64.json')
            self.assertEqual(env['LVP_VIRTUAL_DISPLAY'], '0')
            self.assertEqual(env['LD_LIBRARY_PATH'], str(runtime / 'bin/linux64'))
            self.assertEqual(env['DISPLAY'], ':89')
            self.assertNotIn('FEX_ENV', env)
            self.assertIn(b'LVP_VIRTUAL_DISPLAY=1', (process / 'environ').read_bytes())

    def test_native_session_refuses_wrong_driver_client_process_and_shared_registry(self):
        for wrong in ('driver', 'client', 'process', 'permissions', 'config'):
            with self.subTest(wrong=wrong), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                runtime, bundle, process, state = self.session_fixture(root)
                if wrong == 'driver':
                    p = state / 'config/steamvr.vrsettings'
                    s = json.loads(p.read_text()); s['driver_armada_virtual']['simulateHeadset'] = False
                    p.write_text(json.dumps(s))
                elif wrong == 'client':
                    (runtime / 'bin/linux64/vrclient.so').write_bytes(X86 + b'different')
                elif wrong == 'process':
                    (process / 'exe').unlink(); (process / 'exe').symlink_to(runtime / 'bin/linuxarm64/vrserver')
                elif wrong == 'permissions':
                    state.chmod(0o755)
                else:
                    p = process / 'environ'; p.write_bytes(p.read_bytes().replace(b'VR_CONFIG_PATH=', b'OTHER_CONFIG_PATH='))
                with mock.patch.object(windows.Path, 'home', return_value=root), self.assertRaises(ValueError):
                    windows.session_details(process, bundle)

    def test_translated_session_still_requires_its_original_registry_and_null_driver(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            runtime, _bundle, process, state = self.session_fixture(root, native=False)
            with mock.patch.object(windows.Path, 'home', return_value=root):
                fex, manifest, env = windows.session_details(process)
                self.assertEqual(fex, root / 'FEX')
                self.assertEqual(manifest, runtime / 'steamxr_linux64.json')
                self.assertEqual(env['FEX_ENV'], 'LD_PRELOAD=/old.so')
                s = state / 'config/steamvr.vrsettings'; settings = json.loads(s.read_text())
                settings['steamvr']['forcedDriver'] = 'physical'; s.write_text(json.dumps(settings))
                with self.assertRaisesRegex(ValueError, 'virtual null headset'):
                    windows.session_details(process)

    def translation_fixture(self, root):
        fex = write(root / 'fex/usr/bin/FEX', ARM)
        write(root / 'fex/usr/lib/aarch64-linux-gnu/fex-emu/HostThunks/libvulkan-host.so', ARM)
        write(root / 'fex/usr/share/fex-emu/GuestThunks/libvulkan-guest.so', X86)
        rootfs = root / 'rootfs'
        for name in ('libc.so.6', 'libvulkan.so', 'libvulkan.so.1', 'libvulkan.so.1.4.357'):
            write(rootfs / 'usr/lib' / name, X86)
        resolver = write(root / 'resolver.so', X86)
        temporary = root / 'private'; temporary.mkdir()
        session = windows.session_tools()
        session.FEX_BROWSER_CACHE_SHA256 = hashlib.sha256(ARM).hexdigest()
        return temporary, fex, rootfs, resolver, session

    def test_native_translation_uses_private_config_and_matching_thunks(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            directory, fex, rootfs, resolver, session = self.translation_fixture(root)
            original = {'LVP_VIRTUAL_DISPLAY': '0', 'DISPLAY': ':89'}
            with mock.patch.object(windows, 'session_tools', return_value=session):
                interpreter, env = windows.prepare_native_translation(directory, fex, rootfs, resolver, original)
            self.assertEqual(interpreter, fex)
            config = json.loads((directory / 'config/Config.json').read_text())
            self.assertEqual(config['Config']['RootFS'], str(rootfs))
            self.assertEqual(config['Config']['ThunkGuestLibs'], str(fex.parents[1] / 'share/fex-emu/GuestThunks'))
            db = json.loads((directory / 'config/ThunksDB.json').read_text())['DB']['Vulkan']
            self.assertIn('/usr/lib/libvulkan.so.1.4.357', db['Overlay'])
            self.assertEqual(env['FEX_ENV'], 'LD_PRELOAD=' + str(resolver))
            self.assertEqual(env['LVP_VIRTUAL_DISPLAY'], '0')
            self.assertNotIn('FEX_ENV', original)
            self.assertEqual((directory / 'config').stat().st_mode & 0o777, 0o700)

    def test_native_translation_refuses_wrong_architecture_before_writing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            directory, fex, rootfs, resolver, session = self.translation_fixture(root)
            resolver.write_bytes(ARM)
            with mock.patch.object(windows, 'session_tools', return_value=session):
                with self.assertRaisesRegex(ValueError, 'x86-64 ELF'):
                    windows.prepare_native_translation(directory, fex, rootfs, resolver, {})
            self.assertEqual(list(directory.iterdir()), [])

    def test_rejected_existing_prefix_does_not_start_or_stop_wine(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            proton = root / 'proton'
            for name in ('wine', 'wine-preloader', 'wineopenxr.so'):
                write(proton / 'lib/wine/x86_64-unix' / name, X86)
            compat = write(root / 'compat.so', X86)
            launcher = write(root / 'launcher.exe', b'MZ')
            prefix = root / 'prefix'; prefix.mkdir()
            registry = write(prefix / 'system.reg', b'preserve')
            args = SimpleNamespace(proton=proton, compat=compat, launcher=launcher, prefix=prefix, initialize=True)
            started = []
            with mock.patch.object(windows.subprocess, 'run') as run, mock.patch.object(windows.subprocess, 'call') as call:
                with self.assertRaisesRegex(ValueError, 'nonexistent prefix'):
                    windows.run_application(args, root / 'FEX', root / 'openxr.json', {'FEX_ENV': 'LD_PRELOAD=/resolver.so'}, started)
                run.assert_not_called(); call.assert_not_called()
            self.assertEqual(started, [])
            self.assertEqual(registry.read_bytes(), b'preserve')


if __name__ == '__main__':
    unittest.main()
