# Offline headset root filesystem

`tools/build-headset-root.py` creates a regular ext4 filesystem image from an
existing ARM64 Linux container and one matching kernel, Quest startup initramfs
and firmware bundle. It has no device installation operation. The result is an
offline integration artifact, not a validated Quest installation image.

```sh
python3 -B tools/build-headset-root.py KERNEL_BUILD STARTUP_INITRAMFS \
  --firmware QUEST_FIRMWARE --image RUNTIME_IMAGE \
  --output NEW_ROOT_DIRECTORY
```

The builder resolves `RUNTIME_IMAGE` to its immutable Docker ID. It exports a
new, never-started container, then constructs the filesystem in a separate,
network-disabled container limited to two CPUs, 2 GiB and five minutes. It never
mounts or formats a host block device. `rootfs.tar` preserves the exported input;
previous output directories are refused.

The root includes the selected vendor module set, the verified 48-file ADSP
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

The builder removes the exported distribution kernel modules and VM boot images,
known QEMU-specific display/network configuration and guest-agent enablement
when present, and the
automatic lab-test and virtual-desktop enable links. It clears the container
machine identity, selects `multi-user.target`, and uses hostname `armada-vr`.
The existing VR programs and acceptance tools remain available. There is no
automatic physical VR session yet; headset display, tracking and runtime
integration are still required.

The default label is `armada-vr-root`; `--root-label` must agree with the offline
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
there for the successful root case.

The successful root case requires the mapper's original PID and real QRTR/QMI
responses across handoff, manual restart refusal, native and Windows VR lab
acceptance, clean shutdown and an unchanged complete backing-image hash. A
separate `root-fault` case terminates the actual mapper after handoff and requires
orderly filesystem shutdown and power-off. `--case NAME` can select individual
cases; the default runs all eight startup cases.

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
