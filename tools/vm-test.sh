#!/usr/bin/env bash
set -uo pipefail
mkdir -p /var/log/armada-vr
result=0
uname -a
if ! test-xr /var/log/armada-vr/tracking; then
    tail -n 60 /var/log/armada-vr/tracking/*.log
    result=1
fi
if ! render-xr /var/log/armada-vr/render; then
    tail -n 60 /var/log/armada-vr/render/*.log
    result=1
fi
if ! test-fex; then result=1; fi
if [[ -x /usr/local/bin/test-input ]]; then
    if ! test-input /var/log/armada-vr/input; then
        tail -n 40 /var/log/armada-vr/input/*.log
        result=1
    fi
    if ! xvfb-run -a --server-args='-screen 0 1280x800x24 -noreset' \
        test-input /var/log/armada-vr/rendered-input --render; then
        tail -n 40 /var/log/armada-vr/rendered-input/*.log
        result=1
    fi
fi
if [[ -x /usr/local/bin/test-windows-xr ]]; then
    if ! runuser -u vr -- env HOME=/home/vr test-windows-xr; then result=1; fi
fi
if [[ "$result" == 0 ]]; then
    echo ARMADA_VR_VM_PASS
else
    echo ARMADA_VR_VM_FAIL
fi
systemctl --no-block poweroff
exit "$result"
