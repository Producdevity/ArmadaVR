#!/usr/bin/env bash
set -euo pipefail
out="${1:-$HOME/.local/state/armada-vr/windows-acceptance}"
[[ ! -e "$out" ]] || { echo "Output exists: $out" >&2; exit 2; }
mkdir -p "$out"
out=$(cd "$out" && pwd)
export ARMADA_VR_PREFIX="$HOME/.local/share/armada-vr/compatdata-openxr"
# Initialize a cold prefix before the sample's short rendering/capture deadline.
if ! WINEDEBUG=-all timeout 90s xvfb-run -a --server-args=-noreset run-proton \
    /opt/armada-vr/windows/windows-smoke.exe "Z:$out/windows-cpu.txt" >"$out/prefix.log" 2>&1; then
    cat "$out/prefix.log" >&2
    exit 1
fi
grep -Fx 'ARMADA_VR_WINDOWS_X64_PASS sum=500500' "$out/windows-cpu.txt"
for binding in Vulkan Vulkan2; do
    if ! WINEDEBUG=+timestamp,+openxr render-xr "$out/$binding" run-proton-openxr --test-quit-after 12 \
        'Z:\opt\armada-vr\windows\hello_xr.exe' -g "$binding"; then
        tail -n 40 "$out/$binding/hello-xr.log" >&2
        exit 1
    fi
done
if ! WINEDEBUG=-all xvfb-run -a --server-args=-noreset test-input "$out/input" --windows; then
    cat "$out/input/probe.log" "$out/input/openxr.log" >&2
    exit 1
fi
echo ARMADA_VR_WINDOWS_INPUT_PASS
if ! WINEDEBUG=-all xvfb-run -a --server-args='-screen 0 1280x800x24 -noreset' \
    test-input "$out/rendered-input" --windows --render; then
    cat "$out/rendered-input/probe.log" "$out/rendered-input/openxr.log" >&2
    exit 1
fi
echo ARMADA_VR_WINDOWS_RENDERED_INPUT_PASS
echo ARMADA_VR_WINDOWS_OPENXR_PASS
