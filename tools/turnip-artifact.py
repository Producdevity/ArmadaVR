#!/usr/bin/env python3
"""Validate a compiled Turnip bundle before packaging it into headset userspace."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIBRARY = 'libvulkan_freedreno.so'
ICD = 'freedreno_icd.aarch64.json'
PREFIX = '/opt/armada-vr/turnip'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate(directory):
    directory = Path(directory)
    report = json.loads((directory / 'build.json').read_text())
    identity = report['identity']
    if (report.get('status') != 'compiled' or identity.get('driver') != 'turnip' or
            report.get('kgsl_drm_bridge_compiled') is not True or
            report.get('kgsl_display_compiled') is not True or report.get('offline_tests_passed') != 5):
        raise ValueError('Turnip requires a compiled DRM bridge and passing offline tests')
    if identity['profile'] != json.loads((ROOT / 'profiles/mesa.json').read_text()):
        raise ValueError('Turnip profile changed since compilation')
    patches = [{'path': str(p.relative_to(ROOT)), 'sha256': digest(p)}
               for p in sorted((ROOT / 'patches/mesa/turnip').glob('*.patch'))]
    if not patches or identity['patches'] != patches:
        raise ValueError('Turnip patches changed since compilation')
    if identity['builder_sha256'] != digest(ROOT / 'tools/mesa-build.py'):
        raise ValueError('Turnip builder changed since compilation')
    artifacts = {}
    for item in report['artifacts']:
        name = item['path']
        if name in artifacts or Path(name).name != name:
            raise ValueError('Invalid Turnip artifact path')
        path = directory / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size != item['bytes'] or digest(path) != item['sha256']:
            raise ValueError('Turnip artifact mismatch: ' + name)
        artifacts[name] = item
    required = {LIBRARY, ICD, 'kgsl-compile-command.txt', 'dynamic-dependencies.txt',
                'kgsl-display', 'tu_kgsl_display.h'}
    if not required <= artifacts.keys():
        raise ValueError('Turnip build omits library or compile/link evidence')
    library = (directory / LIBRARY).read_bytes()
    if (library[:6] != b'\x7fELF\x02\x01' or library[18:20] != b'\xb7\x00' or
            b'TU_KGSL_DRM_SYNC' not in library or b'TU_KGSL_DISPLAY' not in library):
        raise ValueError('Turnip is not an ARM64 library with the DRM bridge')
    command = (directory / 'kgsl-compile-command.txt').read_text()
    if '-DHAVE_LIBDRM ' not in command or '-DMESA_SYSTEM_HAS_KMS_DRM=1 ' not in command:
        raise ValueError('Turnip KGSL compile omitted DRM support')
    if 'Shared library: [libdrm.so.2]' not in (directory / 'dynamic-dependencies.txt').read_text():
        raise ValueError('Turnip is not linked to libdrm')
    icd = json.loads((directory / ICD).read_text())
    if icd['ICD']['library_path'] != PREFIX + '/lib64/' + LIBRARY or icd['ICD'].get('library_arch') != '64':
        raise ValueError('Unexpected Turnip ICD library path or architecture')
    for suite in ('fence', 'wait', 'submit', 'display'):
        result = report['kgsl_' + suite + '_tests']
        stem = 'kgsl-' + suite + '-tests'
        if (result.get('status') != 'passed' or result['runner_sha256'] != digest(ROOT / ('tests/' + stem + '.py')) or
                result['model_sha256'] != digest(ROOT / ('tests/' + stem + '.cpp'))):
            raise ValueError('Turnip ' + suite + ' test sources changed or tests failed')
    display = report['kgsl_display_test']
    if (display['source_sha256'] != digest(ROOT / 'tests/kgsl-display.c') or
            display['header_sha256'] != artifacts['tu_kgsl_display.h']['sha256'] or
            report['kgsl_display_tests']['helpers_sha256'] != display['header_sha256']):
        raise ValueError('Turnip display fixture or header changed since testing')
    return {'build_sha256': digest(directory / 'build.json'),
            'files': [{'source': LIBRARY, 'path': PREFIX.lstrip('/') + '/lib64/' + LIBRARY, 'sha256': artifacts[LIBRARY]['sha256']},
                      {'source': ICD, 'path': PREFIX.lstrip('/') + '/share/vulkan/icd.d/' + ICD, 'sha256': artifacts[ICD]['sha256']}],
            'kgsl_drm_bridge_compiled': True, 'kgsl_display_compiled': True, 'default_icd': False, 'hardware_gpu_verified': False}
