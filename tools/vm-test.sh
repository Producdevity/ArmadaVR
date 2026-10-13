#!/usr/bin/env bash
set -uo pipefail
mkdir -p /var/log/reframed-vr
result=0
uname -a
if ! test-xr /var/log/reframed-vr/tracking; then
    tail -n 60 /var/log/reframed-vr/tracking/*.log
    result=1
fi
if ! render-xr /var/log/reframed-vr/render; then
    tail -n 60 /var/log/reframed-vr/render/*.log
    result=1
fi
if ! test-fex; then result=1; fi
if [[ -x /usr/local/bin/test-input ]]; then
    if ! test-input /var/log/reframed-vr/input; then
        tail -n 40 /var/log/reframed-vr/input/*.log
        result=1
    fi
    if ! xvfb-run -a --server-args='-screen 0 1280x800x24 -noreset' \
        test-input /var/log/reframed-vr/rendered-input --render; then
        tail -n 40 /var/log/reframed-vr/rendered-input/*.log
        result=1
    fi
fi
if [[ -x /usr/local/bin/test-windows-xr ]]; then
    if ! runuser -u vr -- env HOME=/home/vr test-windows-xr; then result=1; fi
fi
if [[ "$result" == 0 ]]; then
    echo REFRAMED_VR_VM_PASS
else
    echo REFRAMED_VR_VM_FAIL
fi
systemctl --no-block poweroff
exit "$result"
