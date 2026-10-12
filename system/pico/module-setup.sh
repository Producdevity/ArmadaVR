#!/bin/bash

check() {
    require_binaries qrtr-ns qrtr-lookup systemd-notify || return 1
    return 255
}

depends() {
    echo 'systemd systemd-initrd'
}

install() {
    inst_multiple qrtr-ns qrtr-lookup systemd-notify sleep
    inst_script "$moddir/armada-pico-qrtr" /usr/libexec/armada-pico-qrtr
    inst_simple "$moddir/armada-pico-qrtr.service" "$systemdsystemunitdir/armada-pico-qrtr.service"
    $SYSTEMCTL -q --root "$initdir" add-wants initrd.target armada-pico-qrtr.service
}
