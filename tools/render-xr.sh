#!/usr/bin/env bash
set -euo pipefail
out="${1:-output/render}"
if [[ "$#" -gt 0 ]]; then shift; fi
if [[ "$#" == 0 ]]; then set -- hello_xr -g Vulkan2; fi
mkdir -p "$out"
export XDG_RUNTIME_DIR
XDG_RUNTIME_DIR=$(mktemp -d)
chmod 700 "$XDG_RUNTIME_DIR"
pids=()
cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    rm -rf "$XDG_RUNTIME_DIR"
}
trap cleanup EXIT
export DISPLAY=:99 XRT_NO_STDIN=1
export SIMULATED_ENABLE=1 SIMULATED_LEFT=simple SIMULATED_RIGHT=simple
export XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.aarch64.json}"
export XRT_COMPOSITOR_FORCE_XCB=1 XRT_COMPOSITOR_SCALE_PERCENTAGE=40
export LP_NUM_THREADS=2
unset XRT_COMPOSITOR_NULL
Xvfb "$DISPLAY" -screen 0 1280x800x24 -nolisten tcp -noreset >"$out/xvfb.log" 2>&1 &
pids+=("$!")
for ((i=0; i<100; i++)); do
    xdpyinfo >/dev/null 2>&1 && break
    sleep 0.1
done
vulkaninfo --summary >"$out/vulkan.log" 2>&1
monado-service >"$out/monado.log" 2>&1 &
pids+=("$!")
for ((i=0; i<100; i++)); do
    [[ -S "$XDG_RUNTIME_DIR/monado_comp_ipc" ]] && break
    kill -0 "${pids[1]}" || { cat "$out/monado.log" >&2; exit 1; }
    sleep 0.1
done
mkfifo "$XDG_RUNTIME_DIR/hello-input"
exec 3<>"$XDG_RUNTIME_DIR/hello-input"
timeout 30s "$@" <&3 >"$out/hello-xr.log" 2>&1 &
pids+=("$!")
sleep 6
kill -0 "${pids[2]}" || { cat "$out/hello-xr.log" >&2; exit 1; }
import -window root "$out/stereo-1.png"
sleep 2
kill -0 "${pids[2]}"
import -window root "$out/stereo-2.png"
printf '\n' >&3
wait "${pids[2]}"
exec 3>&-
python3 /usr/local/libexec/armada-vr/check-render.py "$out/stereo-1.png" "$out/stereo-2.png" | tee "$out/result.txt"
