#!/usr/bin/env bash
set -euo pipefail
logs="$HOME/.local/state/armada-vr"
mkdir -p "$logs"
export QWERTY_ENABLE=1 XRT_DEBUG_GUI=1 XRT_NO_STDIN=1
export XRT_COMPOSITOR_FORCE_XCB=1 XRT_COMPOSITOR_SCALE_PERCENTAGE=40
export XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json
export LP_NUM_THREADS=2
unset SIMULATED_ENABLE XRT_COMPOSITOR_NULL
exec 9> "$XDG_RUNTIME_DIR/armada-vr-scene.lock"
if ! flock -n 9 || pgrep -u "$UID" -x monado-service >/dev/null; then
    echo 'A Monado session is already running.'
    exit 1
fi
# Monado can leave its IPC socket after its process exits.
rm -f "$XDG_RUNTIME_DIR/monado_comp_ipc"
monado-service > "$logs/monado.log" 2>&1 &
service_pid=$!
cleanup() {
    kill "$service_pid" 2>/dev/null || true
    wait "$service_pid" 2>/dev/null || true
}
trap cleanup EXIT
for ((i=0; i<100; i++)); do
    [[ -S "$XDG_RUNTIME_DIR/monado_comp_ipc" ]] && break
    kill -0 "$service_pid" || { cat "$logs/monado.log"; exit 1; }
    sleep 0.1
done
[[ -S "$XDG_RUNTIME_DIR/monado_comp_ipc" ]] || { cat "$logs/monado.log"; exit 1; }
printf 'Use the Monado debug window: Qwerty System > Help lists headset/controller controls.\nPress Enter in this terminal to close the scene.\n'
hello_xr -g Vulkan2 > "$logs/hello-xr.log" 2>&1
