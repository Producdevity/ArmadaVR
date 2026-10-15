#!/bin/bash
set -euo pipefail
trap 'echo "QEMU_HANDOFF_FIXTURE_FAIL line=$LINENO command=$BASH_COMMAND" >&2' ERR
IFS= read -r -d '' model < /sys/firmware/devicetree/base/compatible
[[ $model == linux,dummy-virt && ! -e /etc/initrd-release ]]
[[ $(stat -f -c %t /) == 794c7630 ]]
before=$(< /run/quest-before-pid)
after=$(systemctl show -p MainPID --value reframed-quest-boot.service)
[[ $before != 0 && $before == "$after" ]]
systemctl is-active --quiet reframed-quest-boot.service
LD_LIBRARY_PATH=/usr/libexec/reframed-quest-fixture /usr/libexec/reframed-quest-fixture/qmi-probe
echo "REFRAMED_QUEST_ROOT_HANDOFF_PASS pid=$after"
if systemctl restart reframed-quest-boot.service; then
    echo REFRAMED_QUEST_UNEXPECTED_RESTART >&2
    exit 1
fi
[[ $(systemctl show -p MainPID --value reframed-quest-boot.service) == "$before" ]]
systemctl is-active --quiet reframed-quest-boot.service
LD_LIBRARY_PATH=/usr/libexec/reframed-quest-fixture /usr/libexec/reframed-quest-fixture/qmi-probe
echo REFRAMED_QUEST_ROOT_RESTART_REFUSED

if [[ ${REFRAMED_QUEST_CASE:-root} == root-fault ]]; then
    mapper=$(pgrep -P "$before" -x pd-mapper)
    [[ $mapper =~ ^[0-9]+$ ]]
    echo "REFRAMED_QUEST_MAPPER_FAULT_INJECTED pid=$mapper"
    kill -TERM "$mapper"
    sleep 30
    echo REFRAMED_QUEST_MAPPER_FAULT_NOT_HANDLED
    exit 1
fi

if [[ ${REFRAMED_QUEST_PACKAGED_ROOT:-0} == 1 ]]; then
    LD_LIBRARY_PATH=/opt/reframed-vr/monado/lib64:/opt/reframed-vr/ssc/lib64 python3 -B /run/quest-root-runtime.py
    systemctl --no-block poweroff
fi
