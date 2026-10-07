#!/usr/bin/env bash
set -euo pipefail
client="${1:-/client}"
out="${2:-/output}"
[[ -f "$client/armada-client-manifest.json" ]] || { echo 'Expected the fetched lab client' >&2; exit 1; }
mkdir -p "$out"
rm -f "$out/steam-12s.png" "$out/steam-30s.png" "$out/result.txt"
export HOME XDG_RUNTIME_DIR
XDG_RUNTIME_DIR=$(mktemp -d)
HOME="$XDG_RUNTIME_DIR/home"
mkdir -p "$HOME/.steam"
chmod 700 "$XDG_RUNTIME_DIR"
ln -s "$client" "$HOME/.steam/root"
ln -s "$client" "$HOME/.steam/steam"
pids=()
cleanup() {
    for name in memory.peak memory.events pids.peak; do
        [[ ! -r "/sys/fs/cgroup/$name" ]] || cp --remove-destination "/sys/fs/cgroup/$name" "$out/$name"
    done
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    rm -rf "$XDG_RUNTIME_DIR"
}
trap cleanup EXIT
export DISPLAY=:98 LANG=en_US.UTF-8 LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=2
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json
Xvfb "$DISPLAY" -screen 0 1280x800x24 -nolisten tcp >"$out/xvfb.log" 2>&1 &
pids+=("$!")
for ((i=0; i<100; i++)); do
    xdpyinfo >/dev/null 2>&1 && break
    sleep 0.1
done
openbox --sm-disable >"$out/window-manager.log" 2>&1 &
pids+=("$!")
# Timed container sessions can leave CEF locks pointing at an exited process.
rm -f "$client/config/htmlcache/SingletonCookie" "$client/config/htmlcache/SingletonLock" "$client/config/htmlcache/SingletonSocket"
cd "$client"
LD_LIBRARY_PATH="$client/steamrtarm64:$client/steamrtarm64/libs" \
    dbus-run-session -- timeout --kill-after=5s 45s ./steamrtarm64/steam \
    -nobootstrapupdate -skipinitialbootstrap -publicbeta -no-cef-sandbox >"$out/client.log" 2>&1 &
pids+=("$!")
client_pid=$!
sleep 12
import -window root "$out/steam-12s.png"
sleep 18
import -window root "$out/steam-30s.png"
set +e
wait "$client_pid"
result=$?
set -e
printf 'Native client exit status: %s (124 means the bounded session expired)\n' "$result" | tee "$out/result.txt"
printf 'Inspect the logs and captures; process survival alone does not verify the client UI.\n'
[[ "$result" == 0 || "$result" == 124 ]]
