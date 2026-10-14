#!/usr/bin/env python3
"""Build the pinned native Monado runtime without installing or selecting it."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import shlex
import signal
import subprocess
import sys
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('kernel_source', ROOT / 'tools/kernel-source.py')
sources = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sources)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def tree_digest(directory):
    entries = []
    for path in sorted(directory.rglob('*')):
        name = str(path.relative_to(directory))
        if path.is_symlink():
            entries.append([name, 'symlink', os.readlink(path)])
        elif path.is_file():
            entries.append([name, path.stat().st_mode & 0o777, digest(path)])
        elif path.is_dir():
            entries.append([name, 'directory'])
        else:
            raise ValueError('Unexpected source entry: ' + name)
    return hashlib.sha256(json.dumps(entries).encode()).hexdigest()


def build(profile):
    if sys.platform != 'linux':
        raise ValueError('Use tools/build-monado.sh on macOS')
    output, scratch = Path('/output'), Path('/work')
    if any(output.iterdir()) or any(scratch.iterdir()):
        raise ValueError('Output and scratch directories must be empty')
    archive = Path('/archive.tar.gz')
    sources.full_archive(profile, archive)
    patches = sorted((ROOT / 'patches/monado').glob('*.patch'))
    image = os.environ['MONADO_BUILD_IMAGE_ID']
    if not image.startswith('sha256:') or len(image) != 71:
        raise ValueError('Build image must be an immutable SHA-256 ID')
    report = {'status': 'started', 'hardware_verified': False, 'flash_image': False,
              'identity': {'profile': profile, 'image': image, 'builder_sha256': digest(Path(__file__)),
                           'pico_sources': {name: digest(ROOT / 'src' / name) for name in profile['pico_driver_sources']},
                           'patches': [{'path': str(p.relative_to(ROOT)), 'sha256': digest(p)} for p in patches],
                           'tests': {p.name: digest(p) for p in (ROOT / 'tests/monado-display-tests.py', ROOT / 'tests/monado-display-tests.c', ROOT / 'tests/monado-space-tests.c', ROOT / 'tests/monado-projection-tests.c')}},
              'commands': [], 'limits': {'jobs': 2, 'memory_bytes': 4 * 1024**3, 'scratch_bytes': 1024**3}}
    source = scratch / profile['full_source']['archive_root']
    build_dir = scratch / 'build'
    env = {**os.environ, 'DESTDIR': str(output / 'stage'), 'PYTHONDONTWRITEBYTECODE': '1'}
    started = time.monotonic()

    def run(command, name, timeout=120):
        report['commands'].append(command)
        print('Running ' + name, flush=True)
        with (output / name).open('wb') as log:
            process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                code = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                raise
        if code:
            raise subprocess.CalledProcessError(code, command)

    try:
        with tarfile.open(archive) as stream:
            stream.extractall(scratch, filter='data')
        for i, patch in enumerate(patches):
            run(['patch', '--batch', '--fuzz=0', '-d', str(source), '-p1', '-i', str(patch)], f'patch-{i + 1}.log')
        driver = source / 'src/xrt/drivers/pico_ssc'
        driver.mkdir()
        for name in profile['pico_driver_sources']:
            shutil.copy2(ROOT / 'src' / name, driver / name)
        options = []
        if os.environ.get('MONADO_SSC_BUNDLE'):
            spec = importlib.util.spec_from_file_location('ssc_artifact', ROOT / 'tools/ssc-artifact.py')
            ssc = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(ssc)
            bundle = Path(os.environ['MONADO_SSC_BUNDLE'])
            report['ssc_dependency'] = ssc.package(bundle)
            prefix = bundle / ssc.PREFIX
            header_dir = scratch / 'ssc-header'
            header_dir.mkdir()
            with tarfile.open(prefix / 'share/ssc/source.tar.gz') as stream:
                members = [item for item in stream.getmembers() if item.name.endswith('/src/libssc-sensor.h')]
                if len(members) != 1 or not members[0].isfile() or members[0].size > 65536:
                    raise ValueError('Expected the pinned SSC public header')
                (header_dir / 'libssc-sensor.h').write_bytes(stream.extractfile(members[0]).read())
            expected = json.loads((ROOT / 'profiles/ssc.json').read_text())['test_source_sha256']['src/libssc-sensor.h']
            if digest(header_dir / 'libssc-sensor.h') != expected:
                raise ValueError('SSC public header differs from pinned source')
            report['ssc_header_sha256'] = expected
            options = ['-DXRT_BUILD_DRIVER_PICO_SSC=ON', '-DPICO_SSC_INCLUDE_DIR=' + str(header_dir),
                       '-DPICO_SSC_LIBRARY=' + str(prefix / 'lib64/libssc.so.2'),
                       '-DCMAKE_EXE_LINKER_FLAGS=-Wl,-rpath-link,' + str(prefix / 'lib64'),
                       '-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-rpath-link,' + str(prefix / 'lib64')]
            env['LD_LIBRARY_PATH'] = str(prefix / 'lib64')
        else:
            options = ['-DXRT_BUILD_DRIVER_PICO_SSC=OFF']
        report['source_sha256'] = tree_digest(source)
        run([sys.executable, str(ROOT / 'tests/monado-display-tests.py'), str(source), str(output / 'display-tests')],
            'display-tests.log')
        report['display_tests_sha256'] = digest(output / 'display-tests/result.json')
        run(['cmake', '-S', str(source), '-B', str(build_dir), '-G', 'Ninja', *profile['cmake_options'], *options], 'configure.log')
        cache = (build_dir / 'CMakeCache.txt').read_text().splitlines()
        for feature in profile['required_features']:
            if feature + ':BOOL=ON' not in cache:
                raise ValueError('Required native runtime feature missing: ' + feature)
        run(['cmake', '--build', str(build_dir), '--parallel', '2'], 'compile.log', timeout=900)
        run(['ctest', '--test-dir', str(build_dir), '--output-on-failure', '--output-junit', str(output / 'ctest.xml'), '-j', '1'],
            'ctest.log', timeout=180)
        run(['cmake', '--install', str(build_dir)], 'install.log')
        prefix = output / 'stage/opt/reframed-vr/monado'
        for name in ['bin/monado-service', 'bin/monado-cli', 'lib64/libopenxr_monado.so', 'lib64/libmonado.so']:
            data = (prefix / name).read_bytes()
            if data[:6] != b'\x7fELF\x02\x01' or data[18:20] != b'\xb7\x00':
                raise ValueError('Expected native ARM64 ELF: ' + name)
        manifest = json.loads((prefix / 'share/openxr/1/openxr_monado.json').read_text())
        if manifest['runtime']['library_path'] != '/opt/reframed-vr/monado/lib64/libopenxr_monado.so':
            raise ValueError('Unexpected OpenXR runtime path')
        if any((output / 'stage/etc').rglob('*')) or any((output / 'stage/usr').rglob('*')):
            raise ValueError('Monado install escaped its opt prefix')
        commands = json.loads((build_dir / 'compile_commands.json').read_text())
        for name in ['comp_window_direct.c', 'comp_window_vk_display.c']:
            command = [entry['command'] for entry in commands if entry['file'].endswith('/' + name)]
            if len(command) != 1:
                raise ValueError('Native direct-display source omitted: ' + name)
        args = shlex.split(command[0])
        index = args.index('-o')
        del args[index:index + 2]
        args.remove('-c')
        run([*args, '-E', '-dM'], 'display-preprocessor.txt')
        if '#define VK_USE_PLATFORM_DISPLAY_KHR' not in (output / 'display-preprocessor.txt').read_text():
            raise ValueError('Direct display was excluded by the actual preprocessor configuration')
        if b'Unable to select a compatible display plane' not in (prefix / 'bin/monado-service').read_bytes():
            raise ValueError('Installed service does not contain the display selection implementation')
        if b'XRT_COMPOSITOR_FORCE_DISPLAY_MODE' not in (prefix / 'bin/monado-service').read_bytes():
            raise ValueError('Installed service omits exact display mode selection')
        if 'ssc_dependency' in report:
            if 'XRT_BUILD_DRIVER_PICO_SSC:BOOL=ON' not in cache:
                raise ValueError('Pico driver was disabled')
            for name in profile['pico_driver_sources']:
                if name.endswith('.c') and len([entry for entry in commands if entry['file'].endswith('/pico_ssc/' + name)]) != 1:
                    raise ValueError('Pico driver source omitted: ' + name)
            library = prefix / 'lib64/libpico_tracking.so'
            run(['nm', '-D', '--defined-only', str(library)], 'pico-runtime-exports.txt')
            exported = {line.split()[-1] for line in (output / 'pico-runtime-exports.txt').read_text().splitlines() if line.split()}
            if not set(profile['pico_runtime_exports']) <= exported:
                raise ValueError('Installed Pico runtime omits required entry points')
            for name in ('monado-service', 'monado-cli'):
                run(['readelf', '-d', str(prefix / 'bin' / name)], name + '-dynamic.txt')
                if 'Shared library: [libpico_tracking.so]' not in (output / (name + '-dynamic.txt')).read_text():
                    raise ValueError('Installed executable is not linked to the Pico runtime: ' + name)
            run([sys.executable, '-c',
                 'import ctypes; lib=ctypes.CDLL(' + repr(str(library)) + '); '
                 'assert lib.pico_slam_live_prepare(None,None)==-22; '
                 'assert lib.pico_slam_live_destroy(None)==-22; '
                 'print("PICO_RUNTIME_LIBRARY_PASS invalid_input_refused=1 device_actions=0")'],
                'pico-runtime-load.txt')
        if tree_digest(source) != report['source_sha256']:
            raise ValueError('Monado source changed during compilation')
        report['status'] = 'compiled'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        for name in ['CMakeCache.txt', 'compile_commands.json', 'install_manifest.txt']:
            if (build_dir / name).exists():
                shutil.copy2(build_dir / name, output / name)
        tests = build_dir / 'Testing/Temporary'
        if tests.exists():
            shutil.copytree(tests, output / 'ctest-output')
        (output / 'packages.txt').write_text(subprocess.check_output(['rpm', '-qa'], text=True))
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        report['files_sha256'] = {str(p.relative_to(output)): digest(p) for p in sorted(output.rglob('*'))
                                   if p.is_file() and not p.is_symlink() and p != output / 'build.json'}
        report['symlinks'] = {str(p.relative_to(output)): os.readlink(p) for p in sorted(output.rglob('*')) if p.is_symlink()}
        (output / 'build.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('fetch', 'build'))
    args = parser.parse_args()
    profile = json.loads((ROOT / 'profiles/monado.json').read_text())
    if args.action == 'fetch':
        archive = ROOT / 'output/downloads' / ('monado-' + profile['commit'] + '.tar.gz')
        sources.full_archive(profile, archive)
        print('Verified ' + str(archive))
    else:
        build(profile)
