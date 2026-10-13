#!/usr/bin/env bash
set -euo pipefail
runtime=/run/reframed-vr
install -d -m700 -o vr -g vr "$runtime"
auth="$runtime/Xauthority"
xauth -f "$auth" add :0 . "$(mcookie)"
chown vr:vr "$auth"
Xorg :0 vt1 -noreset -s 0 -dpms -nolisten tcp -auth "$auth" > /var/log/reframed-vr-xorg.log 2>&1 &
xorg_pid=$!
trap 'kill "$xorg_pid" 2>/dev/null || true' EXIT
for ((i=0; i<200; i++)); do
    DISPLAY=:0 XAUTHORITY="$auth" xdpyinfo >/dev/null 2>&1 && break
    kill -0 "$xorg_pid" || { cat /var/log/reframed-vr-xorg.log; exit 1; }
    sleep 0.1
done
DISPLAY=:0 XAUTHORITY="$auth" xdpyinfo >/dev/null 2>&1 || { cat /var/log/reframed-vr-xorg.log; exit 1; }
echo REFRAMED_VR_DESKTOP_READY
runuser -u vr -- env HOME=/home/vr DISPLAY=:0 XAUTHORITY="$auth" XDG_RUNTIME_DIR="$runtime" \
    dbus-run-session -- /usr/local/libexec/reframed-vr/desktop-session.sh
systemctl --no-block poweroff
