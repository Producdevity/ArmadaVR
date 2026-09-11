#!/bin/bash

check() {
    require_binaries pd-mapper qrtr-lookup systemd-notify sha256sum || return 1
    return 255
}

depends() {
    echo 'systemd systemd-initrd systemd-udevd systemd-modules-load'
}

install() {
    inst_multiple pd-mapper qrtr-lookup systemd-notify sha256sum modprobe sleep mkdir basename readlink
    inst_script "$moddir/armada-quest-boot" /usr/libexec/armada-quest-boot
    inst_simple "$moddir/armada-quest-boot.service" "$systemdsystemunitdir/armada-quest-boot.service"
    inst_simple /tmp/quest-firmware.sha256 /etc/armada-vr/quest-firmware.sha256
    $SYSTEMCTL -q --root "$initdir" add-wants initrd.target armada-quest-boot.service
}
