#!/usr/bin/env python3
"""Launch a Windows OpenXR application in the active virtual SteamVR session."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def active_session():
    processes = []
    for path in Path('/proc').glob('[0-9]*/comm'):
        try:
            if path.stat().st_uid == os.getuid() and path.read_text().strip() == 'vrcompositor':
                processes.append(path.parent)
        except FileNotFoundError:
            continue
    if len(processes) != 1:
        raise ValueError('Expected one SteamVR compositor owned by the current user')
    process = processes[0]
    original = dict(entry.decode().split('=', 1) for entry in
                    (process / 'environ').read_bytes().split(b'\0') if b'=' in entry)
    expected = Path.home() / '.local/state/armada-vr/steamvr-virtual/openvrpaths.vrpath'
    if Path(original.get('VR_PATHREG_OVERRIDE', '')).resolve() != expected.resolve():
        raise ValueError('Expected the private virtual-headset registry')
    registry = json.loads(expected.read_text())
    settings = json.loads((Path(registry['config'][0]) / 'steamvr.vrsettings').read_text())
    if settings.get('steamvr', {}).get('forcedDriver') != 'null' or not settings.get('driver_null', {}).get('enable'):
        raise ValueError('This launcher currently supports the virtual null headset only')
    runtime = Path(registry['runtime'][0]).resolve(strict=True)
    manifest = runtime / 'steamxr_linux64.json'
    if manifest.is_symlink() or (runtime / 'bin/linux64/vrclient.so').is_symlink():
        raise ValueError('SteamVR runtime files must stay inside the selected installation')
    library = json.loads(manifest.read_text())['runtime']['library_path']
    if (manifest.parent / library).resolve(strict=True) != runtime / 'bin/linux64/vrclient.so':
        raise ValueError('OpenXR manifest selects a different SteamVR client')
    fex = (process / 'exe').resolve(strict=True)
    if fex.name != 'FEX':
        raise ValueError('Expected the running compositor to use FEX')
    names = ('HOME', 'USER', 'PATH', 'LANG', 'DISPLAY', 'XAUTHORITY', 'XDG_RUNTIME_DIR',
             'FEX_APP_CONFIG_LOCATION', 'FEX_PORTABLE', 'FEX_ENV', 'VK_DRIVER_FILES',
             'LVP_DRM_SYNC', 'LP_NUM_THREADS', 'LD_LIBRARY_PATH', 'VR_PATHREG_OVERRIDE',
             'VR_OVERRIDE', 'VR_CONFIG_PATH', 'VR_LOG_PATH', 'STEAMVR_TOOLSDIR',
             'STEAMVR_VRENV', 'MESA_VK_WSI_SW_PRESENT')
    return fex, manifest, {name: original[name] for name in names if name in original}


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
    fex, manifest, env = active_session()
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
    parser.add_argument('program', type=Path, help='Windows OpenXR executable')
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    try:
        raise SystemExit(run(parser.parse_args()))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
