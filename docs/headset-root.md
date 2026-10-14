# Offline headset root filesystem

`tools/build-headset-root.py` creates a regular ext4 filesystem image from an
existing ARM64 Linux container and one matching headset kernel, startup initramfs
and firmware bundle. It has no device installation operation. The result is an
offline integration artifact; physical boot and recovery require separate checks.

```sh
python3 -B tools/build-headset-root.py KERNEL_BUILD STARTUP_INITRAMFS \
  --firmware QUEST_FIRMWARE --image RUNTIME_IMAGE \
  --output NEW_ROOT_DIRECTORY
```

The default profile is `quest3`. For Pico, select `--profile pico-neo3`, pass the
prepared [Adreno 650 firmware bundle](pico-firmware.md) as `--firmware`, and add
`--systemd-runtime PREPARED_ROOT_RUNTIME`. The Pico kernel and initramfs must
match, the initramfs must already contain its compatible systemd runtime, and
Quest startup services are refused. Pico does not use the separate Quest
`--gpu-firmware` argument.

The root runtime must have `scope=root`. Its template files, command inventory
and native loader/libc identities must match the assembled userspace after any
RPM transaction. The builder installs it before formatting and verifies every
runtime file, link, permission and native ABI hash after extraction from ext4.
Systemd unit filenames containing literal backslashes are extracted with their
parent directory so the debugfs command parser cannot reinterpret those names.
This does not replace the distribution's unrelated services or enable a headset
session. Prepared runtime and firmware binaries remain private build inputs.

The builder resolves `RUNTIME_IMAGE` to its immutable Docker ID. It exports a
new, never-started container, then constructs the filesystem in a separate,
network-disabled container limited to two CPUs, 2 GiB and fifteen minutes. It never
mounts or formats a host block device. `rootfs.tar` preserves the exported input;
previous output directories are refused.

Before creating or exporting a container, the builder checks available output
space against the requested filesystem size, a new export when needed, estimated
unpacked container scratch space and 1 GiB of headroom. On Docker Desktop these
usually share the host disk. The scratch estimate includes verification work and
four times the optional RPM download size; unusually large package expansion can
still exceed it. A failed preflight retains its report without starting a large
export.

With `--reuse-export PREVIOUS_ROOT_DIRECTORY`, the builder instead reads that
successful build's retained `rootfs.tar` or `rootfs.tar.zst`. The immutable
userspace image must match, and the export's checksum must match before and
after construction. Compressed exports must reproduce the original uncompressed
checksum; streaming verification and extraction avoid an extra expanded archive.
The archive is mounted read-only; `manifest.json` records its source path,
source manifest hash and archive hash. The new output does not duplicate the
archive. Preserve the source directory alongside the new root.

If the original Docker image was removed, use `--userspace-export
PREVIOUS_ROOT_DIRECTORY` instead. This keeps the archived userspace's original
image identity in `container_image`; `--image` supplies Linux ARM64 build tools
and is recorded separately as `build_container_image`. The source must still
have a successful root-build manifest and a matching export checksum. The
builder never relabels a replacement image as the deleted one.

Use `--runtime-rpms DIRECTORY` to add missing dependencies offline. The directory
must contain only the RPMs and a `manifest.json` of this form:

```json
{"schema_version": 1, "packages": [{"path": "PACKAGE.aarch64.rpm", "sha256": "SHA256"}]}
```

Supply the dependency closure for the archived userspace. The builder checks
file hashes, requires successful package signatures against that userspace's
trusted RPM keys, and runs an RPM transaction test before installation. It does
not fetch packages, import new keys or disable signature/dependency checks.
Installation logs and the package manifest are retained; the manifest is also
included in the root image.

The Quest root includes the selected vendor module set, the verified 48-file ADSP
bundle, their build records, and the exact startup service used in the initramfs.
The mapper and startup helper remain in the initramfs; the root does not replace
that running storage service. Runtime libraries and VR software come from the
selected userspace image.

Use `--turnip VERIFIED_MESA_BUILD` to add the native AArch64 KGSL driver.
The builder verifies its source and test identities, library/ICD checksums and
actual DRM compile/link evidence, loads it inside the assembled userspace, and
checks the extracted files from the finished ext4 image. See
[Turnip build and evidence boundaries](turnip.md). This does not select a global
ICD or enable an automatic headset session.

Use `--monado VERIFIED_MONADO_BUILD` to install the native service, client libraries,
tools and OpenXR manifest under `/opt/reframed-vr/monado`. The builder validates
source/build/test identities, native ELF architecture, exact file inventories,
permissions and relative library links. It refuses an existing bundle at that
prefix and preserves the distribution's current runtime selection. No Monado
service is enabled. See [Monado build and runtime checks](monado.md).

A small, read-only container first checks the new service's dependencies and
immediately loads both client libraries against the chosen immutable userspace
image. Missing libraries or symbols stop the build before export. With a detached
userspace export or additional RPMs, this image-only probe is inapplicable and
the required check runs against the staged userspace before formatting. The assembled
root repeats these checks in a chroot, then the files, modes, links and build
record extracted from ext4 must match the validated bundle exactly. The userspace
must supply its dependencies, including `opencv-videoio` for the current
build; use an already complete image or explicitly provide signed runtime RPMs.

`--gpu-firmware VERIFIED_GPU_BUNDLE` adds the five KGSL firmware files selected
by the [firmware preparer](quest-firmware.md#gpu-firmware). The GPU and ADSP
bundles must declare the same reference build and vendor-image hash. GPU files
are checked again after construction and against copies extracted from ext4.
They are packaged as data; the builder does not execute them.

### Lepton and Android init

Prepare a separate execution archive from an acquired Lepton payload and a
compatible source-built init:

```sh
python3 -B tools/prepare-lepton.py ACQUIRED_TAR_GZ ACQUISITION_MANIFEST \
  --archive-sha256 ACQUIRED_SHA256 --init-build INIT_BUILD_DIRECTORY \
  --output NEW_LEPTON_DIRECTORY
```

This requires Python, GNU patch and Bash 4.2 or later. `--bash PATH` selects the
syntax-checking shell; macOS's system Bash is too old. The supported input is
Lepton launcher 2.8.14/image 2.8.11, app 3029110/build 25257944. The acquisition
manifest must list every regular file and link with its original size, hash,
mode or target. `INIT_BUILD_DIRECTORY` supplies `init-candidate` and its matching
`manifest.json`; see the [compatible init build](../patches/android-init/README.md).
The caller supplies the acquisition checksum and build record. Preparation does
not establish their independent provenance or prove Android boot.

The preparer checks the release, complete inventory, original launcher/init
hashes and ARM64 init format. It applies the fourteen maintained launcher patches
without fuzz and inserts init only into `images/rootfs_overlay/system/bin/`.
The original Android init, licenses, payload metadata and acquired archive remain
unchanged. Android's absolute links remain archive data; archive traversal,
duplicate names, special files and paths below symlinks are rejected. A complete
roundtrip checks the new archive's contents and metadata before publication.

Pass the resulting directory as `--lepton NEW_LEPTON_DIRECTORY`, together with
`--lepton-host FOSSILIZE_BUNDLE`, when building the root. The latter has a version
1 `manifest.json` containing `path`, `bytes`, `mode` and `sha256` for exactly:

- `usr/lib/libVkLayer_fossilize.so` (0755, Linux ARM64)
- `usr/share/guestos/android/vendor/vulkan_layers/libVkLayer_fossilize.so` (0755, Android ARM64)
- `usr/share/vulkan/implicit_layer.d/VkLayer_fossilize.json` (0644)

The manifest must identify `VK_LAYER_fossilize` and its native library path.
Preserve the acquisition provenance and licenses alongside the source material;
this checksum validation does not authenticate a manufacturer signature or grant
redistribution rights. These files provide the shader-capture dependency, not
a complete Android graphics overlay.

The execution bundle is installed under `/opt/reframed-vr/lepton`, with its build
record under `/usr/share/reframed-vr`. The builder verifies extraction from the
finished ext4 image, including regular-file timestamps used by APK baking. The
archive preparer retains extended attributes; root-image packaging currently
refuses bundles containing them until their extraction verification is supported.

The userspace must supply Podman, fuse-overlayfs, ADB, the UID/GID-map helpers,
conmon, crun, attr, inotify-tools, jq, iproute, which, hostname and ncurses.
Resolve missing dependencies against the chosen userspace's RPM
database, then provide the signed closure with `--runtime-rpms`. The builder checks
native relocations and package versions without requiring procfs in its offline
chroot. Actual container execution requires a booted system.

Fedora records `cap_setuid=ep` and `cap_setgid=ep` for its UID-map helpers, but
Docker exports omit those file capabilities. The Lepton integration verifies the
RPM metadata, restores those two capabilities and compares their raw extended
attributes after ext4 construction. It also provides Lepton's Android SDK ADB
path as a link to the installed `/usr/bin/adb`. It does not enable a Lepton
service or supply a headset-specific Android host overlay, Wayland session or
SteamVR session; those remain runtime integration requirements.

Lepton stages its baked Android data beneath the launch prefix before restoring
the depot's extended attributes. This preserves a read-only, shared installation.
The prefix, Android data and Podman storage must reside on filesystems supporting
user extended attributes; the tested 5.10 kernel's tmpfs does not support these
Android attributes. Use ext4 for the writable state, including a caller-provided
`LEPTON_PREFIX`, rather than the launcher's default temporary directory. The
offline root still uses a RAM overlay and does not configure persistent user data.
Absent debugfs and tracing directories are optional; their omission leaves those
debugging features unavailable without blocking Android startup.

The builder installs a per-user Podman storage configuration for `vr` under
`/home/vr/.config/containers/storage.conf`, selecting `fuse-overlayfs`. Rootless
Podman does not inherit that option from the system-wide configuration on this
stack. Lepton preserves the host storage environment when attaching to Android,
so attach and launch resolve the same container database.

The earlier ten-patch Quest test root passes normal startup and the controlled mapper
failure case with the current 524 reference firmware. In a fresh VM, its installed
Lepton bundle boots Android twice, runs the input APK twice and restores its saved
counter from two to four. Both APK sessions end within 15 seconds of launch;
Android and Lepton exit zero, temporary prefixes are removed and the kernel
remains untainted. This exercises the short-session data-preservation patch with
normal cleanup and no runtime edits to the installed bundle.

These tests use read-only root media, ext4 writable state held in guest RAM,
software rendering and a private Steam metadata adapter. They establish packaging
and Android lifecycle behavior on the Quest kernel in QEMU. They do not establish
persistent user storage, Android XR integration, Pico userspace compatibility,
physical boot, recovery or headset driver operation. Subsequent fourteen-patch
Quest and Pico runtime results are recorded in
[Android container acceptance](android-containers.md#pico-application-storage-and-launcher-shutdown).

The builder removes the exported distribution kernel modules and VM boot images,
known QEMU-specific display/network configuration and guest-agent enablement
when present, and the
automatic lab-test and virtual-desktop enable links. It clears the container
machine identity, selects `multi-user.target`, and uses hostname `reframed-vr`.
The existing VR programs and acceptance tools remain available. There is no
automatic physical VR session yet; headset display, tracking and runtime
integration are still required.

The default label is `reframed-vr-root`; `--root-label` must agree with the offline
boot assembler's label. The default size is 8 GiB (`--size-gib` accepts 6–32).
The boot command line mounts ext4 read-only with journal replay disabled and an
OverlayFS upper layer in RAM. The root fstab introduces no other device mounts.
This does not repartition or decrypt Android storage and is not an updater.

After formatting, read-only `e2fsck` must pass. The builder extracts the unit,
modules and firmware from the finished filesystem with `debugfs` and verifies
their hashes, including exclusion of additional kernel modules. `manifest.json`
records the kernel variant, exact matching initramfs, source/image identities,
filesystem label and artifact hashes. It always records
`hardware_flash_image=false` and `hardware_boot_verified=false`.

## Virtual acceptance

For a Pico root built with the QEMU kernel and its matching initramfs, run:

```sh
python3 -B tools/test-pico-root.py PICO_QEMU_KERNEL PICO_QEMU_INITRAMFS PICO_QEMU_ROOT \
  --output NEW_TEST_DIRECTORY
```

This boots the actual packaged image read-only under TCG. The fixture only adds
an agent and probes in the RAM overlay; it does not replace modules or systemd.
The fixture resolves its explicitly named virtio port through sysfs, so the
packaged root does not need a QEMU-specific udev alias rule.
It verifies the installed kernel-module and GPU-firmware inventories, all
prepared systemd files and command dependencies, user-session startup and
service isolation. Acceptance requires a read-only backing device, a writable RAM
overlay, no disk writes, no failed services and normal poweroff. The kernel,
initramfs and complete root-image hashes must remain unchanged. Physical-kernel
roots and mismatched inputs are refused. QEMU does not execute Pico panel, GPU,
radio, sensor or controller hardware; this is userspace and boot-handoff evidence.
`--transport usb-storage` boots the same packaged root through emulated xHCI
and USB mass storage. The probe resolves the root by label, verifies its USB
ancestry and read-only block state, and checks the whole-disk write counter.
Native package checks include the installed Turnip build record, ICD and driver
hashes, ICD path resolution and immediate dynamic loading. This catches missing
or incompatible library dependencies; it does not execute GPU commands or verify
panel scanout. USB-root fixtures and failure cases are described in
[initramfs verification](headset-initramfs.md#verification).

The initial command line requests `selinux=0`, but this vendor kernel omits
`CONFIG_SECURITY_SELINUX_BOOTPARAM` and ignores that request. Use
`--enforcing-boot` to test the physical candidate's `selinux=1 enforcing=1` flags.
The probe reads the actual enforcement flag and policy-load counter through a
read-only selinuxfs mount; it does not load a policy or claim policy acceptance.

Build the root with the QEMU kernel and its matching startup initramfs, then run:

```sh
python3 -B tools/test-quest-startup.py QEMU_KERNEL QEMU_STARTUP_INITRAMFS QCOM_SOURCES \
  --rootfs QEMU_ROOT_DIRECTORY --output NEW_TEST_DIRECTORY
```

The test refuses a physical-kernel root or mismatched root/initramfs/kernel
records. It attaches the image read-only and exercises root discovery by its
actual label. Before switch-root, the fixture compares the unit already in the
image with the initramfs copy; it does not install a substitute. Only test tools
and their dependencies are copied into RAM, and the existing VR lab is enabled
there for the successful root case. The QMI probe uses a private library
directory, validates linkage with the root's dynamic loader, and must exchange
real QMI requests after handoff. Identical libc package revisions are not
required, and the fixture does not overwrite the root's global QRTR library.

The successful root case requires the mapper's original PID and real QRTR/QMI
responses across handoff, manual restart refusal, native and Windows VR lab
acceptance, clean shutdown and an unchanged complete backing-image hash. A
separate `root-fault` case terminates the actual mapper after handoff and requires
orderly filesystem shutdown and power-off. `--case NAME` can select individual
cases; the default runs all eight startup cases.

For a diskless initramfs check, explicitly select cases such as `--case ready`
and omit `--rootfs`. The runner then reports no root-image hash or root-handoff
claim. The default complete suite and explicit `root` or `root-fault` cases
still require a matching root image; a missing or older root is refused.

The hardware-facing ADSP/PMIC/UCSI/USB state is modeled. These tests do not boot
DSP firmware, negotiate USB-C power, execute on the headset or prove recovery.
The physical kernel still needs real panel/GPU, controller/tracking/calibration,
firmware, thermal and other driver validation. All SteamVR interaction milestones
and exact-device boot/recovery prerequisites remain in scope.

## Recorded acceptance

September 11: `output/headset-root-qemu-v1/` and `output/headset-root-v1/`
contain separately verified 8 GiB roots for the QEMU and physical kernel
variants. Both contain exactly the selected 272 modules, all 48 packaged ADSP
files and the matching startup unit, and both pass read-only filesystem checks.
The QEMU image SHA-256 is
`868373b22c5b05859a3569e05765be9165cd4320ae098a88c7945aef272eed37`;
the physical-kernel image SHA-256 is
`6317124ad3036c80a9fa76db0f4596bd16b7e49f0dcad0ba784e769a6fc568f5`.

`output/quest-startup-packaged-v1/` passes all eight cases. Normal root acceptance
takes 112.203 seconds including the native/Windows VR lab. Mapper termination
after handoff leads to clean power-off in 5.48 seconds. The QEMU root hash remains
unchanged. `output/quest3-boot-assembly-startup-v2/` round-trips the corresponding
unsigned physical-kernel containers with the corrected failure policy.

These are boot-stack and Linux userspace results. The physical-kernel root has
not executed on a headset. Stock SteamVR dashboard navigation and the remaining
physical driver, bootloader and recovery gates are not established by this run.

The subsequent `output/headset-root-qemu-v2/` and `output/headset-root-v2/`
include the corrected `turnip-drm-v7` library and ICD. Both pass loading against
the assembled userspace and extraction/hash checks on the finished filesystem.
The new QEMU root passes the normal boot/VR lab case in 112.719 seconds and the
mapper-failure shutdown case in 5.446 seconds, with the full root hash unchanged.
Evidence: `output/quest-startup-turnip-v1/`. The physical-kernel root remains
inspected offline only; the software-rendered QEMU lab does not execute KGSL.


`output/headset-root-qemu-v3/` and `output/headset-root-v3/` additionally include
the verified GPU firmware. Both reuse their v2 userspace exports read-only,
with archive hashes unchanged. The new QEMU root passes normal boot and the
VR lab in 112.913 seconds, and mapper-failure poweroff in 5.509 seconds.
Evidence: `output/quest-startup-gpu-firmware-v1/`. The separate diskless kernel
firmware test verifies lookup and MDT metadata using files extracted from the
completed root; it does not run the GPU.


September 12: `output/monado-root-package-v6/` exercises the maintained assembly
script with the v10 QEMU kernel modules, matching startup unit, 48 ADSP files,
five GPU files, `turnip-display-v2` and `monado/build-v3`. All 85 Monado regular
files and two relative library links survive installation and extraction with
matching hashes and modes. Native dependency/loading checks and read-only ext4
checks pass. The dependency fixture and 6 GiB logical filesystem were held in RAM;
the temporary filesystem was not retained. Logs and source identities remain.

This fixture contains the native dependencies from the selected runtime image,
not the complete headset userspace. It proves combined packaging and extraction,
not a full root build or boot. The last complete roots were v3 and do not match
the newer kernel/startup pair. They still need rebuilding and root-handoff
acceptance with the new graphics/runtime inputs.

`output/monado-root-audit-v1/` records the packaging result and 130 passing host
tests. The original runtime image fails the early dependency probe for missing
OpenCV videoio. The corrected runtime image passes that probe, then stops at the
output-space guard before export. Both preflight reports are preserved.

October 8: the prior root images and their Docker userspace image are no longer
present after storage cleanup. Their manifests remain, and the compressed v2
userspace export is intact at `output/headset-root-v2/rootfs.tar.zst`. Streaming
verification reproduces the original 6,902,722,560-byte export and its SHA-256
`8a8d5036014851f125cd7f83d1993c47bbf08e8b579dbf33f76e47bfe9234309`.
Evidence: `output/headset-root-archive-audit-20261008-v1/`.
This avoids reacquiring the older userspace contents; it does not restore the
missing immutable Docker image or supply the newer Monado dependencies. The
builder at the time required an uncompressed export and matching image for
`--reuse-export`. The newer export and RPM options above remove those restoration
requirements. Keep the original archive and its manifest as provenance.

October 9: `output/headset-root-quest-current-20261009-v1/` and
`output/headset-root-qemu-current-20261009-v1/` restore complete roots from that
archive using the new options. Each contains its matching 272-module kernel set,
the current build's 48 ADSP and five GPU firmware files, `turnip-display-v2` and
`monado/build-v3`. All 85 Monado files and two library links pass extraction
checks. The missing videoio dependency is supplied by 41 signed Fedora RPMs
(31,107,139 bytes), including their dependency closure. Package signatures and
transaction checks pass against the archived userspace, native dependency and
loader checks pass, and both ext4 filesystems pass read-only checks.

The immutable build-tool image and archived userspace identity remain separate
in each manifest. The compressed source archive is unchanged. Dependency
download and verification evidence is retained under
`output/headset-root-restore-20261008-v1/`. The physical-kernel root has not booted
on a headset and is not an installation image.

`output/quest-startup-root-current-20261009-v2/` passes normal root handoff and
the existing native/Windows VR lab in 110.810 seconds. Mapper failure after
handoff produces orderly poweroff in 5.558 seconds. The mapper PID survives
switch-root, QMI requests succeed before and after the refused manual restart,
and the complete backing-image hash remains unchanged. The preceding v1 failure
is preserved: its fixture required byte-identical libc files across initramfs
and root. The revised fixture validates the probe's actual dependencies and
isolates its library in RAM instead of replacing a global runtime library.

These are the existing software-rendered lab tests. They do not establish
native SteamVR compositor presentation, stock Library interaction or physical
KGSL rendering. The complete host suite passes 207 tests.

October 10: `output/headset-root-quest-gpuobj-20261010-v2/` rebuilds the physical
kernel root with `turnip-gpuobj-v1`, whose unchanged Linux driver passes offscreen
rendering on both stock headsets. The matching kernel/initramfs, 272 modules,
48 ADSP files, five GPU firmware files and 85-file Monado bundle are retained.
Signed dependency installation, native driver/client loading, extraction checks
and read-only filesystem checks pass. The image SHA-256 is
`14abd72c07e9ba55a2e14bba78cd027c1705a4c02dd9946df0cc035064d70f11`.
Its verified compressed copy reproduces all 8 GiB and that hash; the expanded
copy is removed afterward to conserve space. The original export and prior
roots remain unchanged.

The corresponding `headset-root-qemu-gpuobj-20261010-v1` now passes root handoff
and mapper-failure shutdown with an unchanged backing image. These two roots
use the previously current `52083180032000520` firmware.
Android init and native SteamVR acceptance recorded separately do not establish
their integration into this older archived userspace. The new physical root has
not booted on a headset and is not an installation image.

### Current Quest firmware, October 10

After the Quest updated to `52433670036000520`,
`headset-root-qemu-524-gpuobj-20261010-v1` was built with its matching new
initramfs, authenticated 48-file ADSP/five-file GPU bundles and the same tested
Turnip driver. Its SHA-256 is
`e8a3cfd777fc7c8fb07773a3e4cf97cc0bc13f67575ade0bf54c72aa2e27e401`.
All eight cases pass in `output/quest-startup-524-gpuobj-20261010-v1/`, including
native/Windows root acceptance, persistent mapper handoff and orderly shutdown
after mapper failure. The complete backing-image checksum remains unchanged.
The compressed archive reproduces all 8 GiB and that checksum before raw-image
removal. This updates firmware integration proof; it does not execute that
firmware in QEMU or establish physical boot/recovery.

The October 11 physical-configuration rebuild in
`output/headset-root-quest-lepton-524-20261011-v4` contains the complete
fourteen-patch Lepton bundle and source-built Android init. Its 8 GiB image has
SHA-256 `920eeb5d2c1668bd249b47af23961ddb66e5464ad256533613296d4f9ff5ca9c`.
The build completes in 150.963 seconds using the matching current-firmware
kernel/initramfs, 272 modules, ADSP/GPU firmware, Turnip, Monado and signed runtime
dependencies. Filesystem, extracted content, capabilities and native loader checks
pass; all 17 recorded output hashes were independently rechecked. The full image
is preserved in a verified compact representation with local restoration
instructions. This is offline assembly acceptance; the physical-configuration
image has not booted on a headset.

October 12: `output/headset-root-pico-20261012-v3` contains a complete 7 GiB
Pico physical-configuration root with the kernel through patch `0030`, its
38 modules, five authenticated-image Adreno 650 files, Turnip and Monado.
The prepared systemd 257 runtime is installed into the filesystem. Signed RPM
installation, native loaders, read-only filesystem checks and extracted runtime,
firmware and module checks pass. Its SHA-256 is
`31f4997766e257fc524e568c9509426ed22606ec3b3980ef4aa0ece909c1f2fe`.
The first build's extraction failure and second attempt's storage refusal are
preserved. The first filesystem is retained in a byte-verified compressed copy.

`output/headset-root-pico-qemu-20261012-v1` is an APFS clone of that physical
image with only the complete QEMU module set and kernel build record replaced.
All 52 module/support files were extracted and checked; the physical image hash
is unchanged. Its manifest records this derivation rather than a fresh userspace
build. `output/pico-root-acceptance-20261012-v3` boots it with the matching QEMU
kernel/initramfs, verifies all 541 systemd files, 128 command dependency closures,
40 modules and five firmware files, starts the user session, and passes service
isolation. Normal shutdown, zero disk writes and unchanged input hashes pass.
No runtime or module replacement occurs during this boot test. The known virtual
board warning/taint remains. Neither image establishes physical driver operation,
bootloader acceptance, damaged-boot restoration or flash readiness.

The subsequent `output/pico-root-acceptance-20261012-v4` repeats acceptance with
the candidate's enforcing boot flags and passes in 22.307 seconds, including
clean shutdown and unchanged inputs. SELinux reports enforcement enabled, policy
version support through 31, and **zero loaded policies**. The root contains no
SELinux configuration or targeted policy. Therefore the flag alone does not
establish mandatory-access-control protection. Earlier small policy-denial
fixtures remain separate evidence; they do not establish a full root policy.

October 13: `output/headset-root-pico-native-sensors-20261013-v1` updates the
verified physical-configuration Pico image with the eleven-patch SSC bundle and
matching rebuilt Monado. Its SHA-256 is
`0155a197503397b89ea8fb64766a23e73a1827a34e063ed78040119019b60727`.
The APFS-cloned predecessor remains byte-for-byte unchanged. Nine files change;
the kernel, all 38 modules, firmware and FastRPC inventory remain intact.
Exact readback, permissions, filesystem checks, 485 ELF dependency closures,
service/CLI symbol relocation and immediate OpenXR/client loading pass.

The matching runtime passes 25 upstream tests and two OpenXR service/client
cycles with both controller hands in the isolated physical-kernel VM. The
unsigned boot container in `output/pico-boot-container-20261013-v1` passes
assembly/unpacking checks with 25,415,680 bytes of partition headroom and the
preserved stock DTB pack. Head IMU sessions remain separate from Monado's HMD
path. These checks establish an offline artifact set, not physical boot,
display/tracking operation, damaged-boot recovery or flash readiness.

The October 13 root remains a raw APFS clone: only 593,920 bytes differ from its
verified parent. A trial compressed representation used more additional space
than the shared clone. Reconstructing the image from that archive, writing only
differing 4 KiB blocks into a new parent clone, reproduced the full checksum and
left the parent unchanged. The redundant archive was then removed. Receipts
remain alongside the image; apparent file allocation is not exclusive disk use.

## Pico development userspace

`Containerfile.pico` builds the current native diagnostic applications and creates
a locked `vr` development account on an ARM64 base containing the build
prerequisites, `systemd-pam` and freshly built Qualcomm services. Select that base explicitly:

```sh
docker build --platform linux/arm64 --network none --pull=false -f Containerfile.pico \
  --build-arg BASE_IMAGE=VERIFIED_QCOM_IMAGE -t localhost/reframed-vr:pico-base .
```

This is a native driver-development userspace. It does not add Steam, FEX, Proton
or Lepton. Supply separately verified packages when assembling those later
runtime layers. The root builder still rejects old project paths, installs the
matching physical or QEMU module set and checks the complete native package
closure. Neither the base nor the root starts the Pico HMD automatically; see
[Pico HMD calibration and activation](pico-hmd.md).

For a root without a packaged QEMU agent, `tools/test-pico-root.py --guest-agent
ARM64_QEMU_AGENT` stages the supplied bounded ARM64 executable in guest RAM. The
agent hash is recorded. Repeat `--guest-agent-library LIBRARY` for any extra
agent dependencies; they are hash-recorded and loaded from the agent's private
RAM directory. This leaves the image's package contents and runtime
selection intact, and the regular root media remains read-only.

The current Pico artifact set is in `output/pico-usb-host-20261014-v1/`.
The physical `root`, matching `initramfs` and unsigned `boot` include the USB
host-startup cleanup and the verified Turnip allocation, import and PRIME
ownership fixes. Full filesystem/component readback and boot-container
roundtrip checks pass. The matching native Monado and SSC packages are retained;
there is no calibrated HMD autostart.

The `qemu-root` is an APFS clone of that physical root with the VM kernel's
complete module tree and kernel build record substituted. Full module bytes,
permissions and component readback pass; the physical parent remains unchanged.
Physical and virtual kernels have identical patched source. Their 38 and 40
modules respectively pass signature, import-version and dependency checks.

A fresh 33.889-second USB-storage VM boot verifies native package relocation,
Turnip DSO loading, 40 matched modules, user sessions and service isolation.
Backing-disk writes remain zero, input hashes are unchanged, and shutdown is
normal. This run does not exercise Adreno rendering or physical scanout. Its
SELinux flag is set but no policy is loaded, so it is not policy-enforcement
acceptance. Physical USB transport, display, boot and recovery remain unverified.
