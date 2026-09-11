#!/bin/bash
set -euo pipefail
trap 'echo "QEMU_HANDOFF_FIXTURE_FAIL line=$LINENO command=$BASH_COMMAND" >&2' ERR
IFS= read -r -d '' model < /sys/firmware/devicetree/base/compatible
[[ $model == linux,dummy-virt && $(stat -f -c %t /sysroot) == 794c7630 ]]
cmp /usr/lib64/libc.so.6 /sysroot/usr/lib64/libc.so.6
systemctl show -p MainPID --value armada-quest-boot.service > /run/quest-before-pid
cp /usr/lib/systemd/system/armada-quest-boot.service /sysroot/usr/lib/systemd/system/
cp -a /usr/lib64/libqrtr.so* /sysroot/usr/lib64/
cp /qmi-probe /sysroot/usr/bin/quest-qmi-probe
mkdir -p /sysroot/usr/libexec
cp /quest-root-check /sysroot/usr/libexec/quest-root-check
chmod 755 /sysroot/usr/libexec/quest-root-check
cp /quest-root-check.service /sysroot/etc/systemd/system/
mkdir -p /sysroot/etc/systemd/system/multi-user.target.wants
ln -s /etc/systemd/system/quest-root-check.service /sysroot/etc/systemd/system/multi-user.target.wants/
echo ARMADA_QUEST_ROOT_FILES_STAGED_IN_RAM
