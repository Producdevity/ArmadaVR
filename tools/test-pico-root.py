#!/usr/bin/env python3
"""Boot a matching Pico QEMU root image without changing its packaged runtime."""
import argparse
import base64
import gzip
import importlib.util
import json
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("initramfs", ROOT / "tools/build-headset-initramfs.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
spec = importlib.util.spec_from_file_location("vm_control", ROOT / "tools/vm_control.py")
vm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vm)

HANDOFF = r'''#!/bin/bash
set -euo pipefail
IFS= read -r -d '' model < /sys/firmware/devicetree/base/compatible
[[ $model == linux,dummy-virt && $(stat -f -c %t /sysroot) == 794c7630 ]]
grep -qw 'armada.pico_root_test=1' /proc/cmdline
[[ ! -e /sysroot/etc/systemd/system/armada-pico-test-agent.service ]]
mkdir -p /sysroot/etc/systemd/system/multi-user.target.wants
cp /pico-agent.service /sysroot/etc/systemd/system/armada-pico-test-agent.service
ln -s /etc/systemd/system/armada-pico-test-agent.service /sysroot/etc/systemd/system/multi-user.target.wants/
cp /pico-probe.py /run/pico-probe.py
cp /pico-expected.json /run/pico-expected.json
echo ARMADA_PICO_PACKAGED_ROOT_HANDOFF
'''

HANDOFF_UNIT = '''[Unit]
Description=Enable the private agent in the Pico QEMU RAM overlay
DefaultDependencies=no
After=initrd-root-fs.target systemd-volatile-root.service
Before=initrd-cleanup.service initrd-switch-root.target
OnFailure=emergency.target

[Service]
Type=oneshot
ExecStart=/pico-handoff
RemainAfterExit=yes
StandardOutput=journal+console
StandardError=journal+console
'''

AGENT_UNIT = '''[Unit]
Description=Private Pico root verification agent

[Service]
ExecStart=/usr/bin/qemu-ga --method=virtio-serial --path=/dev/virtio-ports/org.armada.pico_root_test.0
StandardOutput=journal+console
StandardError=journal+console
'''

PROBE = r'''import hashlib,json,os,stat,struct,subprocess
from pathlib import Path
def sha(p):
    with p.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def command(args,timeout=30):
    return subprocess.check_output(args,text=True,stderr=subprocess.STDOUT,timeout=timeout)
assert os.getuid()==0 and 'armada.pico_root_test=1' in Path('/proc/cmdline').read_text()
assert Path('/sys/firmware/devicetree/base/compatible').read_bytes().split(b'\0')[0]==b'linux,dummy-virt'
expected=json.loads(Path('/run/pico-expected.json').read_text())
if expected['enforcing_boot']:
    assert {'selinux=1','enforcing=1'} <= set(Path('/proc/cmdline').read_text().split())
assert command(['findmnt','-n','-o','FSTYPE','/']).strip()=='overlay'
assert Path('/sys/block/vda/ro').read_text().strip()=='1'
for line in Path('/proc/self/mountinfo').read_text().splitlines():
    if ' - ext4 /dev/vda ' in line:assert 'ro' in line.split()[5].split(',')
assert command(['systemctl','is-system-running','--wait'],90).strip()=='running'
assert sha(Path('/usr/share/armada-vr/kernel-build.json'))==expected['kernel_build_sha256']
assert not Path('/usr/lib/systemd/system/armada-quest-boot.service').exists()
for item in expected['modules']:
    assert sha(Path('/usr/lib/modules')/item['path'])==item['sha256'],item['path']
assert {str(p.relative_to('/usr/lib/modules')) for p in Path('/usr/lib/modules').rglob('*.ko')}=={i['path'] for i in expected['modules']}
for item in expected['firmware']:
    assert sha(Path('/usr/lib/firmware')/item['path'])==item['sha256'],item['path']
runtime_path=Path('/usr/share/armada-vr/root-systemd.json')
assert sha(runtime_path)==expected['systemd_runtime_sha256']
runtime=json.loads(runtime_path.read_text())
for item in runtime['files']:
    p=Path('/')/item['path'].removeprefix('files/')
    if 'link' in item:assert p.is_symlink() and os.readlink(p)==item['link'],item['path']
    else:assert sha(p)==item['sha256'] and stat.S_IMODE(p.stat().st_mode)==item['mode'],item['path']
for name,digest in runtime['native_abi_sha256'].items():assert sha(Path('/')/name)==digest,name
packages={}
for name,program in (('ssc','bin/ssccli'),('ssc_inventory','libexec/ssc-inventory'),
                     ('fastrpc','lib64/libpico-sensors.so.1'),('monado','bin/monado-service')):
    record=expected.get(name)
    if record is None:continue
    installed=name.replace('_','-')
    prefix=Path('/opt/armada-vr')/installed
    tree=record['contents']
    if name=='fastrpc':
        files={n:v for n,v in tree.items() if 'sha256' in v}
        links={n:v['symlink'] for n,v in tree.items() if 'symlink' in v}
    else:
        files=tree['files'];links=tree.get('links',tree.get('symlinks',{}))
    assert sha(Path('/usr/share/armada-vr')/(installed+'-build.json'))==record['build_sha256']
    assert {str(p.relative_to(prefix)) for p in prefix.rglob('*') if p.is_file() and not p.is_symlink()}==set(files)
    assert {str(p.relative_to(prefix)):os.readlink(p) for p in prefix.rglob('*') if p.is_symlink()}==links
    for path,item in files.items():
        p=prefix/path
        assert not p.is_symlink() and sha(p)==item['sha256'] and stat.S_IMODE(p.stat().st_mode)==item['mode'],str(p)
    libraries=str(prefix/'lib64')+':/opt/armada-vr/ssc/lib64:/usr/lib64'
    trace=command(['/usr/bin/env','LD_TRACE_LOADED_OBJECTS=1','LD_WARN=yes','LD_BIND_NOW=yes','LD_VERBOSE=yes',
                   '/lib/ld-linux-aarch64.so.1','--library-path',libraries,str(prefix/program)])
    assert 'not found' not in trace and 'undefined symbol' not in trace,(name,trace)
    packages[name]={'files':len(files),'links':len(links),'loader_relocations':True,'program_executed':False}
shared=[i['path'].removeprefix('files/') for i in runtime['files'] if '/libsystemd-shared-' in i['path'] and 'sha256' in i]
assert len(shared)==1 and '/'+shared[0] in Path('/proc/1/maps').read_text()
for name in runtime['commands']:
    data=command(['/lib/ld-linux-aarch64.so.1','--library-path','/usr/lib64:/opt/armada-vr/systemd/usr/lib:/opt/armada-vr/systemd/usr/lib/systemd','--list','/opt/armada-vr/systemd/'+name])
    assert 'not found' not in data,name
command(['loginctl','enable-linger','vr'])
command(['systemctl','start','user@1000.service'])
assert Path('/run/user/1000/bus').is_socket()
assert command(['systemctl','is-active','user@1000.service']).strip()=='active'
probe=Path('/run/pico-isolation.py')
probe.write_text("from pathlib import Path\nassert 'NoNewPrivs:\\t1' in Path('/proc/self/status').read_text()\ntry: Path('/usr/pico-forbidden').write_text('bad')\nexcept OSError as e: assert e.errno==30,e\nelse: raise AssertionError('system writable')\nPath('/tmp/pico-private-probe').write_text('private')\nprint('PICO_ROOT_ISOLATION_PASS')\n".replace('\\t','\t'))
isolation=command(['systemd-run','--unit=armada-pico-isolation','--wait','--pipe','--property=NoNewPrivileges=yes','--property=PrivateTmp=yes','--property=PrivateNetwork=yes','--property=ProtectSystem=strict','/usr/bin/python3',str(probe)],45)
assert 'PICO_ROOT_ISOLATION_PASS' in isolation and not Path('/tmp/pico-private-probe').exists()
Path('/pico-volatile-write').write_text('guest RAM only\n')
assert Path('/sys/block/vda/stat').read_text().split()[4]=='0'
assert command(['systemctl','show','--property=NFailedUnits','--value']).strip()=='0'
selinux=Path('/run/pico-selinux');selinux.mkdir()
command(['mount','-t','selinuxfs','-o','ro','none',str(selinux)])
enforcing=int((selinux/'enforce').read_text())
version,sequence,status_enforcing,policyload,deny_unknown=struct.unpack('=5I',(selinux/'status').read_bytes()[:20])
assert enforcing==status_enforcing and version==1 and sequence%2==0
if expected['enforcing_boot']:assert enforcing==1
policy={'enforcing_flag':bool(enforcing),'policy_load_count':policyload,'policy_loaded':policyload>0,
        'maximum_policy_version':int((selinux/'policyvers').read_text()),'deny_unknown':bool(deny_unknown)}
command(['umount',str(selinux)])
print(json.dumps({'status':'passed','boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
    'taint':int(Path('/proc/sys/kernel/tainted').read_text()),'runtime_files':len(runtime['files']),
    'runtime_commands':len(runtime['commands']),'modules':len(expected['modules']),'firmware':len(expected['firmware']),
    'user_session':True,'service_isolation':True,'root_overlay':True,'root_block_readonly':True,'disk_writes':0,
    'selinux':policy,'selinux_policy_acceptance':False,'native_packages':packages}),flush=True)
'''


def root_input(kernel, initramfs, directory):
    build, hashes, _, _ = builder.inputs(kernel)
    initrd = json.loads((initramfs / "build.json").read_text())
    report = json.loads((directory / "manifest.json").read_text())
    if (build.get("repository") != "bytedance/neo3-kernel" or build.get("qemu_transports") is not True or
            initrd.get("status") != "built" or initrd.get("qemu_transports") is not True or
            initrd.get("quest_services") is not False or report.get("status") != "built" or
            report.get("profile") != "pico-neo3" or report.get("qemu_transports") is not True):
        raise ValueError("Requires the Pico QEMU kernel, initramfs and completed Pico QEMU root")
    kernel_sha = builder.digest(kernel / "build.json")
    initrd_sha = builder.digest(initramfs / "initramfs.img")
    if (initrd.get("kernel_build_sha256") != kernel_sha or report.get("kernel_build_sha256") != kernel_sha or
            report.get("initramfs_sha256") != initrd_sha or
            not any(a["path"] == "initramfs.img" and a["sha256"] == initrd_sha for a in initrd["artifacts"])):
        raise ValueError("Root, initramfs and kernel identities must match")
    image = directory / "rootfs.ext4"
    if builder.digest(image) != report["sha256"]["rootfs.ext4"]:
        raise ValueError("Root image checksum mismatch")
    contents = directory / "contents.json"
    if builder.digest(contents) != report["sha256"]["contents.json"]:
        raise ValueError("Root contents record checksum mismatch")
    if report.get("systemd_runtime_verified") is not True or not report.get("systemd_runtime_sha256"):
        raise ValueError("Pico root requires verified packaged systemd runtime")
    expected = json.loads(contents.read_text())
    expected.update(kernel_build_sha256=kernel_sha, systemd_runtime_sha256=report["systemd_runtime_sha256"])
    return hashes, report, expected, image, initrd_sha


def cpio(entries):
    data = bytearray()
    for i, (name, mode, payload) in enumerate([*entries, ("TRAILER!!!", 0, b"")], 1):
        name = name.encode() + b"\0"
        fields = (i, mode, 0, 0, 1, 0, len(payload), 0, 0, 0, 0, len(name), 0)
        data += b"070701" + "".join(f"{v:08x}" for v in fields).encode() + name
        data += b"\0" * (-len(data) % 4) + payload
        data += b"\0" * (-len(data) % 4)
    return bytes(data)


def execute(directory, arguments, timeout):
    pid = vm.agent_request(directory, "guest-exec", {"path": arguments[0], "arg": arguments[1:], "capture-output": True})["pid"]
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = vm.agent_request(directory, "guest-exec-status", {"pid": pid})
        if result.get("exited"):
            for key in ("out-data", "err-data"):
                if key in result:
                    result[key] = base64.b64decode(result[key]).decode(errors="replace")
            return result
        time.sleep(0.5)
    raise TimeoutError("Pico root guest command did not finish")


def run(args):
    kernel, initramfs, directory, output = (p.resolve() for p in (args.kernel, args.initramfs, args.rootfs, args.output))
    if output.exists() or any(c in str(p) for p in (kernel, initramfs, directory, output) for c in (",", ":")):
        raise ValueError("Choose a new output directory and paths without commas or colons")
    hashes, root, expected, disk, initrd_sha = root_input(kernel, initramfs, directory)
    expected['enforcing_boot'] = args.enforcing_boot
    if shutil.disk_usage(output.parent).free < 1024**3:
        raise ValueError("Requires 1 GiB free for bounded test evidence")
    output.mkdir()
    record = {"status": "started", "hardware_boot_verified": False, "hardware_recovery_verified": False,
              "kernel_sha256": hashes["Image"], "initramfs_sha256": initrd_sha,
              "root_sha256": root["sha256"]["rootfs.ext4"], "runner_sha256": builder.digest(Path(__file__)),
              "enforcing_boot_requested": args.enforcing_boot,
              "network": "none", "writable_state": "guest RAM only", "runtime_replaced_in_fixture": False}
    process = None
    started = time.monotonic()
    try:
        files = [("pico-handoff", stat.S_IFREG | 0o755, HANDOFF.encode()),
                 ("pico-agent.service", stat.S_IFREG | 0o644, AGENT_UNIT.encode()),
                 ("pico-probe.py", stat.S_IFREG | 0o644, PROBE.encode()),
                 ("pico-expected.json", stat.S_IFREG | 0o644, json.dumps(expected).encode()),
                 ("etc/systemd/system/pico-handoff.service", stat.S_IFREG | 0o644, HANDOFF_UNIT.encode()),
                 ("etc/systemd/system/initrd.target.wants/pico-handoff.service", stat.S_IFLNK | 0o777,
                  b"/etc/systemd/system/pico-handoff.service")]
        for name, _, data in files[:4]:
            (output / name).write_bytes(data)
        boot = output / "test-initramfs.img"
        boot.write_bytes((initramfs / "initramfs.img").read_bytes() + gzip.compress(cpio(files), mtime=0))
        record["test_initramfs_sha256"] = builder.digest(boot)
        with tempfile.TemporaryDirectory(prefix="pico-root-", dir="/tmp") as sockets:
            command = [args.qemu, "-name", "ArmadaVR Pico root acceptance", "-machine", "virt,secure=on,virtualization=on",
                       "-accel", "tcg", "-cpu", "cortex-a76", "-smp", "2", "-m", "4096", "-nodefaults",
                       "-nographic", "-monitor", "none", "-serial", "stdio", "-no-reboot", "-nic", "none",
                       "-kernel", str(kernel / "Image"), "-initrd", str(boot),
                       "-drive", f"file={disk},if=none,id=root,format=raw,readonly=on", "-device", "virtio-blk-device,drive=root",
                       "-object", "rng-random,id=rng,filename=/dev/urandom", "-device", "virtio-rng-device,rng=rng",
                       "-device", "virtio-serial-device", "-chardev", f"socket,path={sockets}/agent.sock,server=on,wait=off,id=agent",
                       "-device", "virtserialport,chardev=agent,name=org.armada.pico_root_test.0", "-append",
                       "console=ttyAMA0 earlycon " + ("selinux=1 enforcing=1" if args.enforcing_boot else "selinux=0") +
                       " audit=0 root=LABEL=" + root["root_label"] +
                       " ro rootfstype=ext4 rootflags=noload systemd.volatile=overlay rd.fstab=0 rd.luks=0 rd.lvm=0 rd.md=0"
                       " rd.systemd.gpt_auto=0 systemd.gpt_auto=0 rd.shell=0 rd.emergency=poweroff rd.retry=15 rd.timeout=30"
                       " initcall_blacklist=hwinfo_init armada.pico_root_test=1 systemd.unified_cgroup_hierarchy=1"]
            record["command"] = command
            with (output / "boot.log").open("xb") as log:
                process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 180
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError("Pico root exited before its guest agent; inspect boot.log")
                    try:
                        vm.agent_request(sockets, "guest-ping")
                        break
                    except (OSError, ValueError):
                        time.sleep(1)
                else:
                    raise TimeoutError("Pico root startup did not complete")
                probe = execute(sockets, ["/usr/bin/python3", "/run/pico-probe.py"], 240)
                (output / "probe.json").write_text(json.dumps(probe, indent=2) + "\n")
                journal = execute(sockets, ["/usr/bin/journalctl", "-b", "--no-pager", "-n", "500", "-o", "short-monotonic"], 45)
                (output / "journal.json").write_text(json.dumps(journal, indent=2) + "\n")
                state = execute(sockets, ["/bin/bash", "-ec", "cat /proc/self/mountinfo; systemctl --failed --plain --no-legend; cat /proc/1/maps"], 30)
                (output / "guest-state.json").write_text(json.dumps(state, indent=2) + "\n")
                try:
                    vm.agent_request(sockets, "guest-shutdown", {"mode": "powerdown"})
                except ConnectionError:
                    pass
                record["returncode"] = process.wait(timeout=60)
            if record["returncode"] != 0 or "reboot: Power down" not in (output / "boot.log").read_text(errors="replace"):
                raise RuntimeError("Pico root did not shut down normally")
            record["normal_powerdown"] = True
            if probe.get("exitcode") != 0 or probe.get("out-truncated"):
                raise RuntimeError("Pico root acceptance probe failed; inspect probe.json")
            record["probe"] = json.loads(probe["out-data"])
            if record["probe"].get("status") != "passed":
                raise RuntimeError("Pico root probe omitted acceptance")
            record["status"] = "passed"
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        record.update(status="failed", error=str(error))
        raise
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        unchanged = (builder.digest(disk) == record["root_sha256"] and
                     builder.digest(kernel / "Image") == hashes["Image"] and
                     builder.digest(initramfs / "initramfs.img") == initrd_sha)
        record["inputs_unchanged"] = unchanged
        if not unchanged:
            record.update(status="failed", error="Test inputs changed")
        record["elapsed_seconds"] = round(time.monotonic() - started, 3)
        (output / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    if record["status"] != "passed":
        raise ValueError(record["error"])
    print(output / "result.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("initramfs", type=Path)
    parser.add_argument("rootfs", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--enforcing-boot", action="store_true", help="Use the physical candidate's SELinux boot flags and report actual policy state")
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
