#!/usr/bin/env bash
set -euo pipefail
[[ "$(uname -m)" == aarch64 ]] || { echo 'FEX requires an ARM64 Linux host' >&2; exit 1; }
export FEX_ROOTFS=/
result=$(mktemp)
trap 'rm -f "$result"' EXIT
timeout 20s FEX /usr/local/libexec/armada-vr/fex-smoke >"$result"
cat "$result"
grep -Fxq ARMADA_VR_FEX_PASS "$result"
