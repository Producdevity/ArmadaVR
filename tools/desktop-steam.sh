#!/usr/bin/env bash
set -euo pipefail
client="$HOME/.local/share/Steam"
mkdir -p "$HOME/.steam" "$HOME/.local/state/armada-vr"
ln -sfn "$client" "$HOME/.steam/root"
ln -sfn "$client" "$HOME/.steam/steam"
ln -sfn "$client/linux32" "$HOME/.steam/sdk32"
ln -sfn "$client/linux64" "$HOME/.steam/sdk64"
ln -sfn "$client/linuxarm64" "$HOME/.steam/sdkarm64"
ln -sfn "$client/ubuntu12_32" "$HOME/.steam/bin"
ln -sfn "$client/ubuntu12_32" "$HOME/.steam/bin32"
ln -sfn "$client/ubuntu12_64" "$HOME/.steam/bin64"
cd "$client"
export LD_LIBRARY_PATH="$client/steamrtarm64:$client/steamrtarm64/libs"
export PATH="$client/steam-runtime-steamrt-arm64/bin:$PATH"
# This ARM64 client cannot load the public SteamVR package's x86-64 vrclient.
if [[ -f "$client/steamapps/common/SteamVR/bin/linux64/vrclient.so" &&
      ! -f "$client/steamapps/common/SteamVR/bin/linuxarm64/vrclient.so" ]]; then
    set -- -vrdisable "$@"
fi
exec ./steamrtarm64/steam -nobootstrapupdate -skipinitialbootstrap -publicbeta \
    "$@" \
    >> "$HOME/.local/state/armada-vr/steam.log" 2>&1
