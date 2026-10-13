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
    inst_script "$moddir/reframed-quest-boot" /usr/libexec/reframed-quest-boot
    inst_simple "$moddir/reframed-quest-boot.service" "$systemdsystemunitdir/reframed-quest-boot.service"
    inst_simple /tmp/quest-firmware.sha256 /etc/reframed-vr/quest-firmware.sha256
    $SYSTEMCTL -q --root "$initdir" add-wants initrd.target reframed-quest-boot.service
}
