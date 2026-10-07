#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
archive="${1:-output/downloads/openxr-sdk-b5fd54b.tar.gz}"
out="${2:-output/steamvr-openxr}"
rootfs="${FEX_ROOTFS:-/usr/share/guestos/fex-mesa}"
[[ "$(uname -s)" == Linux ]] || { echo 'Build in the ARM64 Linux VM' >&2; exit 1; }
[[ ! -e "$out" ]] || { echo "Output exists: $out" >&2; exit 1; }
[[ -d "$rootfs/usr/lib/gcc/x86_64-pc-linux-gnu/16" ]] || { echo 'Missing pinned FEX rootfs toolchain' >&2; exit 1; }
python3 - "$archive" <<'PY'
import hashlib, sys
with open(sys.argv[1], 'rb') as stream:
    if hashlib.file_digest(stream, 'sha256').hexdigest() != '3a6f217eda99c5535ad2626e34a67147ba63b8e6fe398183b484d7d43846642a':
        raise SystemExit('OpenXR SDK source checksum mismatch')
PY
python3 -B tools/fetch-openvr.py output/openvr-sdk
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/sdk"
tar -xf "$archive" -C "$work/sdk" --strip-components=1
cmake -S "$work/sdk" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64 -DCMAKE_SYSROOT="$rootfs" \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_COMPILER_TARGET=x86_64-pc-linux-gnu -DCMAKE_CXX_COMPILER_TARGET=x86_64-pc-linux-gnu \
    -DCMAKE_C_FLAGS="--gcc-install-dir=$rootfs/usr/lib/gcc/x86_64-pc-linux-gnu/16" \
    -DCMAKE_CXX_FLAGS="--gcc-install-dir=$rootfs/usr/lib/gcc/x86_64-pc-linux-gnu/16" \
    -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
    -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF -DBUILD_CONFORMANCE_TESTS=OFF -DBUILD_WITH_SYSTEM_JSONCPP=OFF
cmake --build "$work/build" --target openxr_loader --parallel 1
cp -r /usr/include/vulkan /usr/include/vk_video "$work/"
mkdir -p "$out/lib"
clang++ --target=x86_64-pc-linux-gnu --sysroot="$rootfs" \
    --gcc-install-dir="$rootfs/usr/lib/gcc/x86_64-pc-linux-gnu/16" -fuse-ld=lld \
    -std=c++17 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers \
    -I"$work" -I"$work/sdk/include" -I"$work/build/include" -Ioutput/openvr-sdk -Isrc \
    tests/steamvr-openxr.cpp src/xr-renderer.cpp -L"$work/build/src/loader" -Wl,-rpath,'$ORIGIN/lib' \
    -lopenxr_loader -lvulkan -ldl -o "$out/steamvr-openxr"
cp -L "$work/build/src/loader/libopenxr_loader.so.1" "$out/lib/"
cp "$work/sdk/LICENSE" "$out/OpenXR-LICENSE"
cp -r "$work/sdk/LICENSES" "$out/"
cp output/openvr-sdk/LICENSE "$out/OpenVR-LICENSE"
python3 - "$out" <<'PY'
import hashlib, json, pathlib, sys
out = pathlib.Path(sys.argv[1])
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
report = {'target':'steamvr-openxr-x86_64',
          'openxr_source_sha256':'3a6f217eda99c5535ad2626e34a67147ba63b8e6fe398183b484d7d43846642a',
          'source_sha256':{name:digest(pathlib.Path(name)) for name in
                          ('tests/steamvr-openxr.cpp','src/xr-renderer.cpp','src/xr-renderer.h')},
          'sha256':{p.relative_to(out).as_posix():digest(p) for p in sorted(out.rglob('*')) if p.is_file()}}
(out/'build.json').write_text(json.dumps(report,indent=2)+'\n')
PY
