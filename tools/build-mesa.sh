#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
engine="${CONTAINER_ENGINE:-docker}"
out="${1:-output/mesa/build}"
driver="${2:-lavapipe}"
case "$driver" in lavapipe|turnip) ;; *) echo "Unknown Mesa driver: $driver" >&2; exit 1 ;; esac
[[ ! -e "$out" ]] || { echo "Output exists: $out; choose a new report directory" >&2; exit 1; }
image=$("$engine" image inspect --format '{{.Id}}' localhost/armada-vr:mesa-tools)
python3 -B tools/mesa-build.py fetch
mkdir -p "$out"
out=$(cd "$out" && pwd)
"$engine" run --rm --name armada-vr-mesa-builder --platform linux/arm64 \
    --network none --memory 4g --memory-swap 4g --cpus 2 --pids-limit 256 \
    --env "MESA_BUILD_IMAGE_ID=$image" --env PYTHONDONTWRITEBYTECODE=1 \
    --mount "type=volume,src=armada-vr-mesa,dst=/work" \
    --mount "type=bind,src=$PWD/tools,dst=/project/tools,readonly" \
    --mount "type=bind,src=$PWD/profiles/mesa.json,dst=/project/profiles/mesa.json,readonly" \
    --mount "type=bind,src=$PWD/patches/mesa,dst=/project/patches/mesa,readonly" \
    --mount "type=bind,src=$PWD/tests,dst=/project/tests,readonly" \
    --mount "type=bind,src=$PWD/src/vulkan-interop.c,dst=/project/src/vulkan-interop.c,readonly" \
    --mount "type=bind,src=$PWD/output/downloads/mesa-26.1.8.tar.xz,dst=/archive.tar.xz,readonly" \
    --mount "type=bind,src=$out,dst=/output" \
    "$image" -c 'exec python3 -B /project/tools/mesa-build.py build --driver "$1"' _ "$driver"
