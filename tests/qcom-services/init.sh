#!/bin/bash
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs tmpfs /run
IFS= read -r -d '' compatible < /proc/device-tree/compatible
if [ "$compatible" != linux,dummy-virt ] || [ "$$" != 1 ]; then
    echo ARMADA_QMI_INIT_REFUSED
    exec /bin/sh
fi

run_cases() {
    modprobe qrtr || return 1
    test -d /sys/class/remoteproc || return 1
    mkdir -p /run/remoteproc/remoteproc0 || return 1
    printf 'adsp.mdt\n' > /run/remoteproc/remoteproc0/firmware
    mount --bind /run/remoteproc /sys/class/remoteproc || return 1
    for cycle in 1 2; do
        /usr/bin/pd-mapper > /run/pd-mapper.log 2>&1 &
        mapper=$!
        /qmi-probe
        result=$?
        kill "$mapper" || return 1
        wait "$mapper"
        cat /run/pd-mapper.log
        test "$result" = 0 || return 1
        echo ARMADA_QMI_CYCLE_PASS="$cycle"
    done
    printf 'missing/adsp.mdt\n' > /run/remoteproc/remoteproc0/firmware
    /usr/bin/pd-mapper > /run/no-maps.log 2>&1
    result=$?
    cat /run/no-maps.log
    test "$result" = 1 || return 1
    echo ARMADA_QMI_NO_MAPS_PASS
}

if run_cases; then
    echo ARMADA_QMI_INIT_PASS
else
    echo ARMADA_QMI_INIT_FAIL
fi
systemctl --force --force poweroff
exec /bin/sh
