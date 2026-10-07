#!/usr/bin/env bash
set -euo pipefail
export LANG=en_US.UTF-8 LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=2
mkdir -p "$HOME/.local/state/armada-vr"
xsetroot -solid '#18242b'
openbox > >(tee "$HOME/.local/state/armada-vr/window-manager.log") 2>&1 &
wm=$!
xterm -T 'Armada VR terminal' -geometry 90x12+15+15 -e bash -c '
    printf "Armada VR development VM\n\nRight-click the desktop for Steam, SteamVR, OpenXR sample, Terminal, or Power off VM.\nChanges persist in the desktop.qcow2 overlay.\n"
    if compgen -G "/sys/class/net/en*" >/dev/null; then
        printf "Networking is enabled. Steam sign-in requires a working connection.\n\n"
    else
        printf "Networking is disabled. Steam sign-in is unavailable.\n\n"
    fi
    exec bash' &
/usr/local/libexec/armada-vr/desktop-steam.sh &
wait "$wm"
