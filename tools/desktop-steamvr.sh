#!/usr/bin/env bash
set -euo pipefail
bundle=/opt/armada-vr/openvr
if [[ ! -f "$bundle/steamvr-session.py" || ! -f "$bundle/xpresent/libXpresent.so.1" ]]; then
    echo 'The virtual SteamVR bundle is not installed. See docs/steamvr.md.' >&2
    exit 1
fi
exec python3 -B "$bundle/steamvr-session.py" --virtual-display window "$@"
