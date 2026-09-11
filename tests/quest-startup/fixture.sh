#!/bin/bash
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
IFS= read -r -d '' platform < /sys/firmware/devicetree/base/compatible
[[ $platform == linux,dummy-virt && $(< /proc/1/comm) == systemd ]] || exit 1
for module in qrtr qrtr_smd qcom_q6v5_pas adsp_loader_dlkm pmic_glink ucsi_glink; do
    modprobe "$module"
    echo "ARMADA_QUEST_MODULE_PASS=$module"
done
fixture=/run/quest-fixture
mkdir -p "$fixture"/{remoteproc/remoteproc0/device/of_node,rpmsg,typec,usb_role}
mount -t tmpfs tmpfs /sys/firmware
mount -t tmpfs tmpfs /sys/kernel
mkdir -p /sys/firmware/devicetree/base /sys/kernel/boot_adsp
printf 'linux,dummy-virt\0qcom,anorak\0oculus,eureka\0' > /sys/firmware/devicetree/base/compatible
printf 'qcom,anorak-adsp-pas\0' > "$fixture/remoteproc/remoteproc0/device/of_node/compatible"
printf 'adsp.mdt\n' > "$fixture/remoteproc/remoteproc0/firmware"
printf 'offline\n' > "$fixture/remoteproc/remoteproc0/state"
: > /sys/kernel/boot_adsp/boot
mount --bind "$fixture/remoteproc" /sys/class/remoteproc
mount --bind "$fixture/rpmsg" /sys/bus/rpmsg/devices
mount --bind "$fixture/typec" /sys/class/typec
mount --bind "$fixture/usb_role" /sys/class/usb_role
if [[ $ARMADA_QUEST_CASE == missing-firmware ]]; then
    : > "$fixture/missing-firmware"
    mount --bind "$fixture/missing-firmware" /usr/lib/firmware/adsp.mdt
fi
if [[ $ARMADA_QUEST_CASE == missing-mapper ]]; then
    printf '#!/bin/bash\nsleep 30\n' > "$fixture/mapper-unavailable"
    chmod 755 "$fixture/mapper-unavailable"
    mount --bind "$fixture/mapper-unavailable" /usr/bin/pd-mapper
fi
if [[ $ARMADA_QUEST_CASE == repeated-request ]]; then
    mkdir /run/armada-quest-adsp-requested
fi
systemd-notify --ready
if [[ $ARMADA_QUEST_CASE == missing-firmware || $ARMADA_QUEST_CASE == repeated-request || $ARMADA_QUEST_CASE == missing-mapper ]]; then
    while [[ ! -s /sys/kernel/boot_adsp/boot ]]; do sleep 0.1; done
    echo ARMADA_QUEST_UNEXPECTED_BOOT_REQUEST
    exit 1
fi
while [[ ! -s /sys/kernel/boot_adsp/boot ]]; do sleep 0.1; done
[[ $(< /sys/kernel/boot_adsp/boot) == 1 ]]
echo ARMADA_QUEST_MODEL_BOOT_REQUEST
sleep 0.3
mkdir -p "$fixture/rpmsg/pmic" "$fixture/typec/port0/device/of_node" "$fixture/usb_role/dwc3/device/of_node"
printf 'PMIC_RTR_ADSP_APPS\n' > "$fixture/rpmsg/pmic/name"
ln -s /sys/bus/rpmsg/drivers/pmic_glink_rpmsg "$fixture/rpmsg/pmic/driver"
ln -s /sys/bus/platform/drivers/ucsi_glink "$fixture/typec/port0/device/driver"
printf 'qcom,ucsi-glink\0' > "$fixture/typec/port0/device/of_node/compatible"
printf 'snps,dwc3\0' > "$fixture/usb_role/dwc3/device/of_node/compatible"
printf 'running\n' > "$fixture/remoteproc/remoteproc0/state"
if [[ $ARMADA_QUEST_CASE == missing-host-role ]]; then
    printf 'none\n' > "$fixture/usb_role/dwc3/role"
    sleep 25
    exit 1
fi
printf 'host\n' > "$fixture/usb_role/dwc3/role"
until systemctl is-active --quiet armada-quest-boot.service; do sleep 0.1; done
echo ARMADA_QUEST_MODEL_READY_PASS
/qmi-probe
echo ARMADA_QUEST_MODEL_QMI_PASS
if [[ $ARMADA_QUEST_CASE == root || $ARMADA_QUEST_CASE == root-fault ]]; then
    sleep 300
    exit 1
fi
systemctl --no-block poweroff
sleep 15
exit 1
