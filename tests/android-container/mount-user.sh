#!/bin/sh
set -eu
test "$(id -u)" = 1000
mkdir -p "$HOME/rootfs/bin" "$HOME/rootfs/lib" "$HOME/rootfs/data/steam_app" \
    "$HOME/rootfs/proc" "$HOME/rootfs/sys" "$HOME/rootfs/dev" "$HOME/rootfs/etc" \
    "$HOME/rootfs/run" "$HOME/rootfs/tmp" "$HOME/data-lower/steam_app" "$HOME/apk-lower"
cp /bin/busybox "$HOME/rootfs/bin/"
cp /lib/ld-musl-aarch64.so.1 "$HOME/rootfs/lib/"
ln -s busybox "$HOME/rootfs/bin/sh"
ln -s ld-musl-aarch64.so.1 "$HOME/rootfs/lib/libc.musl-aarch64.so.1"
printf base > "$HOME/data-lower/value"
printf delete > "$HOME/data-lower/removed"
printf apk > "$HOME/apk-lower/base.apk"
printf root-base > "$HOME/rootfs/base"
for context in a b; do
    mkdir -p "$HOME/$context/data-upper" "$HOME/$context/data-work" \
        "$HOME/$context/apk-upper" "$HOME/$context/apk-work"
done
pman() {
    podman --cgroup-manager=cgroupfs --events-backend=file --runtime=/usr/bin/crun \
        --storage-driver=overlay --storage-opt=overlay.mount_program=/usr/bin/fuse-overlayfs "$@"
}
run() {
    context="$1"
    shift
    detach_flag=
    if [ "$1" = --detach ]; then detach_flag=--detach; shift; fi
    pman run $detach_flag --rm --name "lepton-mount-$context" --network=none --cgroups=disabled \
        --userns=keep-id:uid=0,gid=0 --user=0:0 --group-add=keep-groups \
        --read-only --init=false --cap-drop=all \
        -v "$HOME/data-lower:/data:O,upperdir=$HOME/$context/data-upper,workdir=$HOME/$context/data-work" \
        -v "$HOME/apk-lower:/data/steam_app:O,upperdir=$HOME/$context/apk-upper,workdir=$HOME/$context/apk-work" \
        --rootfs "$HOME/rootfs:O" "$@"
}
test "$(pman info --format '{{.Host.Security.Rootless}}')" = true
echo LEPTON_MOUNT_ROOTLESS_PASS host_uid=1000
run a /bin/busybox sh -ec '
    test "$(/bin/busybox id -u)" = 0
    test "$$" = 1
    test "$(/bin/busybox grep -c "fuse.fuse-overlayfs" /proc/self/mountinfo)" = 3
    test "$(/bin/busybox cat /data/value)" = base
    printf changed > /data/value
    /bin/busybox rm /data/removed
    printf installed > /data/steam_app/base.apk
    if (printf fail > /base) 2>/dev/null; then exit 1; fi
    echo LEPTON_MOUNT_FIRST_PASS
'
run b /bin/busybox sh -ec '
    test "$(/bin/busybox cat /data/value)" = base
    test -e /data/removed
    test "$(/bin/busybox cat /data/steam_app/base.apk)" = apk
    echo LEPTON_MOUNT_CONTEXT_ISOLATION_PASS
'
run a /bin/busybox sh -ec '
    test "$(/bin/busybox cat /data/value)" = changed
    test ! -e /data/removed
    test "$(/bin/busybox cat /data/steam_app/base.apk)" = installed
    test "$(/bin/busybox cat /base)" = root-base
    echo LEPTON_MOUNT_RESTART_PASS
'
run a --detach /bin/busybox sleep 300 > "$HOME/container-id"
test "$(pman inspect --format '{{.State.Running}}' "$(cat "$HOME/container-id")")" = true
pman stop --time=1 "$(cat "$HOME/container-id")" > /dev/null
test -z "$(pman ps -a -q)"
pman unshare /bin/sh -ec '! grep -q "fuse.fuse-overlayfs" /proc/self/mountinfo'
test "$(cat "$HOME/data-lower/value")" = base
test "$(cat "$HOME/data-lower/removed")" = delete
test "$(cat "$HOME/apk-lower/base.apk")" = apk
test "$(cat "$HOME/rootfs/base")" = root-base
echo LEPTON_MOUNT_LOWER_UNCHANGED_PASS
pman system migrate
echo LEPTON_MOUNT_LIFECYCLE_PASS
