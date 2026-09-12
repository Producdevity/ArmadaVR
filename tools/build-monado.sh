#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
engine="${CONTAINER_ENGINE:-docker}"
out="${1:?Choose a new output directory}"
[[ ! -e "$out" && ! -L "$out" ]] || { echo "Output exists: $out" >&2; exit 1; }
image=$("$engine" image inspect --format '{{.Id}}' localhost/armada-vr:monado-tools)
python3 -B tools/monado-build.py fetch
archive=$(python3 -B -c 'import json; p=json.load(open("profiles/monado.json")); print("output/downloads/monado-"+p["commit"]+".tar.gz")')
mkdir -p "$out"
out=$(cd "$out" && pwd)
"$engine" run --rm --name armada-vr-monado-builder --platform linux/arm64 \
    --network none --read-only --memory 4g --memory-swap 4g --cpus 2 --pids-limit 256 \
    --tmpfs /work:rw,exec,size=1024m --tmpfs /tmp:rw,exec,size=64m \
    --env "MONADO_BUILD_IMAGE_ID=$image" --env PYTHONDONTWRITEBYTECODE=1 \
    --mount "type=bind,src=$PWD/tools,dst=/project/tools,readonly" \
    --mount "type=bind,src=$PWD/profiles/monado.json,dst=/project/profiles/monado.json,readonly" \
    --mount "type=bind,src=$PWD/patches/monado,dst=/project/patches/monado,readonly" \
    --mount "type=bind,src=$PWD/tests,dst=/project/tests,readonly" \
    --mount "type=bind,src=$PWD/$archive,dst=/archive.tar.gz,readonly" \
    --mount "type=bind,src=$out,dst=/output" \
    "$image" -c 'exec python3 -B /project/tools/monado-build.py build'
