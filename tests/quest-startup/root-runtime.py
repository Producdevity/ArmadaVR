import ctypes
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def command(args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT, timeout=30).strip()


assert Path('/sys/firmware/devicetree/base/compatible').read_bytes().split(b'\0')[0] == b'linux,dummy-virt'
assert command(['findmnt', '-n', '-o', 'FSTYPE', '/']) == 'overlay'
expected = json.loads(Path('/run/quest-root-expected.json').read_text())
block_device = (Path('/dev/disk/by-label') / expected['root_label']).stat()
assert stat.S_ISBLK(block_device.st_mode)
block = (Path('/sys/dev/block') / f'{os.major(block_device.st_rdev)}:{os.minor(block_device.st_rdev)}').resolve(strict=True)
assert (block / 'ro').read_text().strip() == '1'
if (block / 'partition').exists():
    block = block.parent
assert sha(Path('/usr/share/reframed-vr/kernel-build.json')) == expected['kernel_build_sha256']
for item in expected['modules']:
    assert sha(Path('/usr/lib/modules') / item['path']) == item['sha256'], item['path']
assert {str(p.relative_to('/usr/lib/modules')) for p in Path('/usr/lib/modules').rglob('*.ko')} == {i['path'] for i in expected['modules']}
firmware = expected['firmware'] + (expected.get('gpu_firmware') or [])
for item in firmware:
    assert sha(Path('/usr/lib/firmware') / item['path']) == item['sha256'], item['path']
runtime_path = Path('/usr/share/reframed-vr/root-systemd.json')
assert sha(runtime_path) == expected['systemd_runtime_sha256']
runtime = json.loads(runtime_path.read_text())
for item in runtime['files']:
    path = Path('/') / item['path'].removeprefix('files/')
    if 'link' in item:
        assert path.is_symlink() and os.readlink(path) == item['link'], item['path']
    else:
        assert sha(path) == item['sha256'] and stat.S_IMODE(path.stat().st_mode) == item['mode'], item['path']
for name, digest in runtime['native_abi_sha256'].items():
    assert sha(Path('/') / name) == digest, name
packages = {}
for name in ('monado', 'basalt'):
    record = expected.get(name)
    if record is None:
        continue
    prefix = Path('/opt/reframed-vr') / name
    files, links = record['contents']['files'], record['contents'].get('links', record['contents'].get('symlinks', {}))
    assert sha(Path('/usr/share/reframed-vr') / (name + '-build.json')) == record['build_sha256']
    assert {str(p.relative_to(prefix)) for p in prefix.rglob('*') if p.is_file() and not p.is_symlink()} == set(files)
    assert {str(p.relative_to(prefix)): os.readlink(p) for p in prefix.rglob('*') if p.is_symlink()} == links
    for path, item in files.items():
        source = prefix / path
        assert sha(source) == item['sha256'] and stat.S_IMODE(source.stat().st_mode) == item['mode'], path
    packages[name] = {'files': len(files), 'links': len(links)}
if expected.get('monado'):
    prefix = '/opt/reframed-vr/monado/lib64/'
    for name in ('libopenxr_monado.so', 'libmonado.so', 'libquest_tracking.so'):
        ctypes.CDLL(prefix + name, mode=os.RTLD_NOW)
    quest = ctypes.CDLL(prefix + 'libquest_tracking.so', mode=os.RTLD_NOW)
    quest.quest_slam_runtime_prepare.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    quest.quest_slam_runtime_prepare.restype = ctypes.c_bool
    assert not quest.quest_slam_runtime_prepare(None, None)
    quest.quest_slam_runtime_get_relation.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_int64, ctypes.c_void_p, ctypes.c_void_p]
    quest.quest_slam_runtime_get_relation.restype = ctypes.c_bool
    assert not quest.quest_slam_runtime_get_relation(None, 0, 0, None, None)
    packages['monado']['quest_runtime_loaded'] = True
if expected.get('basalt'):
    lib = ctypes.CDLL('/opt/reframed-vr/basalt/lib64/libbasalt.so.2', mode=os.RTLD_NOW)
    version = [ctypes.c_uint32() for _ in range(3)]
    lib.vit_api_get_version.argtypes = [ctypes.POINTER(ctypes.c_uint32)] * 3
    lib.vit_api_get_version.restype = ctypes.c_int
    assert lib.vit_api_get_version(*[ctypes.byref(v) for v in version]) == 0
    assert [v.value for v in version] == [2, 0, 1]
    packages['basalt']['vit_api'] = [2, 0, 1]
if expected.get('turnip'):
    turnip = expected['turnip']
    assert sha(Path('/usr/share/reframed-vr/turnip-build.json')) == turnip['build_sha256']
    for item in turnip['files']:
        assert sha(Path('/') / item['path']) == item['sha256']
    lib = ctypes.CDLL('/opt/reframed-vr/turnip/lib64/libvulkan_freedreno.so', mode=os.RTLD_NOW)
    assert lib.vk_icdGetInstanceProcAddr
    packages['turnip'] = {'files': len(turnip['files']), 'hardware_gpu_verified': False}
Path('/quest-volatile-test').write_text('RAM overlay only\n')
assert (block / 'stat').read_text().split()[4] == '0'
assert command(['systemctl', 'show', '--property=NFailedUnits', '--value']) == '0'
print('REFRAMED_QUEST_ROOT_RUNTIME_PASS ' + json.dumps({
    'modules': len(expected['modules']), 'firmware': len(firmware),
    'systemd_files': len(runtime['files']), 'packages': packages,
    'disk_writes': 0, 'hardware_boot_verified': False}), flush=True)
