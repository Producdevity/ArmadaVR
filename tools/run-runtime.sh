#!/usr/bin/env bash
set -euo pipefail
runtime=/opt/armada-vr/runtime/SteamLinuxRuntime_4-arm64
export PRESSURE_VESSEL_VARIABLE_DIR="$HOME/.cache/armada-vr/steamrt4"
export PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1
export XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json
export LP_NUM_THREADS=2
[[ "$#" -gt 0 ]] || { echo 'Usage: run-runtime COMMAND [ARGS...]' >&2; exit 2; }
[[ "$PWD" != / ]] || cd "$HOME"
# The runtime remaps OpenXR's manifest and otherwise replaces stdin with /dev/null.
exec "$runtime/run" --pass-fd=9 -- sh -c \
    'exec env -u XR_RUNTIME_JSON "$@" <&9 9<&-' sh "$@" 9<&0
