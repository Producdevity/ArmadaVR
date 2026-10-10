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
- Binder request/reply, file-descriptor passing, sender identity and 32
  epoll/thread-exit cycles.
- Ordinary unprivileged IPv4/IPv6 datagram sockets, raw-socket rejection and
  UDP loopback traffic inside the new network namespace.
- Shared memfd writes across a fork and enforcement of write/size seals.
- A small seccomp filter that denies one syscall and permits another.
- Rootless native OverlayFS mounting and, when supported, preservation of the
  lower file during copy-up.
- Removal of private mounts when the child exits, followed by guest power-off.

The seccomp filter is a test, not a policy for Android applications. The Binder
checks exercise real driver ioctls and context isolation; they do not exchange
Android service transactions. The memory check does not establish graphics
buffer or synchronization interoperability. Subordinate ID ranges, cgroup
delegation, external networking, Podman, Android boot, APK execution and graphics remain
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

On October 9, the expanded Binder and networking fixture passes on the same
Quest kernel and the maintained [Pico Linux kernel](pico-firmware.md).
The Pico test uses a private diskless adapter for the vendor kernel's virtual
board and logging limitations; the maintained runner still requires a QEMU
transport build. Both kernels retain the native rootless OverlayFS limitation.
Pico's separate stock-policy test verifies enforcing access denial; the container
ABI fixture does not load an Android policy.

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
bounded archive, and creates a new 256 MiB guest root disk. OCI `pivot_root`
requires leaving the initial initramfs root filesystem. QEMU
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

The October 9 fixture also stops Podman's namespace pause process using
[`podman system migrate`](https://docs.podman.io/en/v5.6.2/markdown/podman-system-migrate.1.html).
Its PID 1 reaps adopted container helpers throughout execution and requires no
remaining children before power-off. An earlier minimal init left helpers as
zombies and stalled during container exit on the Pico kernel.

The same shell checks pass on the maintained Pico Linux kernel in a diskless
test with a separate tmpfs root. That private adapter supplies fresh host entropy
and relays bounded log lines to the kernel buffer. It retains the virtual-board
accommodations documented in [the Pico kernel results](pico-firmware.md).
These results establish the tested FUSE mount path on both kernels; Pico Android
startup and physical random-number generation remain unverified.

This resolves the measured writable-mount prerequisite for this Podman/helper
pair. Native rootless kernel OverlayFS remains unsupported. It does not measure
FUSE performance or establish graceful Android shutdown, Android boot, APK
execution, delegated Android cgroups, Binder service transactions, graphics or
OpenXR. The runner keeps those proof levels false.

## Acquired Lepton and actual Android boot

On October 8, 2026, a normal authenticated native Steam session installed app
`3029110` through `steam://install/3029110`. Build `25257944` supplies launcher
`2.8.14`, image `2.8.11`, Android 11 / SDK 30 and security patch `2024-02-05`.
Installed depot manifests are `3029111:5805845383697258311` and
`3029112:773148386728154884`. Anonymous depot access remains a separate route.
The original 3,485 files/links and matching sysbake/xattrs were preserved before
execution. The complete preserved archive has SHA-256
`93b86093ef41f3d2f9c0a72ffddf74cd6540fe8c406e7312b986f908d073030e`.
Steam content and Frame binaries remain ignored local inputs.

The execution copy uses Podman 5.8.7, crun, fuse-overlayfs, subordinate IDs and
a real user session with PipeWire/PulseAudio. Configure the user's default
`containers/storage.conf`, not only `CONTAINERS_STORAGE_CONF`: the delivered
launcher clears the environment for some Podman exec calls. Software startup
uses a real headless Weston socket, SwiftShader and the original Android
Fossilize layer/manifest extracted from Frame. A narrow `steamvr` metadata
adapter supplies private mount directories; it provides no XR runtime.

Four [version-specific integration patches](../patches/lepton/README.md) resolve
the original startup and installation failures without changing the kernel,
Android init or vendor version. A fifth patch addresses launcher shutdown:

- The host ashmem node exists but the container never mounts it. Enable memfd
  rather than inferring container capability from host device existence.
- Podman's default read-only `/proc/sys` prevents Android's netd from
  configuring IPv6 in its private network namespace. Selectively unmask that
  entry; retain the other default restrictions and rootless credentials.
- Use ADB's supported nonstreaming shell-v2 installation and preserve the real
  package-command status in Lepton's overlay hook, including layer restoration
  after failure. The old hook hides failed installations behind exit zero.
- Preserve the source APK timestamp in temporary copies so the baked-app
  decision stays consistent between mount setup and the post-boot launch.
- Wait for Android's shutdown before forcing the container to stop. Bound the
  wait and preserve forced cleanup for a failed or expired wait.

A finite user service also needs enough tasks: a 512-task cap caused real
`pthread_create` failures and killed system_server. A 2,048-task cap passes;
the first stable run used 622 tasks and about 1.67 GB of cgroup memory.

Runs `v8`, `v9` and `v10` under `output/blockers-20261008-v1` reach the actual
`sys.boot_completed=1`, running zygote and a single system_server startup.
Activity, package, window and SurfaceFlinger Binder services respond. A data
marker survives context restart. With the network patch, Ethernet reports
`CONNECTED` and `VALIDATED`, with the configured address, route and DNS; a real
outbound packet also succeeds. A locally built APK installs and starts through
PackageManager/ActivityManager, and its red/blue frame is visible in Android's
actual screencap. These are Android framework and software-rendering results,
not Lepton OpenXR or headset hardware results. In the independent `v12`
context, the earlier data marker is absent, Android boots normally, and the
same APK installs through the VM's ADB endpoint. Two injected taps update its
stored counter to 2 and appear in the captured frame. Raw Podman exec lacks
Android init's Java environment and silently fails to start the input tool;
the normal ADB shell supplies that environment. This was a test-harness issue,
not an Android touch failure. The VM's ADB server and serial are explicit and
separate from any physical-device ADB session.

The normal `lepton start <apk>` path now also passes in `v21` and an exact
packaged-patch repeat `v22` under `output/blockers-20261008-v2`.
The launcher installs and auto-focuses the APK;
two taps save and render counter 2. A second normal launch selects the baked
package, reads counter 2 before input, then saves and renders counter 4.
Both launchers return zero after deliberate Android force-stop. Back leaves
this fixture's process alive and is not counted as a launcher exit. These
standalone runs use `LEPTON_NO_CLEANUP` and `LEPTON_KEEP_CONTEXT`; they do not
establish default Steam compatibility-tool save behavior. The earlier manually
installed package disappearing from a developer context remains preserved.

A separate `v26` test exercises `waitforexitandrun` with a synthetic Steam app
ID and private install/compatdata directories, without either retention flag.
Normal cleanup removes each temporary prefix, overlay work directories and
the three Android settings files. The second launch restores counter 2 and
saves/renders counter 4; both launcher exits are zero after deliberate app
force-stop. The delivered cleanup retains an exited Podman record, which the
next launch replaces. An earlier fixture incorrectly required that record to
be removed; its failed result is preserved.

This passes the default compatibility-tool save lifecycle for the test APK.
An actual Steam game, cloud saves and version/depot migration remain untested.
Both containers exit 137 through the launcher's forced stop, so graceful
Android shutdown remains unproven. The earlier harness bypasses Lepton's normal
process-group wrapper and leaves Avahi publishers for systemd to contain.
A `v27` run uses that wrapper, verifies the launcher's own process group,
restores counter 4 and saves/renders counter 6. It exits zero and cleans its
publishers without a systemd kill. The pre-test data snapshot and failures are
retained in `output/blockers-20261008-v2/lepton-default-lifecycle-v2.tar.gz`;
the normal-wrapper result is in `blockers-final-v3.tar.gz`.

The actual host ADB is android-tools 37.0.0 and the guest advertises `shell_v2`.
Invalid APK installation returns 255, an unknown `cmd` service returns 20,
and a shell that prints to stderr and exits 7 returns 7. The old Android linker
still warns about the kernel vDSO's known BTI dynamic tag. Streaming ADB treats
those warnings before `Success` as failure; nonstreaming returns the real
shell-v2 exit status. The flag alone fails the invalid-APK test with the old
hook and must not be applied without its status repair. Neither Android BTI
nor the kernel is weakened. Capture screenshots to a guest file before reading
their bytes: `adb exec-out` combines these warnings with PNG output.
The exact-patch repeat also confirms the same two Fossilize layer bind targets
before and after the deliberately failed installation.

## Android shutdown comparison

Normal Android `sys.powerctl=shutdown` stops the original context and both the
container and launcher return zero. After service stop, synchronization and
unmount, however, init aborts through
`exit` → static C++ finalization → `std::thread` destruction. The delivered
binary's non-reboot-capable branch calls `exit(0)`, consistent with
[Android 11's source](https://android.googlesource.com/platform/system/core/+/refs/tags/android-11.0.0_r48/init/reboot_utils.cpp#82).
Exit status alone therefore misses the failure.

The October 10 comparison changes only that call to the binary's existing
`_exit` import in a separate, read-only overlay. The original init reproduces
the fatal shutdown; the diagnostic copy boots and shuts down cleanly twice,
preserving a data marker across restart. The actual running init hash is checked
on each boot. `CAP_SYS_BOOT` remains absent, kernel taint remains zero, and the
host init process and context mounts disappear after each shutdown.

An actual APK test identifies an independent launcher problem: after an app
exits, Lepton requests shutdown then immediately force-stops the container.
Even with the corrected init, the original launcher produces container exit 137
without completing synchronization. The fifth integration patch waits up to
15 seconds for shutdown, with a further two-second kill deadline for a stuck
wait process, then uses the original forced-stop fallback if necessary.

With both corrections, app force-stop completes Android shutdown and a second
launch restores counter 2, saves counter 4, and shuts down through
`sys.powerctl=shutdown` while the app is in the foreground. Both containers and
launchers exit zero without the fatal stack or forced-stop fallback. Default
cleanup removes temporary prefixes, work directories, settings and owned
mounts; no retention flags or process-group bypass are used. The exact packaged
launcher patch also passes normal, already-stopped, absent, failed-wait and
non-cooperative-wait checks; those fallback checks use a Podman command stub
and the real GNU timeout, separately from the real Android runs.

Evidence is preserved under `output/lepton-shutdown-20261010-v1`. The init change
is a diagnostic binary comparison, not a matching source rebuild or a shipped
replacement. Its source candidate applies to pinned Lineage 18.1, but the
complete resolved product inputs and source-built acceptance remain open.
Native Android graphics, OpenXR, actual game saves and physical-device support
are not established by this software-rendered APK test. Earlier failures and
original inputs remain intact; physical devices, host filesystem shares and
USB passthrough were absent from this VM.

## Android OpenXR runtime connection

A native Android test APK now loads Frame SteamVR 2.17.10's Bionic ARM64
runtime through its delivered OpenXR loader. It creates an instance, discovers
the simulated HMD and enumerates two 624×624 stereo views. An OpenGL ES 3.0
context and OpenXR graphics session also initialize successfully.

The runtime uses abstract Unix sockets, which are scoped to a network
namespace. With SteamVR outside Lepton's private namespace, instance creation
fails with `XR_ERROR_RUNTIME_FAILURE` and connection refused. Starting the
owned native backend inside that same private namespace resolves the
connection. The guest's host network namespace remains separate, and the
runtime correctly translates the Android client PID. This is a controlled VM
comparison; persistent Steam/Lepton session orchestration is not implemented.

The delivered software drivers do not meet the runtime's requirements:

- SwiftShader's GLES implementation lacks the external-memory and semaphore
  entrypoints used during SteamVR swapchain allocation. Selecting the
  advertised sRGB format reaches that failure; plain `GL_RGBA8` is unsupported.
- Its Vulkan 1.1 driver supports opaque-FD color-image import/export, but lacks
  `VK_KHR_timeline_semaphore` and `VK_KHR_image_format_list` required by this
  runtime.
- A headless session starts and exits normally, but its frame calls return
  zero timestamps. The pose queries therefore fail with `XR_ERROR_TIME_INVALID`;
  this is not pose or timing acceptance.

Evidence and failed comparisons are retained in
`output/android-openxr-followup-20261010-v1/evidence-v2`. All 129 archived files
were checksum-verified. Android shutdown, original launcher hashes, normal VM
power-off and unchanged parent/kernel/runtime inputs were checked separately.
These initial connection tests retain the private diagnostic init described
above; their results do not establish stereo rendering or device readiness.

## Android Vulkan stereo rendering

A subsequent isolated VM test replaces the software Vulkan driver with a
private Android build of Mesa 26.1.8 and the repository's Lavapipe sharing
patches. It uses the installed Android NDK and checksum-pinned Android LLVM
21.1.8 dependencies. Original Lepton and SteamVR inputs remain unchanged.
This build is an experimental test input, not an installed product driver.

Android now exposes the required timeline-semaphore and image-format-list
extensions. The actual Frame Android OpenXR runtime creates both swapchains,
submits 60 stereo layers with valid view poses, and exits normally. An
independent observer records 59 compositor presents from the Android client's
translated host PID. Captured compositor output shows red/blue eyes followed
by yellow/blue after the application changes its left-eye image. Closing the
virtual dashboard is required for the scene to gain focus and become visible;
the earlier obscured comparison is retained as a failure.

This establishes basic Android Vulkan stereo rendering, not usable frame
timing. Predicted display times jump and then advance by only one nanosecond
for 56 of 59 intervals. Compositor present-wait errors and implausible dropped
frame counts also remain. Positive, increasing timestamps alone are therefore
insufficient timing acceptance.

Evidence is retained in `output/android-mesa-followup-20261010-v1/evidence-v1`
and `vulkan-rendering-acceptance-v1.json`; all 160 archived files were
checksum-verified. Android and OpenXR shutdown succeed with the same private
diagnostic init. Controller actions, persistent namespace
orchestration, matching source-built init, games and physical headset graphics
still require validation. Neither headset is flash-ready from this result.

## Android GLES stereo rendering

The companion Android EGL/GLES build uses Zink over the same Lavapipe driver.
Select Zink with `MESA_LOADER_DRIVER_OVERRIDE=zink` and select its CPU Vulkan
device with `D3D_ALWAYS_SOFTWARE=true`. Do not use `LIBGL_ALWAYS_SOFTWARE` for
this Android configuration: it selects Mesa's separate software EGL path,
whose loader interface is incompatible with the Android image loader. The
failed SurfaceFlinger startup and backtrace are preserved.

Mesa 26.1.8 also advertises external-memory and semaphore extensions in the
ES 3.1 context while gating their dispatch entries to ES 3.2. Consequently,
`glCreateMemoryObjectsEXT` returns `GL_INVALID_OPERATION`, leaving OpenXR
swapchain textures without storage. The
[dispatch patch](../patches/mesa/0005-gles-external-object-dispatch.patch)
enables memory entrypoints from ES 3.0 and semaphore/common entrypoints from
ES 2.0. It retains each implementation's extension checks and leaves desktop
direct-state-access entries unchanged. The
[Khronos specification](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_external_objects.txt)
requires texture-storage support for memory objects; ES 3.2 is the specification
it was written against, not its minimum requirement.

The original driver fails the actual Android ES 3.1 test. With the correction,
both 624 × 624 swapchains render 60 stereo frames, all 120 eye readbacks have
complete framebuffers and no GL error, and independent compositor captures
show red/blue followed by yellow/blue. The observer attributes 61 compositor
presents to the translated Android process. Android and OpenXR exit normally;
the private mount/property scripts are restored and kernel taint remains zero.

All 190 files in `output/android-mesa-followup-20261010-v1/evidence-v2` were
checksum-verified. `gles-rendering-acceptance-v1.json` records the captures and
remaining timing failure: 55 of 59 predicted-time intervals are one nanosecond.
The patch also applies without fuzz to the pinned archive and reproduces the
tested source. These are software-rendered VM results. Reproducible Android
driver packaging, controller actions, session orchestration, matching init
source, games and physical graphics still require work.
