#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
engine="${CONTAINER_ENGINE:-docker}"
archives="${1:-output/downloads/runtime}"
python3 -B tools/fetch-runtime.py "$archives" \
    --component SteamLinuxRuntime_4-arm64.tar.xz \
    --component proton-cachyos-11.0-20260703-slr-arm64.tar.xz
"$engine" build --platform linux/arm64 -f Containerfile.windows -t localhost/armada-vr:windows .
"$engine" build --platform linux/arm64 --build-context "runtime-archives=$archives" \
    -f Containerfile.runtime -t localhost/armada-vr:runtime .
