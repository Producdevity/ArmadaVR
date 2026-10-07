#!/usr/bin/env bash
set -euo pipefail
proton=/opt/armada-vr/proton/proton-cachyos-11.0-20260703-slr-arm64/proton
[[ "$#" -gt 0 ]] || { echo 'Usage: run-proton PROGRAM [ARGS...]' >&2; exit 2; }
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam"
export STEAM_COMPAT_DATA_PATH="${ARMADA_VR_PREFIX:-$HOME/.local/share/armada-vr/compatdata}"
export PROTON_LOG=1 PROTON_LOG_DIR="$HOME/.local/state/armada-vr"
export SteamAppId=0 SteamGameId=0
mkdir -p "$STEAM_COMPAT_DATA_PATH" "$PROTON_LOG_DIR"
exec /usr/local/bin/run-runtime "$proton" run "$@"
