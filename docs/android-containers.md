# Android container kernel checks

Lepton is Valve's Android compatibility container. Before integrating its guest
image, Armada VR tests the relevant Linux interfaces against its actual Quest
kernel with QEMU transports. The test needs Zig and QEMU, so it can run without
Docker or a connected headset.

```sh
python3 -B tools/test-android-container.py output/kernel/quest3/qemu-abi-v10 \
    --output output/android-container-next
```

Choose a new output directory each time. The runner verifies the kernel build
manifest, image, resolved configuration and module archive before compiling a
static ARM64 musl fixture. It attaches no disks, network interfaces, host shares
or USB devices. Guest writes remain in RAM. Compiler caches, source snapshots,
commands, hashes and the serial log stay in the chosen output directory. The
fixture must run as PID 1 on QEMU's `linux,dummy-virt` board with its explicit
test command-line marker; physical kernel build reports are refused.

The fixture drops to guest UID/GID 1000 before creating a user namespace that
maps the same user to container UID/GID 0. It then checks:

- Creation of mount, PID, IPC, UTS and network namespaces, a container PID 1,
  private procfs and tmpfs mounts.
- Two separate BinderFS mounts, each with three dynamically allocated Binder
  devices; protocol version 8 and independent context-manager registration.
- Rejection of a second manager for the same Binder context, and survival of
  the other instance when a device is removed.
- Shared memfd writes across a fork and enforcement of write/size seals.
- A small seccomp filter that denies one syscall and permits another.
- Rootless native OverlayFS mounting and, when supported, preservation of the
  lower file during copy-up.
- Removal of private mounts when the child exits, followed by guest power-off.

The seccomp filter is a test, not a policy for Android applications. The Binder
checks exercise real driver ioctls and context isolation; they do not exchange
Android service transactions. The memory check does not establish graphics
buffer or synchronization interoperability. Subordinate ID ranges, cgroup
delegation, networking, Podman, Android boot, APK execution and graphics remain
separate integration tests.

## Verified result

On September 14, 2026, `output/android-container-v4/result.json` records a pass
for the interface probes on the unchanged `qemu-abi-v10` kernel. It records
`native_rootless_overlayfs: false`: the actual mount returns `EPERM`. A passed
probe means the recorded capabilities were measured; it is **not a Lepton
launch pass**. The report explicitly keeps `lepton_runtime_verified`,
`hardware_verified` and `flash_image` false.

Kernel image SHA-256:
`7dbcd65685de1c7a7f4538849ebbfe2c3dd375020ff13332c958bfccce44811c`.
Both the original build and the tested image are preserved. Earlier v1/v2 logs
record fixture setup failures, before the successful run; they are not discarded.

The source confirms that this vendor 5.10 tree enables user-namespace mounts for
BinderFS but not OverlayFS. [Podman documents a fuse-overlayfs fallback for
older kernels](https://docs.podman.io/en/latest/markdown/podman.1.html#note-unsupported-file-systems-in-rootless-mode).
Before choosing that fallback, inspect the actual Lepton launcher: Podman's
storage driver and an explicit Android `/data` overlay can be separate mounts.
The separate Podman mount test below now verifies the helper on both types
of mount. A kernel backport is not required solely to satisfy this tested
writable-mount path.

See [the Frame investigation](steam-frame.md) for source availability and the
remaining Android, native runtime and hardware boundaries.

## Podman writable-mount acceptance

Valve's public [Lepton v3.0.5 launcher](https://gitlab.steamos.cloud/frame-public/lepton/-/blob/8be10c8a86ed2a6de1c6a2d6587994e957a35515/compat_tool/liblepton/liblepton.sh#L181)
uses rootfs `:O`, with separate persistent data and APK `:O` views. Podman
5.6.2 passes its configured mount helper to both
[rootfs overlays](https://github.com/containers/podman/blob/v5.6.2/libpod/container_internal.go#L1785)
and [explicit overlay volumes](https://github.com/containers/podman/blob/v5.6.2/libpod/container_internal_common.go#L474).
The kernel's native OverlayFS limitation can therefore be tested with FUSE
without changing the kernel or substituting a privileged Android container.

```sh
just build-android-container-tools
just test-android-mounts output/kernel/quest3/qemu-abi-v10 output/android-mounts-new
```

This separate runner needs Docker, Zig and QEMU. It exports an immutable ARM64
dependency image containing Podman 5.6.2 and fuse-overlayfs 1.15, verifies its
bounded archive, and creates a new 256 MiB guest root disk. The root disk is
necessary because OCI `pivot_root` cannot use the initial initramfs root. QEMU
uses snapshot writes, no networking, host shares, USB or physical devices. The
runner checks that the base disk, kernel and source hashes remain unchanged.
The fixture requires PID 1, the QEMU board and an exact test command-line token.
All container writes are in guest tmpfs.

On October 7, 2026, `output/lepton-mount-v8/result.json` passes on the same
`qemu-abi-v10` kernel image recorded above:

- Guest UID 1000 starts rootless Podman; container PID 1 sees UID 0 through
  Lepton's `keep-id` mapping.
- Rootfs, data and APK mount entries use FUSE. The rootfs rejects writes.
- Data copy-up, APK replacement and deletion whiteouts survive restart.
- A second context sees the unchanged lower data and APK, independently of
  the first context's upper directories.
- Lower files remain unchanged; stopping removes the container and FUSE mounts
  from Podman's mount namespace. The bounded stop deliberately reaches SIGKILL
  for a non-cooperative fixture process.

This resolves the measured writable-mount prerequisite for this Podman/helper
pair. Native rootless kernel OverlayFS remains unsupported. It does not measure
FUSE performance or establish graceful Android shutdown, Android boot, APK
execution, delegated Android cgroups, Binder service transactions, graphics or
OpenXR. The runner keeps those proof levels false.

Actual Lepton startup requires its matching patched Android rootfs, sysbake and
xattrs. Public source and a free development-app listing do not establish
anonymous access to that payload. The inspected official Frame repair image
provides native SteamVR and host Android graphics overlays, but not the Lepton
Android rootfs. See [native runtime availability](steamvr-arm64.md).
