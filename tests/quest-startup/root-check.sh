#!/bin/bash
set -euo pipefail
trap 'echo "QEMU_HANDOFF_FIXTURE_FAIL line=$LINENO command=$BASH_COMMAND" >&2' ERR
IFS= read -r -d '' model < /sys/firmware/devicetree/base/compatible
[[ $model == linux,dummy-virt && ! -e /etc/initrd-release ]]
[[ $(stat -f -c %t /) == 794c7630 ]]
before=$(< /run/quest-before-pid)
after=$(systemctl show -p MainPID --value armada-quest-boot.service)
[[ $before != 0 && $before == "$after" ]]
systemctl is-active --quiet armada-quest-boot.service
/usr/bin/quest-qmi-probe
echo "ARMADA_QUEST_ROOT_HANDOFF_PASS pid=$after"
if systemctl restart armada-quest-boot.service; then
    echo ARMADA_QUEST_UNEXPECTED_RESTART >&2
    exit 1
fi
[[ $(systemctl show -p MainPID --value armada-quest-boot.service) == "$before" ]]
systemctl is-active --quiet armada-quest-boot.service
/usr/bin/quest-qmi-probe
echo ARMADA_QUEST_ROOT_RESTART_REFUSED

if [[ ${ARMADA_QUEST_CASE:-root} == root-fault ]]; then
    mapper=$(pgrep -P "$before" -x pd-mapper)
    [[ $mapper =~ ^[0-9]+$ ]]
    echo "ARMADA_QUEST_MAPPER_FAULT_INJECTED pid=$mapper"
    kill -TERM "$mapper"
    sleep 30
    echo ARMADA_QUEST_MAPPER_FAULT_NOT_HANDLED
    exit 1
fi
