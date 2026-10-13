#!/usr/bin/env python3
"""Build an offline ext4 root from pinned ARM64 userspace and matching headset inputs."""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
from pathlib import Path
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("root_builder", ROOT / "tools/build-headset-initramfs.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
spec = importlib.util.spec_from_file_location("root_assembly", ROOT / "tools/assemble-quest-boot.py")
assembly = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assembly)
spec = importlib.util.spec_from_file_location("turnip_artifact", ROOT / "tools/turnip-artifact.py")
turnip_artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(turnip_artifact)
spec = importlib.util.spec_from_file_location("monado_artifact", ROOT / "tools/monado-artifact.py")
monado_artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monado_artifact)
spec = importlib.util.spec_from_file_location("lepton_artifact", ROOT / "tools/lepton-artifact.py")
lepton_artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lepton_artifact)

spec = importlib.util.spec_from_file_location("ssc_artifact", ROOT / "tools/ssc-artifact.py")
ssc_artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ssc_artifact)

spec = importlib.util.spec_from_file_location("fastrpc_artifact", ROOT / "tools/fastrpc-artifact.py")
fastrpc_artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fastrpc_artifact)

spec = importlib.util.spec_from_file_location("inventory_artifact", ROOT / "tools/ssc-inventory-artifact.py")
inventory_artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory_artifact)

SCRIPT = r'''
set -eu
mkdir -p /tmp/root /tmp/modules /tmp/verify/modules /tmp/verify/firmware
tar -xpf /rootfs.tar -C /tmp/root
if test -d /runtime-rpms; then
    test -x /tmp/root/usr/lib/systemd/systemd
    test ! -e /tmp/root/tmp/armada-vr-rpms
    mkdir /tmp/root/tmp/armada-vr-rpms
    cp /runtime-rpms/*.rpm /tmp/root/tmp/armada-vr-rpms/
    chroot /tmp/root /bin/sh -ec '
        export LC_ALL=C
        for package in /tmp/armada-vr-rpms/*.rpm; do
            rpmkeys --checksig --verbose "$package" > /tmp/armada-vr-rpms/signature.txt
            cat /tmp/armada-vr-rpms/signature.txt
            grep -Ei "signature.*: OK$" /tmp/armada-vr-rpms/signature.txt >/dev/null
        done
        rpm --upgrade --test /tmp/armada-vr-rpms/*.rpm
        rpm --upgrade /tmp/armada-vr-rpms/*.rpm
        ldconfig
        rpm -qp --qf "%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}\n" /tmp/armada-vr-rpms/*.rpm
    ' > /output/runtime-rpms-install.txt 2>&1
    rm -rf /tmp/root/tmp/armada-vr-rpms
fi
tar -xzf /kernel/modules.tar.gz -C /tmp/modules --no-same-owner
python3 - "$1" <<'PYROOT'
import hashlib,importlib.util,json,shutil,sys
from pathlib import Path
root=Path('/tmp/root'); version=sys.argv[1]
target=json.loads(Path('/output/root-profile.json').read_text())
if not (root/'usr/lib/systemd/systemd').is_file() or (root/'etc/initrd-release').exists():
    raise SystemExit('Expected full systemd userspace, not an initramfs')
removed=[]
for name in ['.dockerenv','run/.containerenv','vm-boot','usr/lib/modules',
             'etc/systemd/system/multi-user.target.wants/armada-vr-lab.service',
             'etc/systemd/system/multi-user.target.wants/armada-vr-desktop.service',
             'etc/systemd/system/dev-virtio\\x2dports-org.qemu.guest_agent.0.device.wants',
             'etc/systemd/network/20-qemu.network','etc/X11/xorg.conf.d/20-virtual-display.conf']:
    path=root/name
    if path.is_symlink() or path.is_file():
        path.unlink();removed.append(name)
    elif path.is_dir():
        shutil.rmtree(path);removed.append(name)
(root/'etc/machine-id').write_text('')
(root/'etc/hostname').write_text('armada-vr\n')
(root/'etc/fstab').write_text('# Root is mounted read-only by the initramfs; writable state is in RAM.\n')
default=root/'etc/systemd/system/default.target'
if default.is_symlink() or default.is_file():default.unlink()
default.symlink_to('/usr/lib/systemd/system/multi-user.target')
modules=root/'usr/lib/modules';modules.mkdir()
shutil.copytree(Path('/tmp/modules/lib/modules')/version,modules/version,symlinks=True)
fw=root/'usr/lib/firmware';fw.mkdir(exist_ok=True)
manifest=json.loads(Path('/firmware/manifest.json').read_text())
gpu=json.loads(Path('/gpu-firmware/manifest.json').read_text()) if Path('/gpu-firmware').exists() else None
for item in manifest['files']+(gpu['files'] if gpu else []):
    path=fw/item['path']
    if path.is_symlink() or path.exists():
        raise SystemExit('Base image already supplies selected headset firmware: '+str(path))
    source='/gpu-firmware/files' if gpu and item in gpu['files'] else '/firmware/files'
    shutil.copy2(Path(source)/item['path'],path)
unit=None
if target['profile']=='quest3':
    unit='usr/lib/systemd/system/armada-quest-boot.service'
    shutil.copy2('/quest.service',root/unit)
else:
    if any(root.glob('usr/lib/systemd/system/armada-quest*')) or any(root.glob('etc/systemd/system/**/armada-quest*')):
        raise SystemExit('Pico root cannot inherit Quest startup units')
pico_services=None
if target.get('pico_services'):
    spec=importlib.util.spec_from_file_location('pico_services','/repo/tools/pico-services.py')
    pico=importlib.util.module_from_spec(spec);spec.loader.exec_module(pico)
    pico_services=pico.install('/',root,manifest)
    Path('/output/pico-services.json').write_text(json.dumps(pico_services,indent=2)+'\n')
metadata=root/'usr/share/armada-vr';metadata.mkdir(parents=True,exist_ok=True)
shutil.copy2('/kernel/build.json',metadata/'kernel-build.json')
shutil.copy2('/firmware/manifest.json',metadata/target['firmware_metadata'])
if Path('/output/runtime-rpms.json').exists():
    shutil.copy2('/output/runtime-rpms.json',metadata/'runtime-rpms.json')
if gpu:shutil.copy2('/gpu-firmware/manifest.json',metadata/'quest-gpu-firmware.json')
turnip=None
if Path('/output/turnip.json').is_file():
    turnip=json.loads(Path('/output/turnip.json').read_text())
    prefix=root/'opt/armada-vr/turnip'
    if prefix.exists() or prefix.is_symlink():
        raise SystemExit('Base image already supplies Turnip')
    for item in turnip['files']:
        target=root/item['path'];target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(Path('/turnip')/item['source'],target)
    shutil.copy2('/turnip/build.json',metadata/'turnip-build.json')
ssc=None
if Path('/output/ssc.json').is_file():
    ssc=json.loads(Path('/output/ssc.json').read_text())
    spec=importlib.util.spec_from_file_location('ssc','/repo/tools/ssc-artifact.py')
    artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
    artifact.install('/ssc',root,ssc)
inventory=None
if Path('/output/ssc-inventory.json').is_file():
    inventory=json.loads(Path('/output/ssc-inventory.json').read_text())
    spec=importlib.util.spec_from_file_location('inventory','/repo/tools/ssc-inventory-artifact.py')
    artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
    artifact.install('/ssc-inventory',root,inventory)
fastrpc=None
if Path('/output/fastrpc.json').is_file():
    fastrpc=json.loads(Path('/output/fastrpc.json').read_text())
    spec=importlib.util.spec_from_file_location('fastrpc','/repo/tools/fastrpc-artifact.py')
    artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
    artifact.install('/fastrpc',root,fastrpc)
monado=None
if Path('/output/monado.json').is_file():
    monado=json.loads(Path('/output/monado.json').read_text())
    spec=importlib.util.spec_from_file_location('monado','/repo/tools/monado-artifact.py')
    artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
    artifact.install('/monado',root,monado)
lepton=None
if Path('/output/lepton.json').is_file():
    lepton=json.loads(Path('/output/lepton.json').read_text())
    spec=importlib.util.spec_from_file_location('lepton','/repo/tools/lepton-artifact.py')
    artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
    artifact.install('/lepton',root,lepton)
    Path('/output/lepton-runtime.json').write_text(json.dumps(artifact.configure_runtime(root,'/lepton-host',lepton['host']),indent=2)+'\n')
Path('/output/root-settings.json').write_text(json.dumps({'removed_virtual_paths':removed,'unit_path':unit,
    'unit_sha256':hashlib.sha256((root/unit).read_bytes()).hexdigest() if unit else None,'hostname':'armada-vr',
    'default_target':'multi-user.target','automatic_lab_tests':False,'automatic_desktop':False,'turnip':turnip,'monado':monado,'lepton':lepton,'ssc':ssc,'ssc_inventory':inventory,'fastrpc':fastrpc,'gpu_firmware':gpu,'pico_services':pico_services},indent=2)+'\n')
PYROOT
if test -d /systemd-runtime; then
    python3 /systemd-runtime.py /systemd-runtime --install /tmp/root
fi
if test -f /output/ssc.json; then
    python3 - <<'PYSSC'
import importlib.util,json
from pathlib import Path
spec=importlib.util.spec_from_file_location('ssc','/repo/tools/ssc-artifact.py')
artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
Path('/output/ssc-loader.json').write_text(json.dumps(artifact.check_loader('/tmp/root'),indent=2)+'\n')
PYSSC
fi
if test -f /output/ssc-inventory.json; then
    python3 - <<'PYINVENTORY'
import importlib.util,json
from pathlib import Path
spec=importlib.util.spec_from_file_location('inventory','/repo/tools/ssc-inventory-artifact.py')
artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
Path('/output/ssc-inventory-loader.json').write_text(json.dumps(artifact.check_loader('/tmp/root'),indent=2)+'\n')
PYINVENTORY
fi
if test -f /output/fastrpc.json; then
    python3 - <<'PYFASTRPC'
import importlib.util,json
from pathlib import Path
spec=importlib.util.spec_from_file_location('fastrpc','/repo/tools/fastrpc-artifact.py')
artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
Path('/output/fastrpc-loader.json').write_text(json.dumps(artifact.check_loader('/tmp/root'),indent=2)+'\n')
PYFASTRPC
fi
if test -f /output/pico-services.json; then
    python3 - <<'PYQRTR'
import json,subprocess
from pathlib import Path
logs=[]
services=json.loads(Path('/output/pico-services.json').read_text())
for name in ('qrtr-ns','qrtr-lookup')+(('pd-mapper',) if services['pd_mapper'] else ()):
    data=subprocess.check_output(['chroot','/tmp/root','/usr/bin/ldd','-r','/usr/bin/'+name],text=True,stderr=subprocess.STDOUT)
    logs.append(name+'\n'+data)
    if 'not found' in data or 'undefined symbol' in data:
        raise SystemExit('Pico QRTR runtime has unresolved dependencies: '+name)
Path('/output/pico-services-dependencies.txt').write_text('\n'.join(logs))
PYQRTR
fi
if test -f /output/turnip.json; then
    chroot /tmp/root /usr/bin/ldd -r /opt/armada-vr/turnip/lib64/libvulkan_freedreno.so > /output/turnip-dependencies.txt 2>&1
    chroot /tmp/root /usr/bin/python3 -c 'import ctypes,os; library=ctypes.CDLL("/opt/armada-vr/turnip/lib64/libvulkan_freedreno.so",mode=os.RTLD_NOW); assert library.vk_icdGetInstanceProcAddr; print("Turnip ARM64 RTLD_NOW and ICD entry: passed")' > /output/turnip-loader.txt 2>&1
fi
if test -f /output/monado.json; then
    chroot /tmp/root /usr/bin/env LD_LIBRARY_PATH=/opt/armada-vr/monado/lib64:/opt/armada-vr/ssc/lib64 /usr/bin/ldd -r /opt/armada-vr/monado/bin/monado-service > /output/monado-dependencies.txt 2>&1
    python3 -c 'from pathlib import Path; data=Path("/output/monado-dependencies.txt").read_text(); assert "not found" not in data and "undefined symbol" not in data, data'
    chroot /tmp/root /usr/bin/env LD_LIBRARY_PATH=/opt/armada-vr/monado/lib64:/opt/armada-vr/ssc/lib64 /usr/bin/python3 -c 'import ctypes,os; [ctypes.CDLL("/opt/armada-vr/monado/lib64/"+name,mode=os.RTLD_NOW) for name in ("libopenxr_monado.so","libmonado.so")]; print("Monado ARM64 client libraries RTLD_NOW: passed")' > /output/monado-loader.txt 2>&1
fi
if test -f /output/lepton.json; then
    chroot /tmp/root /bin/sh -ec '
        for program in podman fuse-overlayfs adb newuidmap newgidmap conmon crun setfattr inotifywait jq ip which hostname tput; do
            path=$(command -v "$program")
            printf "%s\n" "$path"
            ldd -r "$path"
        done
        rpm -q podman fuse-overlayfs android-tools shadow-utils conmon crun attr inotify-tools jq iproute which hostname ncurses
        ldd -r /usr/lib/libVkLayer_fossilize.so
    ' > /output/lepton-dependencies.txt 2>&1
    python3 -c 'from pathlib import Path; data=Path("/output/lepton-dependencies.txt").read_text(); assert "not found" not in data and "undefined symbol" not in data, data'
    chroot /tmp/root /usr/bin/python3 -c 'import ctypes,os; library=ctypes.CDLL("/usr/lib/libVkLayer_fossilize.so",mode=os.RTLD_NOW); assert library.VK_LAYER_fossilize_GetInstanceProcAddr' >> /output/lepton-dependencies.txt 2>&1
fi
python3 - "$2" <<'PYSIZE'
import sys
with open('/output/rootfs.ext4','xb') as f:f.truncate(int(sys.argv[1])*1024**3)
PYSIZE
mkfs.ext4 -q -F -L "$3" -O ^orphan_file -E lazy_itable_init=0,lazy_journal_init=0 -d /tmp/root /output/rootfs.ext4
e2fsck -fn /output/rootfs.ext4 > /output/fsck.log 2>&1
debugfs -R stats /output/rootfs.ext4 > /output/filesystem.txt 2>&1
if test -f /quest.service; then
    debugfs -R 'dump /usr/lib/systemd/system/armada-quest-boot.service /tmp/verify/quest.service' /output/rootfs.ext4
fi
debugfs -R 'rdump /usr/lib/modules /tmp/verify/modules' /output/rootfs.ext4
debugfs -R 'rdump /usr/lib/firmware /tmp/verify/firmware' /output/rootfs.ext4
if test -d /systemd-runtime; then
    python3 - <<'PYRUNTIME'
import json,re,subprocess
from pathlib import Path
record=json.loads(Path('/systemd-runtime/manifest.json').read_text())
names=[item['path'].removeprefix('files/') for item in record['files']]
names+=list(record['native_abi_sha256'])+['usr/share/armada-vr/root-systemd.json']
directories={str(Path(name).parent) for name in names if '\\' in name}
names=sorted({name for name in names if not any(Path(name).is_relative_to(directory) for directory in directories)} | directories)
commands=[]
for name in names:
    if not re.fullmatch(r'[A-Za-z0-9_./@+:-]+',name):
        raise SystemExit('Unsupported debugfs runtime path: '+name)
    parent=(Path('/tmp/verify/runtime')/name).parent
    parent.mkdir(parents=True,exist_ok=True)
    commands.append(f'rdump /{name} {parent}\n')
Path('/tmp/runtime-debugfs.txt').write_text(''.join(commands))
subprocess.run(['debugfs','-f','/tmp/runtime-debugfs.txt','/output/rootfs.ext4'],check=True)
PYRUNTIME
    python3 /systemd-runtime.py /systemd-runtime --verify /tmp/verify/runtime
fi
if test -f /output/turnip.json; then
    mkdir /tmp/verify/turnip
    debugfs -R 'rdump /opt/armada-vr/turnip /tmp/verify/turnip' /output/rootfs.ext4
    debugfs -R 'dump /usr/share/armada-vr/turnip-build.json /tmp/verify/turnip-build.json' /output/rootfs.ext4
fi
if test -f /output/monado.json; then
    mkdir -p /tmp/verify/monado-root/opt/armada-vr /tmp/verify/monado-root/usr/share/armada-vr
    debugfs -R 'rdump /opt/armada-vr/monado /tmp/verify/monado-root/opt/armada-vr' /output/rootfs.ext4
    debugfs -R 'dump /usr/share/armada-vr/monado-build.json /tmp/verify/monado-root/usr/share/armada-vr/monado-build.json' /output/rootfs.ext4
fi
if test -f /output/lepton.json; then
    mkdir -p /tmp/verify/lepton-root/opt/armada-vr /tmp/verify/lepton-root/usr/share/armada-vr
    debugfs -R 'rdump /opt/armada-vr/lepton /tmp/verify/lepton-root/opt/armada-vr' /output/rootfs.ext4
    debugfs -R 'dump /usr/share/armada-vr/lepton-build.json /tmp/verify/lepton-root/usr/share/armada-vr/lepton-build.json' /output/rootfs.ext4
    mkdir -p /tmp/verify/lepton-root/usr/bin /tmp/verify/lepton-root/usr/lib/android-sdk /tmp/verify/lepton-capabilities
    for program in newuidmap newgidmap adb; do
        debugfs -R "dump -p /usr/bin/$program /tmp/verify/lepton-root/usr/bin/$program" /output/rootfs.ext4
    done
    for program in newuidmap newgidmap; do
        debugfs -R "ea_get -f /tmp/verify/lepton-capabilities/$program.cap /usr/bin/$program security.capability" /output/rootfs.ext4
    done
    debugfs -R 'rdump /usr/lib/android-sdk/platform-tools /tmp/verify/lepton-root/usr/lib/android-sdk' /output/rootfs.ext4
    python3 - <<'PYHOST'
import json,subprocess
from pathlib import Path
record=json.loads(Path('/output/lepton-runtime.json').read_text())['host']
names=[i['path'] for i in record['files']]+['usr/share/armada-vr/lepton-host.json','home/vr/.config/containers/storage.conf']
for name in names:
    target=Path('/tmp/verify/lepton-root')/name;target.parent.mkdir(parents=True,exist_ok=True)
    subprocess.run(['debugfs','-R',f'dump -p /{name} {target}','/output/rootfs.ext4'],check=True)
PYHOST
fi
python3 - <<'PYVERIFY'
import hashlib,importlib.util,json
from pathlib import Path
module_root=Path('/tmp/verify/modules/modules')
expected=json.loads(Path('/kernel/modules.json').read_text())
modules=[]
for item in expected:
    name=item['path'].split('lib/modules/',1)[1];path=module_root/name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=item['sha256']:
        raise SystemExit('Root image module mismatch: '+name)
    modules.append({'path':name,'sha256':item['sha256']})
actual={str(p.relative_to(module_root)) for p in module_root.rglob('*.ko')}
if actual!={i['path'] for i in modules}:raise SystemExit('Unexpected root kernel modules')
firmware=json.loads(Path('/firmware/manifest.json').read_text())['files']
gpu=json.loads(Path('/gpu-firmware/manifest.json').read_text())['files'] if Path('/gpu-firmware').exists() else []
for item in firmware+gpu:
    p=Path('/tmp/verify/firmware/firmware')/item['path']
    if not p.is_file() or hashlib.sha256(p.read_bytes()).hexdigest()!=item['sha256']:
        raise SystemExit('Root image firmware mismatch: '+item['path'])
if Path('/quest.service').exists() and Path('/tmp/verify/quest.service').read_bytes()!=Path('/quest.service').read_bytes():
    raise SystemExit('Root image service mismatch')
turnip=None
if Path('/output/turnip.json').is_file():
    turnip=json.loads(Path('/output/turnip.json').read_text())
    for item in turnip['files']:
        path=Path('/tmp/verify/turnip')/item['path'].removeprefix('opt/armada-vr/')
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=item['sha256']:
            raise SystemExit('Root image Turnip mismatch: '+item['path'])
    if hashlib.sha256(Path('/tmp/verify/turnip-build.json').read_bytes()).hexdigest()!=turnip['build_sha256']:
        raise SystemExit('Root image Turnip build record mismatch')
monado=None
if Path('/output/monado.json').is_file():
    monado=json.loads(Path('/output/monado.json').read_text())
    spec=importlib.util.spec_from_file_location('monado','/repo/tools/monado-artifact.py')
    artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
    artifact.verify_install('/tmp/verify/monado-root',monado)
lepton=None
if Path('/output/lepton.json').is_file():
    lepton=json.loads(Path('/output/lepton.json').read_text())
    spec=importlib.util.spec_from_file_location('lepton','/repo/tools/lepton-artifact.py')
    artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
    artifact.verify_install('/tmp/verify/lepton-root',lepton)
    artifact.verify_runtime('/tmp/verify/lepton-root',json.loads(Path('/output/lepton-runtime.json').read_text()),'/tmp/verify/lepton-capabilities')
pico_services=None
if Path('/output/pico-services.json').is_file():
    pico_services=json.loads(Path('/output/pico-services.json').read_text())
    spec=importlib.util.spec_from_file_location('pico_services','/repo/tools/pico-services.py')
    pico=importlib.util.module_from_spec(spec);spec.loader.exec_module(pico)
    pico.verify_image('/output/rootfs.ext4',pico_services,'/tmp/verify/pico-root')
ssc=None
if Path('/output/ssc.json').is_file():
    ssc=json.loads(Path('/output/ssc.json').read_text())
    spec=importlib.util.spec_from_file_location('ssc','/repo/tools/ssc-artifact.py')
    artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
    artifact.verify_image('/output/rootfs.ext4',ssc,'/tmp/verify/ssc-root')
inventory=None
if Path('/output/ssc-inventory.json').is_file():
    inventory=json.loads(Path('/output/ssc-inventory.json').read_text())
    spec=importlib.util.spec_from_file_location('inventory','/repo/tools/ssc-inventory-artifact.py')
    artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
    artifact.verify_image('/output/rootfs.ext4',inventory,'/tmp/verify/inventory-root')
fastrpc=None
if Path('/output/fastrpc.json').is_file():
    fastrpc=json.loads(Path('/output/fastrpc.json').read_text())
    spec=importlib.util.spec_from_file_location('fastrpc','/repo/tools/fastrpc-artifact.py')
    artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
    artifact.verify_image('/output/rootfs.ext4',fastrpc,'/tmp/verify/fastrpc-root')
Path('/output/contents.json').write_text(json.dumps({'modules':modules,'firmware':firmware,'gpu_firmware':gpu,'pico_services':pico_services,
    'quest_startup_unit_sha256':hashlib.sha256(Path('/tmp/verify/quest.service').read_bytes()).hexdigest() if Path('/quest.service').exists() else None,
    'systemd_runtime_verified':Path('/systemd-runtime').is_dir(),'turnip':turnip,'monado':monado,'lepton':lepton,'ssc':ssc,'ssc_inventory':inventory,'fastrpc':fastrpc},indent=2)+'\n')
PYVERIFY
rpm -q e2fsprogs systemd > /output/tool-versions.txt
'''


def export_digest(path, compressed=False, limit=32 * 1024**3):
    if path.is_symlink() or not path.is_file():
        raise ValueError("Expected a regular file: " + str(path))
    process = watchdog = None
    with tempfile.TemporaryFile() as errors:
        if compressed:
            process = subprocess.Popen(["zstd", "-dc", "--memory=128MB", "--", str(path)],
                                       stdout=subprocess.PIPE, stderr=errors)
            watchdog = threading.Timer(120, process.kill)
            watchdog.daemon = True
            watchdog.start()
            stream = process.stdout
        else:
            stream = path.open("rb")
        try:
            digest, size = hashlib.sha256(), 0
            while block := stream.read(1024**2):
                size += len(block)
                if size > limit:
                    raise ValueError("Userspace export exceeds the 32 GiB expanded-size limit")
                digest.update(block)
            if process and process.wait(timeout=5):
                errors.seek(0)
                raise ValueError("Cannot decompress userspace export: " + errors.read(4096).decode(errors="replace"))
            return digest.hexdigest(), size
        finally:
            stream.close()
            if watchdog:
                watchdog.cancel()
            if process and process.poll() is None:
                process.kill()
                process.wait(timeout=5)


def read_export(directory, expected_image=None):
    manifest_path = directory / "manifest.json"
    manifest_sha = builder.digest(manifest_path)
    manifest = json.loads(manifest_path.read_text())
    if not isinstance(manifest, dict):
        raise ValueError("Requires a root export manifest object")
    image = manifest.get("container_image", "")
    expected = manifest.get("export_sha256", "")
    if (manifest.get("target") != "headset-root-offline" or manifest.get("status") != "built" or
            not isinstance(image, str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", image) or
            not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected)):
        raise ValueError("Requires a successful root build with immutable userspace and export identities")
    if expected_image is not None and image != expected_image:
        raise ValueError("Reused export must have the same immutable userspace image")
    archive = directory / "rootfs.tar"
    if not archive.exists() and not archive.is_symlink():
        archive = directory / "rootfs.tar.zst"
    archive_sha = builder.digest(archive)
    compressed = archive.name.endswith(".zst")
    digest, size = export_digest(archive, compressed)
    if digest != expected or not size:
        raise ValueError("Reused userspace export checksum mismatch")
    if builder.digest(archive) != archive_sha or builder.digest(manifest_path) != manifest_sha:
        raise ValueError("Userspace export changed during validation")
    return {"path": str(archive), "manifest_sha256": manifest_sha, "sha256": digest,
            "archive_sha256": archive_sha, "expanded_bytes": size, "compressed": compressed,
            "container_image": image}


def read_rpms(directory):
    manifest_path = directory / "manifest.json"
    manifest_sha = builder.digest(manifest_path)
    manifest = json.loads(manifest_path.read_text())
    if not isinstance(manifest, dict):
        raise ValueError("Requires a runtime RPM manifest object")
    packages = manifest.get("packages")
    if manifest.get("schema_version") != 1 or not isinstance(packages, list) or not 1 <= len(packages) <= 128:
        raise ValueError("Requires a version 1 runtime RPM manifest with 1–128 packages")
    names, size = set(), 0
    for item in packages:
        if not isinstance(item, dict):
            raise ValueError("Invalid runtime RPM identity")
        name, sha = item.get("path", ""), item.get("sha256", "")
        if (not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_+.~^-]+\.rpm", name) or
                name in names or not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha)):
            raise ValueError("Invalid or duplicate runtime RPM identity")
        path = directory / name
        if builder.digest(path) != sha:
            raise ValueError("Runtime RPM checksum mismatch: " + name)
        size += path.stat().st_size
        names.add(name)
    if size > 1024**3 or {p.name for p in directory.iterdir()} != names | {"manifest.json"}:
        raise ValueError("Runtime RPM directory has extra files or exceeds 1 GiB")
    return {"manifest_sha256": manifest_sha, "packages": packages, "bytes": size}


def storage_required(size_gib, userspace_bytes, new_export, rpm_bytes=0):
    return {"root_bytes": size_gib * 1024**3,
            "export_bytes": userspace_bytes if new_export else 0,
            "scratch_bytes": userspace_bytes + 4 * rpm_bytes + 1024**3,
            "headroom_bytes": 1024**3}


def target_inputs(profile, kernel, firmware, initramfs, runtime_dir, gpu_dir):
    quest = profile == "quest3"
    if profile not in ("quest3", "pico-neo3"):
        raise ValueError("Unsupported headset profile")
    if not quest and gpu_dir:
        raise ValueError("Pico GPU firmware belongs in --firmware, without --gpu-firmware")
    build, hashes, version, _ = builder.inputs(kernel)
    _, fw, validator = builder.firmware_input(build, firmware if quest else None,
                                              None if quest else firmware, quest)
    initrd = json.loads((initramfs / "build.json").read_text())
    if type(initrd.get("pico_services", False)) is not bool or (quest and initrd.get("pico_services")):
        raise ValueError("Invalid Pico startup selection for this root")
    if (initrd.get("status") != "built" or initrd.get("quest_services") is not quest or
            initrd.get("firmware") != fw or
            initrd.get("kernel_build_sha256") != builder.digest(kernel / "build.json") or
            initrd.get("qemu_transports") is not build["qemu_transports"]):
        raise ValueError("Requires matching headset kernel, startup initramfs and firmware")
    expected = next(a["sha256"] for a in initrd["artifacts"] if a["path"] == "initramfs.img")
    if builder.digest(initramfs / "initramfs.img") != expected:
        raise ValueError("Initramfs checksum mismatch")
    runtime = builder.systemd_runtime.validate(runtime_dir) if runtime_dir else None
    if runtime and runtime["scope"] != "root":
        raise ValueError("The root image requires a root-scope systemd runtime")
    if not quest and (runtime is None or not initrd.get("systemd_runtime_sha256")):
        raise ValueError("Pico requires compatible systemd runtimes in both root and initramfs")
    inputs = {n: builder.digest(ROOT / n) for n in builder.QUEST_INPUTS} if quest else {}
    if quest and initrd.get("quest_inputs_sha256") != inputs:
        raise ValueError("Quest startup sources changed since the initramfs build")
    if not quest and initrd.get("pico_services"):
        inputs = {n: builder.digest(ROOT / n) for n in builder.PICO_INPUTS}
        if initrd.get("pico_inputs_sha256") != inputs:
            raise ValueError("Pico startup sources changed since the initramfs build")
    return build, hashes, version, fw, validator, expected, runtime, inputs


def run(args):
    kernel, initramfs, firmware, output = (p.absolute() for p in (args.kernel, args.initramfs, args.firmware, args.output))
    if output.exists() or output.is_symlink() or any(c in str(p) for p in (kernel, initramfs, firmware, output, ROOT) for c in (",", ":")):
        raise ValueError("Choose a new output directory and paths without commas or colons")
    turnip_dir = args.turnip.absolute() if args.turnip else None
    if turnip_dir and any(c in str(turnip_dir) for c in (",", ":")):
        raise ValueError("Choose a Turnip path without commas or colons")
    turnip = turnip_artifact.validate(turnip_dir) if turnip_dir else None
    monado_dir = args.monado.absolute() if args.monado else None
    if monado_dir and any(c in str(monado_dir) for c in (",", ":")):
        raise ValueError("Choose a Monado path without commas or colons")
    monado = monado_artifact.package(monado_dir) if monado_dir else None
    ssc_dir = args.ssc.absolute() if args.ssc else None
    if ssc_dir and any(c in str(ssc_dir) for c in (",", ":")):
        raise ValueError("Choose an SSC path without commas or colons")
    ssc = ssc_artifact.package(ssc_dir) if ssc_dir else None
    inventory_path = getattr(args, "ssc_inventory", None)
    inventory_dir = inventory_path.absolute() if inventory_path else None
    if inventory_dir and (args.profile != "pico-neo3" or not ssc or
                          any(c in str(inventory_dir) for c in (",", ":"))):
        raise ValueError("SSC inventory requires a Pico root, its SSC bundle, and a valid path")
    inventory = inventory_artifact.package(inventory_dir, ssc) if inventory_dir else None
    fastrpc_path = getattr(args, "fastrpc", None)
    fastrpc_dir = fastrpc_path.absolute() if fastrpc_path else None
    if fastrpc_dir and (args.profile != "pico-neo3" or any(c in str(fastrpc_dir) for c in (",", ":"))):
        raise ValueError("FastRPC requires a Pico root and a path without commas or colons")
    fastrpc = fastrpc_artifact.package(fastrpc_dir) if fastrpc_dir else None
    if monado and monado.get('ssc_dependency') is not None and monado['ssc_dependency'] != ssc:
        raise ValueError("The Pico Monado runtime requires its exact SSC bundle")
    lepton_dir = args.lepton.absolute() if args.lepton else None
    lepton_host_dir = args.lepton_host.absolute() if args.lepton_host else None
    if bool(lepton_dir) != bool(lepton_host_dir):
        raise ValueError("Lepton requires its matching native and Android Fossilize host bundle")
    if any(c in str(p) for p in (lepton_dir, lepton_host_dir) if p for c in (",", ":")):
        raise ValueError("Choose a Lepton path without commas or colons")
    lepton = lepton_artifact.validate(lepton_dir, lepton_host_dir) if lepton_dir else None
    gpu_dir = args.gpu_firmware.absolute() if args.gpu_firmware else None
    runtime_dir = args.systemd_runtime.absolute() if args.systemd_runtime else None
    if args.reuse_export and args.userspace_export:
        raise ValueError("Choose one export source")
    export_dir = (args.reuse_export or args.userspace_export)
    export_dir = export_dir.absolute() if export_dir else None
    rpm_dir = args.runtime_rpms.absolute() if args.runtime_rpms else None
    if any(c in str(p) for p in (gpu_dir, runtime_dir, export_dir, rpm_dir) if p for c in (",", ":")):
        raise ValueError("Choose firmware, runtime, export and RPM paths without commas or colons")
    rpms = read_rpms(rpm_dir) if rpm_dir else None
    gpu = builder.firmware.validate(gpu_dir, "gpu") if gpu_dir else None
    assembly.root_arguments(args.root_label)
    if not 6 <= args.size_gib <= 32:
        raise ValueError("Root size must be 6–32 GiB")
    build, hashes, version, fw, validator, expected, runtime, inputs = target_inputs(
        args.profile, kernel, firmware, initramfs, runtime_dir, gpu_dir)
    pico_services = "system/pico/armada-pico-qrtr" in inputs
    if ssc and (args.profile != "pico-neo3" or not pico_services):
        raise ValueError("SSC currently requires the Pico QRTR root services")
    if ssc:
        inputs["tools/ssc-artifact.py"] = builder.digest(ROOT / "tools/ssc-artifact.py")
    if inventory:
        inputs["tools/ssc-inventory-artifact.py"] = builder.digest(ROOT / "tools/ssc-inventory-artifact.py")
    if fastrpc:
        if not pico_services:
            raise ValueError("FastRPC requires the Pico DSP startup services")
        inputs["tools/fastrpc-artifact.py"] = builder.digest(ROOT / "tools/fastrpc-artifact.py")
    if pico_services:
        for name in ("tools/pico-services.py", "system/pico/armada-pico-qrtr-root.service",
                     "system/pico/armada-pico-pd-mapper-root.service"):
            inputs[name] = builder.digest(ROOT / name)
    runtime_sha = builder.digest(runtime_dir / "manifest.json") if runtime_dir else None
    if gpu and (gpu["reference_build"] != fw["reference_build"] or gpu["source_image_sha256"] != fw["source_image_sha256"]):
        raise ValueError("GPU and ADSP firmware must come from the same declared reference image")
    inputs["tools/build-headset-root.py"] = builder.digest(Path(__file__))
    inputs["tools/build-headset-initramfs.py"] = builder.digest(Path(builder.__file__))
    inputs["profiles/kernel/" + args.profile + ".json"] = builder.digest(ROOT / "profiles/kernel" / (args.profile + ".json"))
    inputs["tools/" + Path(validator.__file__).name] = builder.digest(Path(validator.__file__))
    if args.profile == "pico-neo3":
        inputs["tools/prepare-pico-dsp.py"] = builder.digest(Path(builder.pico_firmware.dsp.__file__))
    if runtime:
        inputs["tools/systemd-runtime.py"] = builder.digest(Path(builder.systemd_runtime.__file__))
    inputs["tools/monado-artifact.py"] = builder.digest(ROOT / "tools/monado-artifact.py")
    if lepton:
        inputs["tools/lepton-artifact.py"] = builder.digest(ROOT / "tools/lepton-artifact.py")
        inputs["tools/prepare-lepton.py"] = builder.digest(ROOT / "tools/prepare-lepton.py")
    inputs["tools/turnip-artifact.py"] = builder.digest(ROOT / "tools/turnip-artifact.py")
    inputs["tools/prepare-quest-firmware.py"] = builder.digest(ROOT / "tools/prepare-quest-firmware.py")
    image = json.loads(subprocess.check_output(
        [args.engine, "image", "inspect", args.image], text=True, timeout=30))[0]
    if image["Architecture"] != "arm64" or image["Os"] != "linux":
        raise ValueError("Root build image must be Linux ARM64")
    export_source = None
    if export_dir:
        export_source = read_export(export_dir, image["Id"] if args.reuse_export else None)
    output.mkdir(parents=True)
    target = {"profile": args.profile, "pico_services": pico_services,
              "firmware_metadata": "quest-firmware.json" if args.profile == "quest3" else "pico-firmware.json"}
    (output / "root-profile.json").write_text(json.dumps(target, indent=2) + "\n")
    if turnip:
        (output / "turnip.json").write_text(json.dumps(turnip, indent=2) + "\n")
    if monado:
        (output / "monado.json").write_text(json.dumps(monado, indent=2) + "\n")
    if lepton:
        (output / "lepton.json").write_text(json.dumps(lepton, indent=2) + "\n")
    if ssc:
        (output / "ssc.json").write_text(json.dumps(ssc, indent=2) + "\n")
    if inventory:
        (output / "ssc-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
    if fastrpc:
        (output / "fastrpc.json").write_text(json.dumps(fastrpc, indent=2) + "\n")
    if rpms:
        (output / "runtime-rpms.json").write_text(json.dumps(rpms, indent=2) + "\n")
    report = {"status": "started", "profile": args.profile, "pico_services": pico_services, "systemd_runtime_sha256": runtime_sha,
              "turnip": turnip, "monado": monado, "lepton": lepton, "ssc": ssc, "ssc_inventory": inventory, "fastrpc": fastrpc, "gpu_firmware": gpu, "export_source": export_source, "target": "headset-root-offline", "hardware_flash_image": False,
              "hardware_boot_verified": False, "kernel_variant": build["variant"], "qemu_transports": build["qemu_transports"],
              "kernel_build_sha256": builder.digest(kernel / "build.json"), "kernel_inputs_sha256": hashes,
              "initramfs_sha256": expected, "firmware": fw,
              "container_image": export_source["container_image"] if export_source else image["Id"],
              "build_container_image": image["Id"], "runtime_rpms": rpms, "inputs_sha256": inputs,
              "root_label": args.root_label, "size_gib": args.size_gib, "commands": []}
    container = None; started = time.monotonic()
    try:
        if monado_dir and not args.userspace_export and not rpms:
            probe = """import ctypes, os, subprocess
prefix='/opt/armada-vr/monado'
data=subprocess.check_output(['ldd','-r',prefix+'/bin/monado-service'],text=True,stderr=subprocess.STDOUT)
print(data,flush=True)
if 'not found' in data or 'undefined symbol' in data:
    raise SystemExit('Monado runtime image has unresolved dependencies')
for name in ('libopenxr_monado.so','libmonado.so'):
    ctypes.CDLL(prefix+'/lib64/'+name,mode=os.RTLD_NOW)
print('Monado runtime image preflight: passed')
"""
            command = [args.engine, "run", "--rm", "--read-only", "--network", "none",
                       "--memory", "256m", "--memory-swap", "256m", "--cpus", "1", "--pids-limit", "64",
                       "-v", f"{monado_dir / monado_artifact.PREFIX}:/opt/armada-vr/monado:ro",
                       *(["-v", f"{ssc_dir / ssc_artifact.PREFIX}:/opt/armada-vr/ssc:ro"] if ssc_dir else []),
                       "-e", "LD_LIBRARY_PATH=/opt/armada-vr/monado/lib64:/opt/armada-vr/ssc/lib64",
                       "--entrypoint", "python3", image["Id"], "-B", "-c", probe]
            report["commands"].append(command)
            with (output / "monado-preflight.txt").open("wb") as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
        storage = storage_required(args.size_gib, export_source["expanded_bytes"] if export_source else image["Size"],
                                   not export_source, rpms["bytes"] if rpms else 0)
        if lepton:
            storage["scratch_bytes"] += 2 * lepton["inventory"]["expanded_bytes"]
            storage["scratch_bytes"] += 2 * sum(i["bytes"] for i in lepton["host"]["files"])
        if runtime:
            storage["scratch_bytes"] += 2 * sum((runtime_dir / i["path"]).stat().st_size for i in runtime["files"] if "sha256" in i)
        required = sum(storage.values())
        available = shutil.disk_usage(output).free
        report["storage_preflight"] = {"available_bytes": available, "required_working_bytes": required,
                                       "container_scratch_included": True, "estimates": storage}
        if available < required:
            raise ValueError(f"Root assembly requires at least {required / 1024**3:.2f} GiB free including estimated scratch; "
                             f"only {available / 1024**3:.2f} GiB is available")
        if export_source:
            archive = Path(export_source["path"])
            report["export_sha256"] = export_source["sha256"]
        else:
            command = [args.engine, "create", image["Id"]]
            report["commands"].append(command)
            container = subprocess.check_output(command, text=True, timeout=30).strip()
            command = [args.engine, "export", container]; report["commands"].append(command)
            archive = output / "rootfs.tar"
            with archive.open("xb") as stream:
                subprocess.run(command, stdout=stream, timeout=180, check=True)
            report["export_sha256"] = builder.digest(archive)
        command = [args.engine, "run", "--rm", "--init", "--network", "none", "--memory", "2g", "--memory-swap", "2g",
                   "--cpus", "2", "--pids-limit", "256", "-v", f"{output}:/output", "-v", f"{archive}:/rootfs.tar:ro", "-v", f"{kernel}:/kernel:ro",
                   "-v", f"{firmware}:/firmware:ro",
                   *(["-v", f"{ROOT / 'system/quest/armada-quest-boot.service'}:/quest.service:ro"] if args.profile == "quest3" else []),
                   *([arg for name in ("tools/pico-services.py", "system/pico", "profiles/qcom-services.json",
                                        "patches/pd-mapper",
                                        "patches/qrtr/0001-check-qmi-packet-bounds.patch")
                      for arg in ("-v", f"{ROOT / name}:/repo/{name}:ro")] if pico_services else []),
                   *(["-v", f"{runtime_dir}:/systemd-runtime:ro", "-v", f"{ROOT / 'tools/systemd-runtime.py'}:/systemd-runtime.py:ro"] if runtime_dir else []),
                   *(["-v", f"{gpu_dir}:/gpu-firmware:ro"] if gpu_dir else []),
                   *(["-v", f"{rpm_dir}:/runtime-rpms:ro"] if rpm_dir else []),
                   *(["-v", f"{turnip_dir}:/turnip:ro"] if turnip_dir else []),
                   *(["-v", f"{ssc_dir}:/ssc:ro", "-v", f"{ROOT / 'tools/ssc-artifact.py'}:/repo/tools/ssc-artifact.py:ro"] if ssc_dir else []),
                   *(["-v", f"{inventory_dir}:/ssc-inventory:ro", "-v", f"{ROOT / 'tools/ssc-inventory-artifact.py'}:/repo/tools/ssc-inventory-artifact.py:ro"] if inventory_dir else []),
                   *(["-v", f"{fastrpc_dir}:/fastrpc:ro", "-v", f"{ROOT / 'tools/fastrpc-artifact.py'}:/repo/tools/fastrpc-artifact.py:ro"] if fastrpc_dir else []),
                   *(["-v", f"{monado_dir}:/monado:ro", "-v", f"{ROOT / 'tools/monado-artifact.py'}:/repo/tools/monado-artifact.py:ro"] if monado_dir else []),
                   *(["-v", f"{lepton_dir}:/lepton:ro", "-v", f"{lepton_host_dir}:/lepton-host:ro", "-v", f"{ROOT / 'tools/lepton-artifact.py'}:/repo/tools/lepton-artifact.py:ro"] if lepton_dir else []),
                   "--entrypoint", "timeout", image["Id"], "--kill-after=5", "900", "bash", "-c", SCRIPT,
                   "build-root", version, str(args.size_gib), args.root_label]
        report["commands"].append(command)
        with (output / "build.log").open("wb") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=920, check=True)
        if builder.inputs(kernel)[1] != hashes or validator.validate(firmware) != fw:
            raise ValueError("Kernel or firmware changed during root construction")
        if runtime_dir and (builder.systemd_runtime.validate(runtime_dir) != runtime or
                            builder.digest(runtime_dir / "manifest.json") != runtime_sha):
            raise ValueError("Systemd runtime changed during root construction")
        if builder.digest(archive) != (export_source["archive_sha256"] if export_source else report["export_sha256"]):
            raise ValueError("Userspace export changed during construction")
        if export_source and builder.digest(export_dir / "manifest.json") != export_source["manifest_sha256"]:
            raise ValueError("Source export manifest changed during construction")
        if gpu_dir and builder.firmware.validate(gpu_dir, "gpu") != gpu:
            raise ValueError("GPU firmware changed during root construction")
        if turnip_dir and turnip_artifact.validate(turnip_dir) != turnip:
            raise ValueError("Turnip changed during root construction")
        if ssc_dir and ssc_artifact.package(ssc_dir) != ssc:
            raise ValueError("SSC changed during root construction")
        if inventory_dir and inventory_artifact.package(inventory_dir, ssc) != inventory:
            raise ValueError("SSC inventory changed during root construction")
        if fastrpc_dir and fastrpc_artifact.package(fastrpc_dir) != fastrpc:
            raise ValueError("FastRPC changed during root construction")
        if monado_dir and monado_artifact.package(monado_dir) != monado:
            raise ValueError("Monado changed during root construction")
        if lepton_dir and lepton_artifact.validate(lepton_dir, lepton_host_dir) != lepton:
            raise ValueError("Lepton changed during root construction")
        if rpm_dir and read_rpms(rpm_dir) != rpms:
            raise ValueError("Runtime RPMs changed during root construction")
        if any(builder.digest(ROOT / n) != sha for n, sha in inputs.items()):
            raise ValueError("Root build sources changed during construction")
        names = ["rootfs.ext4", "root-profile.json", "contents.json", "root-settings.json", "filesystem.txt", "fsck.log", "tool-versions.txt"]
        if pico_services:
            names += ["pico-services.json", "pico-services-dependencies.txt"]
        if turnip:
            names += ["turnip.json", "turnip-dependencies.txt", "turnip-loader.txt"]
        if ssc:
            names += ["ssc.json", "ssc-loader.json"]
        if inventory:
            names += ["ssc-inventory.json", "ssc-inventory-loader.json"]
        if fastrpc:
            names += ["fastrpc.json", "fastrpc-loader.json"]
        if monado:
            names += ["monado.json", "monado-dependencies.txt", "monado-loader.txt"]
            if (output / "monado-preflight.txt").exists():
                names.append("monado-preflight.txt")
        if rpms:
            names += ["runtime-rpms.json", "runtime-rpms-install.txt"]
        if lepton:
            names += ["lepton.json", "lepton-runtime.json", "lepton-dependencies.txt"]
        report["artifacts"] = [{"path": n, "bytes": (output / n).stat().st_size, "sha256": builder.digest(output / n)}
                               for n in names]
        contents = json.loads((output / "contents.json").read_text())
        report["quest_startup_unit_sha256"] = contents["quest_startup_unit_sha256"]
        report["systemd_runtime_verified"] = contents["systemd_runtime_verified"]
        report.update(status="built", sha256={a["path"]: a["sha256"] for a in report["artifacts"]})
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report.update(status="failed", error=str(error)); raise
    finally:
        if container:
            subprocess.run([args.engine, "rm", container], check=True, timeout=30)
        report["elapsed_seconds"] = round(time.monotonic() - started, 3)
        (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    print(output / "manifest.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("initramfs", type=Path)
    parser.add_argument("--profile", choices=("quest3", "pico-neo3"), default="quest3")
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--image", default="localhost/armada-vr:runtime")
    parser.add_argument("--systemd-runtime", type=Path, help="Prepared root-scope systemd runtime; required for Pico")
    parser.add_argument("--turnip", type=Path, help="Verified Mesa Turnip build directory")
    parser.add_argument("--monado", type=Path, help="Verified native Monado build directory")
    parser.add_argument("--lepton", type=Path, help="Prepared Lepton execution bundle with source-built init")
    parser.add_argument("--lepton-host", type=Path, help="Manifest-pinned native and Android Fossilize host bundle")
    parser.add_argument("--gpu-firmware", type=Path, help="Verified GPU bundle from the same Quest reference image")
    parser.add_argument("--reuse-export", type=Path, help="Reuse rootfs.tar from a successful root build with the same userspace image")
    parser.add_argument("--userspace-export", type=Path, help="Use a verified prior root export (tar or tar.zst); --image supplies only build tools")
    parser.add_argument("--ssc", type=Path, help="Verified native sensor runtime bundle for the Pico root")
    parser.add_argument("--ssc-inventory", type=Path, help="Verified metadata collector package; no automatic startup")
    parser.add_argument("--fastrpc", type=Path, help="Verified Pico FastRPC bundle; installs libraries without starting sensor streams")
    parser.add_argument("--runtime-rpms", type=Path, help="Manifest-pinned RPMs; signatures and dependencies must validate inside the selected userspace")
    parser.add_argument("--engine", default=os.environ.get("CONTAINER_ENGINE", "docker"))
    parser.add_argument("--root-label", default="armada-vr-root")
    parser.add_argument("--size-gib", type=int, default=8)
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
