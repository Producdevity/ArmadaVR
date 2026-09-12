# Linux initramfs and root handoff

`tools/build-headset-initramfs.py` builds a dracut initramfs from one verified
vendor-kernel build. It accepts the Linux-userspace or separate QEMU ABI variant
and records which one was used. It checks the Image, module archive, inventory
and configuration against their build hashes before starting the container.

```sh
python3 -B tools/build-headset-initramfs.py KERNEL_BUILD --output NEW_DIRECTORY
```

The builder needs the project's Fedora VM container with dracut, systemd and
kmod, or a compatible image selected with `--image`. It resolves the image to
an immutable Docker ID, disables networking and limits the build to two CPUs,
2 GiB and four minutes. Docker's init process is required: this Fedora image's
`timeout` exits 125 when used directly as container PID 1. Module staging,
dracut temporary files and unpacking use bounded RAM mounts, so they do not
expand the container's writable disk layer. The compressed output and logs
remain in the selected output directory.

All modules are taken from the selected build. After unpacking the result, the
builder verifies every module's hash and rejects additional modules, mixed
kernel releases and missing boot components. It includes dracut's initqueue and
emergency handling as well as systemd's volatile-root service. It does not embed
the builder host's root device, fstab or disk configuration, and includes no
filesystem repair helper. Inputs and previous outputs are never overwritten.

`--firmware DIRECTORY` optionally includes a [prepared Quest ADSP bundle](quest-firmware.md).
The builder verifies its manifest before and after construction, then checks
every firmware hash and rejects unexpected firmware in the unpacked image.
Firmware inclusion alone does not add an ADSP startup service. Add
`--quest-services --image localhost/armada-vr:qcom-services` to include the
[bounded Quest startup and mapper lifecycle](quest-early-boot.md). The builder
checks the pinned mapper source profile and patches, then verifies the installed
helper, unit, enable link, firmware checksum manifest and mapper binaries.
Activation also requires `armada.quest=usb-root`, emitted by the offline assembler
for this variant.

The root handoff currently uses a dedicated ext4 root selected by label, with
journal loading disabled and writable state held in RAM:

```text
root=LABEL=armada-vr-root ro rootfstype=ext4 rootflags=noload
systemd.volatile=overlay rd.fstab=0 rd.luks=0 rd.lvm=0 rd.md=0
rd.systemd.gpt_auto=0 systemd.gpt_auto=0
rd.shell=0 rd.emergency=poweroff rd.retry=15 rd.timeout=30
```

These arguments are supplied by the boot container, not silently injected into
an existing root filesystem. This first handoff does not use or decrypt Android
userdata. The [offline root builder](headset-root.md) packages the matching modules,
firmware and service unit. External storage still needs its actual Quest USB/UFS hardware path
tested. The module inventory includes UFS, USB and their dependencies; inclusion
does not prove driver operation or availability of the required firmware.

## Verification

Build a separate initramfs from the QEMU ABI kernel, then run:

```sh
python3 -B tools/test-initramfs-root.py QEMU_KERNEL_BUILD QEMU_INITRAMFS_BUILD \
  --output NEW_TEST_DIRECTORY
```

The runner rejects physical-headset kernel/initramfs variants and mismatched
builds. It boots an independent QEMU with a newly created, writable regular-file
disk. The fixture requires PID 1 and the QEMU device-tree identity before any
power-off operation. The passing root case verifies OverlayFS, reads the expected
root identity, writes and syncs a file, checks zero block-device writes and
powers off. The runner also compares the entire disk's checksum before/after.
The missing-root case must report the absent device and perform a controlled
power-off without changing its backing image.

`--vendor-ramdisk PATH` additionally tests the vendor-prefix/generic-initramfs
concatenation used by the Android v4 container contract. This exercises Linux's
archive handling; QEMU does not execute the Quest bootloader or use its DTB.

September 11 evidence:

- `output/headset-initramfs-v3/` and `headset-initramfs-v4/`: 272 matching modules;
  both initramfs files are 29,686,256 bytes with SHA-256
  `3a99b84de69b6ed5dfb4f49e4c7aaf84dcfb6634c64efa553ad5122c0e6246f8`.
- `output/headset-initramfs-qemu-v4/`: independently built against the QEMU
  variant with the same module exclusivity checks.
- `output/initramfs-root-v5/`: final fixture and concatenated vendor prefix;
  normal boot and missing-root power-off with unchanged writable backing disks.
- `output/quest3-boot-assembly-v1/`: one-off AOSP v4 reference roundtrip and Linux
  assembly. The stock boot/vendor payloads reproduce byte-for-byte. Extracting
  the Linux artifacts returns the exact selected kernel, initramfs and patched
  base DTB. All 13 reference overlays exactly match compiled overlays and retain
  their original order. The generated files contain no copied AVB/GKI signatures.
- `output/quest3-boot-assembly-v2/`: the maintained [Quest assembler](quest-boot-assembly.md)
  independently reproduces all three v1 Linux artifacts byte-for-byte.
- `output/runtime-volatile-v1/`: the complete clean Fedora/Monado/Proton/FEX
  userspace boots through the new QEMU initramfs and actual vendor CPIO prefix.
  Native and Windows OpenXR rendering, both-hand action-driven eye captures,
  Windows CPU execution and normal shutdown pass in 112 seconds. The 8 GiB root
  image is attached read-only and its full checksum remains unchanged. This is
  a separate QEMU run with no network; it does not test the SteamVR dashboard.
  Its console preserves the automated pixel-check results; images created in
  the RAM overlay were not exported before shutdown or newly reviewed visually.

Earlier failures are retained: the PID-1 timeout launch, a test's incorrect
assumption that the lower mount remains visible after switch-root, missing
dracut emergency handling, and a temporary module-path layout rejected by the
strict archive check. The final builder uses standard `/lib/modules` staging.

September 12: `output/kernel/quest3/linux-build-v10/` and `qemu-abi-v10/`
export the complete DMA-buffer synchronization builds: Image, configuration,
272 matching modules and 14 device trees for each variant. Their kernel images
match the previously tested incremental builds. Module extraction, architecture,
release and hash checks pass; `depmod` reports no unresolved symbols. The SDE
module is updated in both variants; the other module hashes and device trees
match the respective previous full builds.

`output/headset-initramfs-quest-v4/` and `headset-initramfs-quest-qemu-v6/`
contain those matching modules and all 48 ADSP firmware files. The diskless
`output/quest-startup-dmabuf-v1/` readiness case boots the new QEMU package,
loads six actual vendor modules, completes 96 QRTR/QMI requests and powers off
cleanly in 2.568 seconds. ADSP, PMIC and USB hardware state is modeled; no DSP
firmware executes. `output/kernel-startup-audit-v1/` records input/artifact checks,
physical/mismatched-root refusals and 123 passing host tests. Root handoff with
these new artifacts still requires a newly assembled, matching root image.

The maintained assembly command is an offline container builder. It is not an
installer or updater. It uses reference firmware `52433670036000520`, which
differs from the recorded headset. Boot acceptance, authenticated recovery,
physical drivers, firmware/calibration, display, tracking and full VR interaction
remain unverified. These artifacts are not flash-ready.
