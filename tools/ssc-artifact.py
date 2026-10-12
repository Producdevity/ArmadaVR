"""Validate and install the native SSC runtime into an offline headset root."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess

ROOT = Path(__file__).resolve().parents[1]
INSTALL_PREFIX = 'opt/armada-vr/ssc'
PREFIX = 'stage/' + INSTALL_PREFIX
BUILD_RECORD = 'usr/share/armada-vr/ssc-build.json'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def contents(prefix):
    prefix = Path(prefix)
    if prefix.is_symlink() or not prefix.is_dir():
        raise ValueError('Expected a regular SSC prefix')
    result = {'files': {}, 'links': {}}
    for path in (prefix, *sorted(prefix.rglob('*'))):
        name = str(path.relative_to(prefix))
        if path.is_symlink():
            target = path.readlink()
            if target.is_absolute() or not path.is_file() or not path.resolve().is_relative_to(prefix.resolve()):
                raise ValueError('SSC link escapes its prefix or is broken: ' + name)
            result['links'][name] = str(target)
        elif path.is_dir():
            if stat.S_IMODE(path.stat().st_mode) != 0o755:
                raise ValueError('Unexpected SSC directory mode: ' + name)
        elif path.is_file():
            mode = stat.S_IMODE(path.stat().st_mode)
            if mode not in (0o644, 0o755):
                raise ValueError('Unexpected SSC file mode: ' + name)
            result['files'][name] = {'sha256': digest(path), 'mode': mode}
        else:
            raise ValueError('Unexpected SSC file type: ' + name)
    return result


def package(directory):
    directory = Path(directory)
    report = json.loads((directory / 'build.json').read_text())
    if report.get('status') != 'compiled' or report.get('hardware_verified') is not False:
        raise ValueError('Expected an offline compiled SSC bundle')
    profile = json.loads((ROOT / 'profiles/ssc.json').read_text())
    if report.get('profile') != profile:
        raise ValueError('SSC source profile changed')
    names = [*profile['patches'], 'tools/ssc-build.py', 'tools/test-ssc-reports.py',
             'tests/ssc-reports.c', 'tests/ssc-lifetime.c', 'tests/ssc-context.c', 'tests/ssc-operation.c', 'tests/ssc-close.c',
             'src/pico-controller.c', 'src/pico-controller.h']
    if report.get('inputs_sha256') != {name: digest(ROOT / name) for name in names}:
        raise ValueError('SSC build inputs changed')
    test = directory / 'tests/result.json'
    if digest(test) != report['tests_sha256'] or json.loads(test.read_text()).get('passed') is not True:
        raise ValueError('SSC regression tests did not pass')
    prefix = directory / PREFIX
    tree = contents(prefix)
    if tree != report['contents']:
        raise ValueError('SSC artifact inventory or hashes changed')
    files = {'bin/ssccli', 'lib64/libssc.so.2', 'share/licenses/libssc/LICENSE',
             'share/ssc/source.tar.gz', 'share/ssc/profile.json'}
    files.update('share/ssc/' + Path(p).name for p in profile['patches'])
    links = {}
    for name, dep in profile['runtime_dependencies'].items():
        files.update(('lib64/' + dep['target'], 'share/licenses/' + name + '/' + dep['license']))
        links['lib64/' + dep['library']] = dep['target']
    if set(tree['files']) != files or tree['links'] != links:
        raise ValueError('Unexpected SSC runtime inventory')
    for name, item in tree['files'].items():
        path = prefix / name
        executable = name.startswith(('lib64/', 'bin/'))
        if item['mode'] != (0o755 if executable else 0o644):
            raise ValueError('Unexpected SSC runtime mode: ' + name)
        if executable:
            data = path.read_bytes()[:20]
            if data[:7] != b'\x7fELF\x02\x01\x01' or data[18:20] != b'\xb7\x00':
                raise ValueError('Expected a native ARM64 SSC ELF: ' + name)
        elif not path.stat().st_size:
            raise ValueError('Empty SSC metadata or license: ' + name)
    if digest(prefix / 'share/ssc/source.tar.gz') != profile['source_archive_sha256']:
        raise ValueError('SSC source archive differs')
    if (prefix / 'share/ssc/profile.json').read_bytes() != (ROOT / 'profiles/ssc.json').read_bytes():
        raise ValueError('SSC installed profile differs')
    for name in profile['patches']:
        if digest(prefix / 'share/ssc' / Path(name).name) != digest(ROOT / name):
            raise ValueError('SSC installed patch differs')
    return {'build_sha256': digest(directory / 'build.json'), 'contents': tree,
            'hardware_verified': False, 'automatic_startup': False}


def root_path(root, name):
    root = Path(root)
    if not root.is_dir() or root.is_symlink() or root.resolve() == Path('/'):
        raise ValueError('Expected an offline staging root')
    path = root / name
    for parent in (path, *path.parents):
        if parent == root:
            break
        if parent.is_symlink():
            raise ValueError('SSC destination contains a symlink')
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError('SSC destination escapes its root')
    return path


def verify_install(root, record):
    if contents(root_path(root, INSTALL_PREFIX)) != record['contents']:
        raise ValueError('Installed SSC inventory, modes or hashes differ')
    if digest(root_path(root, BUILD_RECORD)) != record['build_sha256']:
        raise ValueError('Installed SSC build record differs')


def install(directory, root, record):
    directory = Path(directory)
    if digest(directory / 'build.json') != record['build_sha256'] or contents(directory / PREFIX) != record['contents']:
        raise ValueError('SSC bundle changed before installation')
    prefix, metadata = root_path(root, INSTALL_PREFIX), root_path(root, BUILD_RECORD)
    if prefix.exists() or metadata.exists():
        raise ValueError('Root already supplies SSC')
    prefix.parent.mkdir(parents=True, exist_ok=True)
    metadata.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(directory / PREFIX, prefix, symlinks=True)
    shutil.copy2(directory / 'build.json', metadata)
    metadata.chmod(0o644)
    verify_install(root, record)


def verify_image(image, record, destination):
    destination = Path(destination)
    if any(c in str(destination) for c in ('"', '\\', '\n', '\r')):
        raise ValueError('Invalid SSC readback directory')
    destination.mkdir(parents=True, exist_ok=False)
    for name in (INSTALL_PREFIX, BUILD_RECORD):
        parent = destination / Path(name).parent
        parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['debugfs', '-R', f'rdump /{name} "{parent}"', str(image)],
                       check=True, timeout=30, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    verify_install(destination, record)


def check_loader(root):
    root = Path(root).resolve()
    root_path(root, INSTALL_PREFIX)
    command = ['/lib/ld-linux-aarch64.so.1', '--library-path', '/' + INSTALL_PREFIX + '/lib64:/usr/lib64',
               '/' + INSTALL_PREFIX + '/bin/ssccli']

    def enter_root():
        os.chroot(root)
        os.chdir('/')

    env = {'LC_ALL': 'C', 'LD_TRACE_LOADED_OBJECTS': '1', 'LD_WARN': 'yes', 'LD_BIND_NOW': 'yes', 'LD_VERBOSE': 'yes'}
    result = subprocess.run(command, preexec_fn=enter_root, env=env, capture_output=True, text=True, timeout=15)
    trace = result.stdout + result.stderr
    if (result.returncode or 'not found' in trace or 'undefined symbol' in trace or
            '/' + INSTALL_PREFIX + '/bin/ssccli:' not in trace or 'libssc.so.2' not in trace):
        raise ValueError('SSC root loader or relocations failed: ' + trace)
    version = subprocess.run([*command, '--version'], preexec_fn=enter_root, env={'LC_ALL': 'C'},
                             capture_output=True, text=True, check=True, timeout=15)
    if version.stdout.strip() != 'libssc version 0.4.4' or version.stderr:
        raise ValueError('SSC root CLI version check failed')
    return {'status': 'passed', 'trace': trace, 'version': version.stdout.strip(),
            'physical_sensor_access': False}
