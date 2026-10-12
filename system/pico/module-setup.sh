#!/bin/bash

check() {
    require_binaries qrtr-ns qrtr-lookup systemd-notify || return 1
    if [[ -f /tmp/pico-service-maps.sha256 ]]; then
        require_binaries pd-mapper sha256sum || return 1
    fi
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
    if [[ -f /tmp/pico-service-maps.sha256 ]]; then
        inst_multiple pd-mapper sha256sum
        inst_simple /tmp/pico-service-maps.sha256 /etc/armada-vr/pico-service-maps.sha256
        inst_script "$moddir/armada-pico-pd-mapper" /usr/libexec/armada-pico-pd-mapper
        inst_simple "$moddir/armada-pico-pd-mapper.service" "$systemdsystemunitdir/armada-pico-pd-mapper.service"
        $SYSTEMCTL -q --root "$initdir" add-wants initrd.target armada-pico-pd-mapper.service
    fi
}
