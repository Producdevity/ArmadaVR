#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
engine="${CONTAINER_ENGINE:-docker}"
out="${1:-output/syncboss}"
[[ ! -e "$out" ]] || { echo "Output exists: $out; choose a new directory" >&2; exit 1; }
mkdir -p "$out"
out=$(cd "$out" && pwd)
"$engine" run --rm --network none --memory 256m --memory-swap 256m --cpus 1 --pids-limit 64 \
    --mount "type=volume,src=armada-vr-quest3-kernel,dst=/work,readonly" \
    --mount "type=bind,src=$PWD/tools,dst=/project/tools,readonly" \
    --mount "type=bind,src=$PWD/profiles,dst=/project/profiles,readonly" \
    --mount "type=bind,src=$PWD/tests/syncboss_uapi_fixture.c,dst=/fixture.c,readonly" \
    --mount "type=bind,src=$PWD/output/downloads/quest3-kernel.tar.gz,dst=/archive.tar.gz,readonly" \
    --mount "type=bind,src=$out,dst=/output" \
    localhost/armada-vr:kernel -c '
        set -e
        python3 -B /project/tools/kernel-source.py verify-full quest3 --source /work/source \
            --archive /archive.tar.gz > /output/source-verify.log
        clang -std=c11 -Wall -Wextra -Werror \
            -I/work/source/drivers/staging/oculus/include/uapi/linux \
            /fixture.c -o /tmp/syncboss-fixture
        /tmp/syncboss-fixture > /output/uapi.bin
        python3 -B /project/tools/syncboss-replay.py /output/uapi.bin \
            --records /output/records.jsonl > /output/summary.json
    '
python3 -B - "$out" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
records = [json.loads(line) for line in (root / "records.jsonl").read_text().splitlines()]
assert len(records) == 3
assert records[0]["nsync"]["offset_us"] == -12345 and records[0]["payload_hex"] == "deadbeef"
assert records[1]["sequence"] == 7 and records[1]["nsync"]["offset_us"] == 54321
assert records[1]["remote"] == {"status": "error", "offset_us": None}
assert records[2]["from_driver"] and records[2]["driver_message_type"] == 2 and records[2]["driver_message_data"] == 1
assert not json.loads((root / "summary.json").read_text())["tracking_verified"]
print("Syncboss replay passed against the pinned vendor C UAPI; synthetic records, no tracking proof")
PY
