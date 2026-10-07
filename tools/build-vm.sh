#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
engine="${CONTAINER_ENGINE:-docker}"
image="${VR_VM_IMAGE:-localhost/armada-vr:vm}"
out="${1:-output/vm}"
[[ ! -e "$out" ]] || { echo "Output already exists: $out; choose a new directory" >&2; exit 1; }
mkdir -p "$out"
out=$(cd "$out" && pwd)
container=
cleanup() {
    if [[ -n "$container" ]]; then "$engine" rm "$container" >/dev/null; fi
}
trap cleanup EXIT
container=$("$engine" create "$image")
"$engine" cp "$container:/vm-boot/Image" "$out/Image"
"$engine" cp "$container:/vm-boot/initramfs.img" "$out/initramfs.img"
"$engine" export "$container" >"$out/rootfs.tar"
"$engine" run --rm --network none --memory 2g --memory-swap 2g --cpus 2 \
    --pids-limit 256 --entrypoint make-rootfs -v "$out:/output" "$image"
rm "$out/rootfs.tar"
"$engine" image inspect "$image" >"$out/image.json"
python3 - "$out" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
image = json.loads((root / "image.json").read_text())[0]
desktop = (image.get("Config", {}).get("Labels") or {}).get("org.armada-vr.desktop") == "true"
hashes = {}
for name in ("Image", "initramfs.img", "rootfs.ext4"):
    with (root / name).open("rb") as stream:
        hashes[name] = hashlib.file_digest(stream, "sha256").hexdigest()
(root / "manifest.json").write_text(json.dumps({"target": "qemu-arm64", "hardware_flash_image": False, "desktop_available": desktop, "sha256": hashes}, indent=2) + "\n")
PY
printf 'QEMU-only image written to %s\n' "$out"
