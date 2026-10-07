#!/usr/bin/env bash
set -euo pipefail
[[ "$#" -gt 0 ]] || { echo 'Usage: run-proton-openxr [--test-quit-after SECONDS] WINDOWS_PROGRAM [ARGS...]' >&2; exit 2; }
# Proton's shared VR registry assumes OpenVR; this prefix initializes only Monado.
export ARMADA_VR_PREFIX="$HOME/.local/share/armada-vr/compatdata-openxr"
export ARMADA_VR_OPENXR_ONLY=1
compat=/usr/local/libexec/armada-vr/libopenxr-procaddr.so
[[ -f "$compat" ]] || { echo "Missing native OpenXR compatibility library: $compat" >&2; exit 1; }
export LD_PRELOAD="$compat${LD_PRELOAD:+:$LD_PRELOAD}"
exec /usr/local/bin/run-proton /opt/armada-vr/windows/openxr-launcher.exe "$@"
