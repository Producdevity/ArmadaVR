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
    inst_script "$moddir/reframed-pico-qrtr" /usr/libexec/reframed-pico-qrtr
    inst_simple "$moddir/reframed-pico-qrtr.service" "$systemdsystemunitdir/reframed-pico-qrtr.service"
    $SYSTEMCTL -q --root "$initdir" add-wants initrd.target reframed-pico-qrtr.service
    if [[ -f /tmp/pico-service-maps.sha256 ]]; then
        inst_multiple pd-mapper sha256sum
        inst_simple /tmp/pico-service-maps.sha256 /etc/reframed-vr/pico-service-maps.sha256
        inst_script "$moddir/reframed-pico-pd-mapper" /usr/libexec/reframed-pico-pd-mapper
        inst_simple "$moddir/reframed-pico-pd-mapper.service" "$systemdsystemunitdir/reframed-pico-pd-mapper.service"
        $SYSTEMCTL -q --root "$initdir" add-wants initrd.target reframed-pico-pd-mapper.service
    fi
}
