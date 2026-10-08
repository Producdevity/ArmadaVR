#!/bin/bash
set -euo pipefail
trap 'echo "QEMU_HANDOFF_FIXTURE_FAIL line=$LINENO command=$BASH_COMMAND" >&2' ERR
IFS= read -r -d '' model < /sys/firmware/devicetree/base/compatible
[[ $model == linux,dummy-virt && $(stat -f -c %t /sysroot) == 794c7630 ]]
systemctl show -p MainPID --value armada-quest-boot.service > /run/quest-before-pid
if [[ ${ARMADA_QUEST_PACKAGED_ROOT:-0} == 1 ]]; then
    cmp /usr/lib/systemd/system/armada-quest-boot.service /sysroot/usr/lib/systemd/system/armada-quest-boot.service
    [[ ! -L /sysroot/etc/systemd/system/multi-user.target.wants/armada-vr-lab.service ]]
    echo ARMADA_QUEST_PACKAGED_ROOT_PASS
else
    cp /usr/lib/systemd/system/armada-quest-boot.service /sysroot/usr/lib/systemd/system/
fi
probe=/usr/libexec/armada-quest-fixture
[[ ! -e /sysroot$probe ]]
mkdir -p /sysroot$probe
cp -a /usr/lib64/libqrtr.so* /sysroot$probe/
cp /qmi-probe /sysroot$probe/
chroot /sysroot /usr/bin/env LD_LIBRARY_PATH=$probe \
    /lib/ld-linux-aarch64.so.1 --list $probe/qmi-probe
echo ARMADA_QUEST_ROOT_PROBE_LINKS_PASS
mkdir -p /sysroot/usr/libexec
cp /quest-root-check /sysroot/usr/libexec/quest-root-check
chmod 755 /sysroot/usr/libexec/quest-root-check
cp /quest-root-check.service /sysroot/etc/systemd/system/
mkdir -p /sysroot/etc/systemd/system/multi-user.target.wants
ln -s /etc/systemd/system/quest-root-check.service /sysroot/etc/systemd/system/multi-user.target.wants/
if [[ ${ARMADA_QUEST_CASE:-root} == root && ! -L /sysroot/etc/systemd/system/multi-user.target.wants/armada-vr-lab.service ]]; then
    ln -s /etc/systemd/system/armada-vr-lab.service /sysroot/etc/systemd/system/multi-user.target.wants/
fi
echo ARMADA_QUEST_ROOT_FILES_STAGED_IN_RAM
