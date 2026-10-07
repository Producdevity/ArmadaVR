#!/usr/bin/env bash
set -euo pipefail
out="${1:-output/lab}"
mkdir -p "$out"
export XDG_RUNTIME_DIR
XDG_RUNTIME_DIR=$(mktemp -d)
chmod 700 "$XDG_RUNTIME_DIR"
service_pid=
cleanup() {
    if [[ -n "$service_pid" ]]; then
        kill "$service_pid" 2>/dev/null || true
        wait "$service_pid" 2>/dev/null || true
    fi
    rm -rf "$XDG_RUNTIME_DIR"
}
trap cleanup EXIT
export SIMULATED_ENABLE=1 SIMULATED_LEFT=simple SIMULATED_RIGHT=simple
export XRT_COMPOSITOR_NULL=1 XRT_NO_STDIN=1
export XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json
monado-service >"$out/monado.log" 2>&1 &
service_pid=$!
for ((i=0; i<100; i++)); do
    [[ -S "$XDG_RUNTIME_DIR/monado_comp_ipc" ]] && break
    if ! kill -0 "$service_pid" 2>/dev/null; then
        cat "$out/monado.log" >&2
        exit 1
    fi
    sleep 0.1
done
timeout 30s /usr/local/bin/xr-probe | tee "$out/openxr.log"
