#!/bin/sh
set -eu
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
mount -t tmpfs -o size=256m tmpfs /home
mount -t tmpfs -o size=32m tmpfs /run
mkdir -p /home/vr /run/user/1000 /dev/shm /dev/pts /sys/fs/cgroup
chmod 700 /run/user/1000
chown 1000:1000 /home/vr /run/user/1000
chmod 666 /dev/fuse
mount -t tmpfs -o mode=1777,size=32m tmpfs /dev/shm
mount -t devpts -o newinstance,ptmxmode=0666 devpts /dev/pts
mount -t cgroup2 cgroup2 /sys/fs/cgroup
mount --make-rshared /
uname -r
podman --version
fuse-overlayfs --version
cat /packages.txt
su -s /bin/sh vr -c 'HOME=/home/vr XDG_RUNTIME_DIR=/run/user/1000 /bin/sh /mount-user.sh'
