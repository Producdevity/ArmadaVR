#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
engine="${CONTAINER_ENGINE:-docker}"
profile="${1:-quest3}"
out="${2:-output/kernel/quest3/vendor-build}"
variant="${3:-vendor}"
mode="${4:-build}"
default_memory=6g
[[ "$mode" != configure ]] || default_memory=4g
memory="${KERNEL_BUILD_MEMORY:-$default_memory}"
case "$memory" in 4g|6g) ;; *) echo "KERNEL_BUILD_MEMORY must be 4g or 6g" >&2; exit 1 ;; esac
[[ "$profile" == quest3 ]] || { echo "Only the Quest 3 complete source is pinned" >&2; exit 1; }
args=("$profile")
case "$variant" in vendor) ;; linux-userspace) args+=(--linux-userspace) ;; qemu-abi) args+=(--qemu-abi) ;; *) echo "Unknown kernel variant" >&2; exit 1 ;; esac
case "$mode" in build) ;; configure) args+=(--configure-only) ;; *) echo "Unknown kernel mode" >&2; exit 1 ;; esac
[[ ! -e "$out" ]] || { echo "Output exists: $out; choose a new report directory" >&2; exit 1; }
image=$("$engine" image inspect --format '{{.Id}}' localhost/armada-vr:kernel)
python3 -B tools/kernel-source.py fetch-archive "$profile"
mkdir -p "$out"
out=$(cd "$out" && pwd)
"$engine" run --rm --name "armada-vr-$profile-$variant-build" --platform linux/arm64 \
    --network none --memory "$memory" --memory-swap "$memory" --cpus 2 --pids-limit 256 \
    --env "KERNEL_BUILD_IMAGE_ID=$image" \
    --env "KERNEL_BUILD_MEMORY=$memory" \
    --mount "type=volume,src=armada-vr-$profile-kernel,dst=/work" \
    --mount "type=bind,src=$PWD/tools,dst=/project/tools,readonly" \
    --mount "type=bind,src=$PWD/profiles,dst=/project/profiles,readonly" \
    --mount "type=bind,src=$PWD/patches,dst=/project/patches,readonly" \
    --mount "type=bind,src=$PWD/output/downloads/$profile-kernel.tar.gz,dst=/archive.tar.gz,readonly" \
    --mount "type=bind,src=$out,dst=/output" \
    "$image" -c 'exec python3 -B /project/tools/kernel-build.py "$@" --workspace /work --output /output --archive /archive.tar.gz' _ "${args[@]}"
