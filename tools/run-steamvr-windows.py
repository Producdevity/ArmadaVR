#!/usr/bin/env python3
"""Launch a Windows OpenXR application in the active virtual SteamVR session."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def active_session(native_bundle=None):
    processes = []
    for path in Path('/proc').glob('[0-9]*/comm'):
        try:
            if path.stat().st_uid == os.getuid() and path.read_text().strip() == 'vrcompositor':
                processes.append(path.parent)
        except FileNotFoundError:
            continue
    if len(processes) != 1:
        raise ValueError('Expected one SteamVR compositor owned by the current user')
    return session_details(processes[0], native_bundle)


def session_tools():
    spec = importlib.util.spec_from_file_location('steamvr_session', Path(__file__).with_name('steamvr-session.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def session_details(process, native_bundle=None):
    original = dict(entry.decode().split('=', 1) for entry in
                    (process / 'environ').read_bytes().split(b'\0') if b'=' in entry)
    state = (Path.home() / '.local/state/armada-vr').resolve()
    expected = Path(original.get('VR_PATHREG_OVERRIDE', '')).resolve(strict=True)
    if native_bundle is None:
        if expected != state / 'steamvr-virtual/openvrpaths.vrpath':
            raise ValueError('Expected the private virtual-headset registry')
    elif (expected.name != 'openvrpaths.vrpath' or expected.parent.parent != state
          or not expected.parent.name.startswith('native-') or expected.stat().st_uid != os.getuid()
          or expected.parent.stat().st_mode & 0o077):
        raise ValueError('Expected an owned private native session registry')
    registry = json.loads(expected.read_text())
    settings = json.loads((Path(registry['config'][0]) / 'steamvr.vrsettings').read_text())
    runtime = Path(registry['runtime'][0]).resolve(strict=True)
    executable = (process / 'exe').resolve(strict=True)
    if native_bundle is None:
        if settings.get('steamvr', {}).get('forcedDriver') != 'null' or not settings.get('driver_null', {}).get('enable'):
            raise ValueError('Translated sessions require the virtual null headset')
        if executable.name != 'FEX':
            raise ValueError('Expected the running compositor to use FEX')
        fex = executable
    else:
        bundle = native_bundle.resolve(strict=True)
        session = session_tools()
        configured = session.prepare_native_presentation(runtime, bundle, session.validate_native_inputs(runtime, bundle))
        steamvr, driver = settings.get('steamvr', {}), settings.get('driver_armada_virtual', {})
        if (any(steamvr.get(key) != configured['steamvr'][key] for key in
                ('forcedDriver', 'activateMultipleDrivers', 'enableLinuxVulkanAsync', 'startCompositorFromAppLaunch'))
            or driver.get('simulateHeadset') is not True or driver.get('enable') is not True):
            raise ValueError('Native Windows requires the active simulated presentation profile')
        if (executable != runtime / 'bin/linuxarm64/vrcompositor'
            or Path(registry['config'][0]).resolve() != expected.parent / 'config'
            or Path(original.get('VR_CONFIG_PATH', '')).resolve() != expected.parent / 'config'
            or registry.get('external_drivers') != [str(bundle / 'armada_virtual')]):
            raise ValueError('Native session components do not match its private registry')
        profile = json.loads((bundle / 'steamvr-presentation.json').read_text())
        with (runtime / 'bin/linux64/vrclient.so').open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != profile.get('client_x86_sha256'):
                raise ValueError('Native Windows requires the verified Frame x86 client')
        fex = None
    manifest = runtime / 'steamxr_linux64.json'
    if manifest.is_symlink() or (runtime / 'bin/linux64/vrclient.so').is_symlink():
        raise ValueError('SteamVR runtime files must stay inside the selected installation')
    library = json.loads(manifest.read_text())['runtime']['library_path']
    if (manifest.parent / library).resolve(strict=True) != runtime / 'bin/linux64/vrclient.so':
        raise ValueError('OpenXR manifest selects a different SteamVR client')
    names = ('HOME', 'USER', 'PATH', 'LANG', 'DISPLAY', 'XAUTHORITY', 'XDG_RUNTIME_DIR',
             'FEX_APP_CONFIG_LOCATION', 'FEX_PORTABLE', 'FEX_ENV', 'VK_DRIVER_FILES',
             'LVP_DRM_SYNC', 'LP_NUM_THREADS', 'LD_LIBRARY_PATH', 'VR_PATHREG_OVERRIDE',
             'VR_OVERRIDE', 'VR_CONFIG_PATH', 'VR_LOG_PATH', 'STEAMVR_TOOLSDIR',
             'STEAMVR_VRENV', 'MESA_VK_WSI_SW_PRESENT')
    env = {name: original[name] for name in names if name in original}
    if native_bundle is not None:
        env.update(LVP_VIRTUAL_DISPLAY='0', LD_LIBRARY_PATH=str(runtime / 'bin/linux64'))
        for name in ('FEX_APP_CONFIG_LOCATION', 'FEX_ENV', 'STEAMVR_VRENV'):
            env.pop(name, None)
    return fex, manifest, env


def prepare_native_translation(directory, fex, rootfs, resolver, env):
    if fex is None or rootfs is None or resolver is None:
        raise ValueError('Native Windows requires --fex, --fex-rootfs and --vulkan-compat')
    fex, rootfs, resolver = (path.resolve(strict=True) for path in (fex, rootfs, resolver))
    if hashlib.sha256(fex.read_bytes()).hexdigest() != session_tools().FEX_BROWSER_CACHE_SHA256:
        raise ValueError('Native Windows requires the verified FEX-2607-76 interpreter')
    host = fex.parent.parent / 'lib/aarch64-linux-gnu/fex-emu/HostThunks'
    guest = fex.parent.parent / 'share/fex-emu/GuestThunks'
    with (host / 'libvulkan-host.so').open('rb') as stream:
        header = stream.read(20)
    if header[:6] != b'\x7fELF\x02\x01' or header[18:20] != b'\xb7\x00':
        raise ValueError('FEX Vulkan host thunk must be AArch64')
    for path in (rootfs / 'usr/lib/libc.so.6', guest / 'libvulkan-guest.so', resolver):
        x86_library(path)
    if any(character in str(resolver) for character in ':;\n'):
        raise ValueError('Vulkan resolver path cannot contain preload separators')
    aliases = sorted({path.name for path in (rootfs / 'usr/lib').glob('libvulkan.so*')})
    if 'libvulkan.so.1' not in aliases:
        raise ValueError('The FEX rootfs is missing its Vulkan loader')
    config, data = directory / 'config', directory / 'data'
    config.mkdir(mode=0o700); data.mkdir(mode=0o700)
    (config / 'Config.json').write_text(json.dumps({'Config': {
        'RootFS': str(rootfs), 'ThunkHostLibs': str(host), 'ThunkGuestLibs': str(guest),
        'SilentLog': '1', 'OutputLog': 'stderr'}, 'ThunksDB': {'Vulkan': '1'}}) + '\n')
    (config / 'ThunksDB.json').write_text(json.dumps({'DB': {'Vulkan': {
        'Library': 'libvulkan-guest.so', 'Overlay': [prefix + '/' + name
            for prefix in ('/usr/lib', '/usr/lib/x86_64-linux-gnu') for name in aliases]}}}) + '\n')
    identity = hashlib.sha256(os.fsencode(directory)).hexdigest()[:16]
    return fex, env | {'FEX_APP_CONFIG_LOCATION': str(config) + '/', 'FEX_APP_DATA_LOCATION': str(data) + '/',
        'FEX_SERVERSOCKETPATH': 'armada-windows-' + identity, 'FEX_PORTABLE': '1',
        'FEX_ENV': 'LD_PRELOAD=' + str(resolver)}


def x86_library(path):
    with path.open('rb') as stream:
        header = stream.read(20)
    if header[:6] != b'\x7fELF\x02\x01' or header[18:20] != b'\x3e\x00':
        raise ValueError(f'Expected an x86-64 ELF file: {path}')


def copy_matching(source, target):
    if target.exists():
        if hashlib.sha256(target.read_bytes()).digest() != hashlib.sha256(source.read_bytes()).digest():
            raise ValueError(f'Existing prefix file differs from the selected Proton: {target}')
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)


def run(args):
    if os.getuid() == 0:
        raise ValueError('Run as the desktop user')
    fex, manifest, env = active_session(args.native_bundle)
    if args.native_bundle:
        with tempfile.TemporaryDirectory(prefix='armada-windows-', dir='/tmp') as temporary:
            fex, env = prepare_native_translation(Path(temporary), args.fex, args.fex_rootfs, args.vulkan_compat, env)
            started = []
            try:
                return run_application(args, fex, manifest, env, started)
            finally:
                if started:
                    server = [str(fex), str(args.proton.resolve() / 'bin/wineserver')]
                    try:
                        subprocess.run(server + ['-w'], env=env, check=True, timeout=15, restore_signals=False)
                    except subprocess.TimeoutExpired:
                        subprocess.run(server + ['-k'], env=env, check=True, timeout=5, restore_signals=False)
                        subprocess.run(server + ['-w'], env=env, check=True, timeout=10, restore_signals=False)
    if args.fex or args.fex_rootfs or args.vulkan_compat:
        raise ValueError('Explicit FEX options require --native-bundle')
    return run_application(args, fex, manifest, env)


def run_application(args, fex, manifest, env, started=None):
    proton = args.proton.resolve(strict=True)
    compat = args.compat.resolve(strict=True)
    launcher = args.launcher.resolve(strict=True)
    prefix = args.prefix.absolute()
    if prefix.is_symlink():
        raise ValueError('Use a dedicated prefix directory, not a symlink')
    wine = proton / 'lib/wine/x86_64-unix/wine'
    preloader = proton / 'lib/wine/x86_64-unix/wine-preloader'
    for path in (wine, preloader, proton / 'lib/wine/x86_64-unix/wineopenxr.so', compat):
        x86_library(path)
    preload = env.get('FEX_ENV', '')
    if not preload.startswith('LD_PRELOAD=') or ';' in preload or '\n' in preload:
        raise ValueError('Expected the active SteamVR Vulkan resolver preload')
    env.update({
        'FEX_ENV': preload + ':' + str(compat),
        'XR_RUNTIME_JSON': str(manifest), 'XR_LOADER_DEBUG': 'warn',
        'ARMADA_VR_OPENXR_ONLY': '1', 'WINEPREFIX': str(prefix),
        'WINELOADERNOEXEC': '1', 'WINELOADER': str(proton / 'bin/wine'),
        'WINESERVER': str(proton / 'bin/wineserver'),
        'WINEDLLPATH': str(proton / 'lib/wine') + ':' + str(proton / 'lib/vkd3d'),
        'WINEDEBUG': os.environ.get('WINEDEBUG', '-all'), 'WINEESYNC': '0', 'WINEFSYNC': '0',
        'WINEDLLOVERRIDES': 'winemenubuilder.exe,mscoree,mshtml=',
        'PATH': str(proton / 'bin') + ':/usr/bin:/bin',
        'LD_LIBRARY_PATH': ':'.join((str(proton / 'lib/x86_64-linux-gnu'),
                                    str(proton / 'lib/i386-linux-gnu'), env.get('LD_LIBRARY_PATH', ''))),
    })
    command = [str(fex), str(preloader), str(wine)]
    if args.initialize:
        if prefix.exists():
            raise ValueError('Fresh-prefix initialization requires a nonexistent prefix')
        prefix.parent.mkdir(parents=True, exist_ok=True)
        if started is not None:
            started.append(True)
        subprocess.run(command + ['wineboot.exe', '--init'], env=env,
                       check=True, timeout=120, restore_signals=False)
        subprocess.run([str(fex), str(proton / 'bin/wineserver'), '-w'], env=env,
                       check=True, timeout=120, restore_signals=False)
    if not (prefix / 'system.reg').is_file():
        raise ValueError('Prefix is not initialized; use --initialize for a new prefix')
    for name in ('libvkd3d-1.dll', 'libvkd3d-shader-1.dll', 'libvkd3d-utils-1.dll'):
        copy_matching(proton / 'share/default_pfx/drive_c/windows/system32' / name,
                      prefix / 'drive_c/windows/system32' / name)
    copy_matching(proton / 'share/openxr/wineopenxr64.json', prefix / 'drive_c/openxr/wineopenxr64.json')
    program = args.program.resolve(strict=True)
    if started is not None:
        started.append(True)
    return subprocess.call(command + ['Z:' + str(launcher), 'Z:' + str(program), *args.arguments],
                           env=env, restore_signals=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--proton', type=Path, required=True, help='x86-64 Proton files directory')
    parser.add_argument('--compat', type=Path,
                        default=Path(__file__).resolve().with_name('libopenxr-procaddr.so'),
                        help='x86-64 resolver library; defaults to the copy beside this launcher')
    parser.add_argument('--launcher', type=Path, required=True, help='Windows openxr-launcher.exe')
    parser.add_argument('--prefix', type=Path, required=True, help='Dedicated Windows OpenXR prefix')
    parser.add_argument('--initialize', action='store_true', help='Initialize a new prefix with wineboot')
    parser.add_argument('--native-bundle', type=Path, help='Checked bundle of the active native virtual presentation session')
    parser.add_argument('--fex', type=Path, help='Verified FEX interpreter for a native backend')
    parser.add_argument('--fex-rootfs', type=Path, help='Mounted x86-64 FEX root filesystem for a native backend')
    parser.add_argument('--vulkan-compat', type=Path, help='x86-64 Vulkan resolver for a native backend')
    parser.add_argument('program', type=Path, help='Windows OpenXR executable')
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    try:
        raise SystemExit(run(parser.parse_args()))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
