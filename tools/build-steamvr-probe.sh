#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sdk="${1:-output/openvr-sdk}"
out="${2:-output/steamvr-probe}"
target="${3:-x86_64}"
rootfs="${FEX_ROOTFS:-/usr/share/guestos/fex-mesa}"
[[ "$(uname -s)" == Linux ]] || { echo 'Build this probe in ARM64 Linux' >&2; exit 1; }
[[ ! -e "$out" && ! -L "$out" ]] || { echo "Output exists: $out" >&2; exit 1; }
case "$target" in
    x86_64)
        [[ -d "$rootfs/usr/lib/gcc/x86_64-pc-linux-gnu/16" ]] || { echo 'Missing pinned FEX rootfs toolchain' >&2; exit 1; }
        architecture=linux64
        profile=steamvr-virtual.json
        compiler=(--target=x86_64-pc-linux-gnu --sysroot="$rootfs"
            --gcc-install-dir="$rootfs/usr/lib/gcc/x86_64-pc-linux-gnu/16" -fuse-ld=lld)
        ;;
    aarch64)
        [[ "$(uname -m)" == aarch64 ]] || { echo 'Native target requires an AArch64 Linux host' >&2; exit 1; }
        architecture=linuxarm64
        profile=steamvr-native-virtual.json
        compiler=()
        ;;
    *) echo 'Target must be x86_64 or aarch64' >&2; exit 1 ;;
esac
python3 -B tools/fetch-openvr.py "$sdk"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
compiler+=(-isystem "$sdk" -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers)
if [[ "$target" == x86_64 ]]; then
    python3 -B tools/fetch-xpresent.py "$work/xpresent"
    cp -r /usr/include/vulkan /usr/include/vk_video /usr/include/openxr "$work/"
    compiler+=(-I"$work")
fi
clang++ "${compiler[@]}" -std=c++17 src/steamvr-probe.cpp -lvulkan -ldl -o "$work/steamvr-probe"
clang++ "${compiler[@]}" -std=c++17 -shared -fPIC -fvisibility=hidden src/steamvr-controllers.cpp -pthread -o "$work/driver_armada_virtual.so"
if [[ "$target" == x86_64 ]]; then
    clang "${compiler[@]}" -std=gnu11 -shared -fPIC src/vulkan-procaddr.c -ldl -pthread -o "$work/libvulkan-procaddr.so"
    clang "${compiler[@]}" -std=gnu11 -shared -fPIC src/openxr-procaddr.c -ldl -pthread -o "$work/libopenxr-procaddr.so"
    clang "${compiler[@]}" -std=gnu11 -shared -fPIC -Wno-unused-parameter -Wl,-soname,libXpresent.so.1 \
        -I"$work/xpresent/include" "$work/xpresent/src/Xpresent.c" -lX11 -lXext -o "$work/libXpresent.so.1"
else
    clang "${compiler[@]}" -std=gnu11 src/vulkan-interop.c -lvulkan -o "$work/vulkan-interop"
    clang "${compiler[@]}" -std=gnu11 tests/vulkan-external-sync.c -lvulkan -o "$work/vulkan-external-sync"
fi
mkdir -p "$out"
install -m755 "$work/steamvr-probe" "$out/steamvr-probe"
mkdir -p "$out/armada_virtual/bin/$architecture"
install -m755 "$work/driver_armada_virtual.so" "$out/armada_virtual/bin/$architecture/driver_armada_virtual.so"
cp system/steamvr-controllers/driver.vrdrivermanifest "$out/armada_virtual/"
cp "$sdk/LICENSE" "$out/OpenVR-LICENSE"
cp "profiles/$profile" "$out/steamvr-virtual.json"
cp system/steamvr-probe/*.json "$out/"
cp tools/steamvr-session.py "$out/steamvr-session.py"
if [[ "$target" == x86_64 ]]; then
    install -m755 "$work/libvulkan-procaddr.so" "$out/libvulkan-procaddr.so"
    install -m755 "$work/libopenxr-procaddr.so" "$out/libopenxr-procaddr.so"
    mkdir -p "$out/xpresent"
    install -m755 "$work/libXpresent.so.1" "$out/xpresent/libXpresent.so.1"
    cp "$work/xpresent/COPYING" "$out/Xpresent-LICENSE"
    install -m755 tools/run-steamvr-windows.py "$out/run-steamvr-windows.py"
    cp profiles/steamvr-presentation.json profiles/steamvr-runtime.json "$out/"
else
    install -m755 "$work/vulkan-interop" "$work/vulkan-external-sync" "$out/"
fi
sha256sum src/steamvr-probe.cpp src/steamvr-controllers.cpp src/steamvr-headset.h "$sdk/openvr.h" "$sdk/openvr_driver.h" \
    "$out/steamvr-probe" "$out/armada_virtual/bin/$architecture/driver_armada_virtual.so" > "$out/sha256.txt"
clang++ --version > "$out/compiler.txt"
python3 - "$out" "$target" "$profile" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
out = Path(sys.argv[1])
files = {p.relative_to(out).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
         for p in sorted(out.rglob('*')) if p.is_file()}
sources = ['src/steamvr-probe.cpp', 'src/steamvr-controllers.cpp', 'src/steamvr-headset.h',
           'tools/build-steamvr-probe.sh', 'tools/fetch-openvr.py',
           'profiles/' + sys.argv[3], 'system/steamvr-controllers/driver.vrdrivermanifest',
           'tools/steamvr-session.py']
if sys.argv[2] == 'x86_64':
    sources += ['src/vulkan-procaddr.c', 'src/openxr-procaddr.c',
                'tools/run-steamvr-windows.py', 'tools/fetch-xpresent.py',
                'patches/xpresent/0001-steamvr-initial-timing.patch']
else:
    sources += ['src/vulkan-interop.c', 'tests/vulkan-external-sync.c']
(out / 'build.json').write_text(json.dumps({'target': 'steamvr-virtual-' + sys.argv[2], 'sha256': files,
    'sources_sha256': {name: hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in sources},
    'openvr': json.loads(Path('profiles/openvr.json').read_text())}, indent=2) + '\n')
PY
