#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
engine="${CONTAINER_ENGINE:-docker}"
archive="${1:?Usage: build-fastrpc.sh source-archive new-output-directory}"
out="${2:?Choose a new output directory}"
[[ -f "$archive" ]] || { echo "Source archive missing: $archive" >&2; exit 1; }
[[ ! -e "$out" && ! -L "$out" ]] || { echo "Output exists: $out" >&2; exit 1; }
archive=$(python3 - "$archive" <<'PY'
import hashlib, json, pathlib, sys
p = pathlib.Path(sys.argv[1]).resolve()
profile = json.load(open('profiles/pico-fastrpc.json'))
with p.open('rb') as stream:
    if hashlib.file_digest(stream, 'sha256').hexdigest() != profile['source_archive_sha256']:
        raise ValueError('Source archive differs from the pinned FastRPC revision')
print(p)
PY
)
image=$("$engine" image inspect --format '{{.Id}}' "${FASTRPC_BUILD_IMAGE_ID:-localhost/reframed-vr:fastrpc-tools}")
mkdir -p "$out"
out=$(cd "$out" && pwd)
"$engine" run --rm --name reframed-vr-fastrpc-builder --platform linux/arm64 \
    --network none --read-only --memory 1g --memory-swap 1g --cpus 2 --pids-limit 256 \
    --tmpfs /tmp:rw,exec,nosuid,size=512m --entrypoint python3 \
    --mount "type=bind,src=$PWD/tools,dst=/project/tools,readonly" \
    --mount "type=bind,src=$PWD/src,dst=/project/src,readonly" \
    --mount "type=bind,src=$PWD/profiles,dst=/project/profiles,readonly" \
    --mount "type=bind,src=$PWD/patches/fastrpc,dst=/project/patches/fastrpc,readonly" \
    --mount "type=bind,src=$PWD/tests,dst=/project/tests,readonly" \
    --mount "type=bind,src=$PWD/Containerfile.fastrpc,dst=/project/Containerfile.fastrpc,readonly" \
    --mount "type=bind,src=$archive,dst=/archive.tar.gz,readonly" \
    --mount "type=bind,src=$out,dst=/output" \
    "$image" -B /project/tools/fastrpc-build.py /archive.tar.gz /output/bundle --image "$image"
