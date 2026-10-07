#!/usr/bin/env bash
set -euo pipefail
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/root"
tar -xpf /output/rootfs.tar -C "$work/root"
rm -f "$work/root/.dockerenv" "$work/root/run/.containerenv"
: > "$work/root/etc/machine-id"
printf '%s\n' armada-vr-vm > "$work/root/etc/hostname"
# Populate a regular filesystem image without mounting host block devices.
[[ ! -e /output/rootfs.ext4 ]] || { echo 'rootfs.ext4 already exists' >&2; exit 1; }
truncate -s "${VR_VM_SIZE:-5G}" /output/rootfs.ext4
mkfs.ext4 -q -F -L armada-vr-vm -d "$work/root" /output/rootfs.ext4
