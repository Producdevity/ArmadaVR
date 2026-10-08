# Headset boot and board-support bring-up

Checked 2026-09-10. No Armada installation has been attempted on a physical
headset. The current QEMU image exercises ARM64 userspace and a simulated headset;
it is not a Qualcomm board image. This document addresses the driver, device-tree
and ROCKNIX ABL concerns separately.

The complete headset kernel and driver port is an implementation deliverable.
Compilation, userspace integration, driver functionality and recoverable boot
are separate acceptance steps within that deliverable. Missing support remains
work to implement, not a reason to remove it from scope. Physical flashing waits
for the VR software milestones and the exact-device recovery checks.

## Establishing the exact device state

`tools/probe-device.py --serial SERIAL --output output/DEVICE-inventory.json`
reads an already authorized ADB device. It does not start root, invoke `su`,
reboot, unlock or write to the headset. The report records the exact incremental
build and fingerprint, bootloader/AVB properties, SKU/revision, selected DTBO
indices when exposed, board IDs, SELinux state and the ADB shell UID. The
presence of a `su` executable is recorded separately from current privileges.

Missing or denied reads remain unknown. Conflicting lock properties are reported
as conflicting; neither a property nor a discovered root executable proves that
an unsigned kernel can boot. Android properties can also be spoofed. The tool
does not infer exploit compatibility by comparing build numbers or patch dates.

The connected Quest was inventoried read-only on 2026-09-10. It reports:

| Observation | Value |
|---|---|
| Incremental / Android | `52083180031500520` / 14 |
| Running kernel | `5.10.237-g16c343ceed81`, built April 21, 2026 |
| Security patch | `2026-01-05` |
| Board / SoC / GPU | `eureka` / `anorak`, `SXR2230P` / `Adreno740v3` |
| Reported boot state | locked, green Verified Boot, active slot `_a` |
| ADB privileges | UID 2000, SELinux Enforcing, no `su` on the shell path |
| Selected DTBO index | `11`; the exact installed DTBO table is not available |

Its readable `/proc/config.gz` was copied and decompressed locally. Both
`CONFIG_KEXEC` and `CONFIG_KEXEC_FILE` are disabled, so ordinary stock-kernel
kexec is not a demonstrated alternate boot route. All 268 loaded module names
are present in the 272-module Linux build, but that build uses a different
kernel version and has 131 configuration differences. Module-name coverage
does not establish ABI compatibility. No driver was loaded or changed.
SELinux denied reads of the stock driver binaries, board IDs and panel modes;
these remain unknown. Evidence is under `output/device-inventory/`.

A second read-only boot-state check returned `flash.locked=1`, `vbmeta.device_state=locked` and Verified Boot `green`.
The supplemental boot-argument reads were denied; fastboot did not list a device
while it remained in Android. These properties do not establish whether an undisclosed root method was used;
root and bootloader unlock require separate evidence. No reboot or unlock attempt was made. Evidence:
`quest3-20260910-unlock-recheck.json` and
`quest3-20260910-unlock-bootargs.txt` in that inventory directory.

The supplemental `quest3-20260910-unlock-context.json` confirms ADB is enabled
and the OEM-lock service exists, but its dump is empty. No known root-tool name
matched the package-name query. These observations do not rule out hidden or
custom root/unlock changes and do not establish acceptance of an unsigned image.

The exact-build archive URL returned HTTP 404. Meta's published April snapshot
[`063d9fb81e3f8d953b86cff0dd7adb4b7432b48e`](https://github.com/facebookincubator/oculus-linux-kernel/tree/063d9fb81e3f8d953b86cff0dd7adb4b7432b48e)
is Linux 5.10.237, but its version alone does not prove it matches the installed
build. Its source overlay order puts PVT1.1 at index 11; this is a candidate
mapping until the unit's installed table is obtained. The newer reference OTA
below is not this headset's recovery image.

The September inspection of the upstream Singularity README names Quest 3 incremental
`52345320035400520`; it does not validate our newer reference OTA or establish a
retail bootloader unlock. At that inspection the Pico tool left Neo3 unconfirmed;
its October source now claims Neo3 support. See the [current Pico assessment](pico-firmware.md)
for the remaining Pro/Pro Eye evidence and recovery requirements.
Pico4 recovery instructions using a Neo3 Pro engineering ABL are tested on
Pico4-family hardware, and cannot serve as a verified Pro/Pro Eye restore route.
Sources: [Singularity](https://github.com/Lumince/singularity),
[Pico unlock tool](https://github.com/chaixshot/more-picohaxx-tool),
[Pico4 recovery scope](https://pico4.wiki/guides/root/02-unroot/).

The [IonStack Quest adaptation](https://github.com/F-19-F/IonStackQuest3)
documents a default for a different kernel/build and requires adaptation for
other firmware. Its [issue 7](https://github.com/F-19-F/IonStackQuest3/issues/7)
specifically mentions this unit's kernel without a resolved compatibility
result. Neither source proves that this exact build can be rooted or accept an
unsigned boot image. No exploit, reboot, unlock or partition write was attempted.

The [exact-build follow-up](quest3-boot-route.md) checks the current source commits,
unresolved exact-kernel issue and Meta updater entry point. It found no verified
unsigned-boot and recovery route for this unit.

## Stock thermal and sensor policy

Read-only ADB copied four readable configuration files from the exact installed
build: `/vendor/etc/thermal_info_config.json`, `/odm/etc/sensorservice.cfg`,
`/vendor/etc/powerhint.cfg` and `/vendor/etc/vr_runtime_power_mitigation.json`.
They are retained with checksums under
`output/device-inventory/quest3-stock-52083180031500520/thermal-sensors/`.
The thermal file's SHA-256 is
`c9301ff553f65255f7e204da9988345a0ecf1a5305099889db43ce1ab5473cd4`.
The sensor configuration requests an 800 Hz IMU and 25 Hz magnetometer; it does
not disclose packet decoding or calibration. No policy was written to the unit.

The stock thermal policy describes 53 sensors and six cooling devices. Its
severity-6 shutdown limits include battery virtual temperature at 65°C, surface
virtual temperature at 60°C, and 26 CPU/GPU/DSP/SoC zones at 115°C. The original
Linux device trees contain the fan/throttling mappings, but lack these 28
shutdown trips. Their only critical trips are six PMIC thresholds at 145°C.
Android shutdown policy therefore cannot simply be omitted when replacing its
userspace. The severity mapping is defined by
[Android's thermal interface](https://android.googlesource.com/platform/hardware/interfaces/+/refs/heads/main/thermal/aidl/android/hardware/thermal/ThrottlingSeverity.aidl).

`profiles/quest3-thermal.json` records the exact source identity and thresholds.
`patches/kernel/linux-userspace/0002-eureka-critical-thermal.patch` adds them as
kernel critical trips, preserving their stock hysteresis. The patch keeps every
existing trip and cooling mapping. `tools/inspect-thermal.py` resolves sensor and
cooling-device phandles, rejects disabled or missing providers and invalid fan
states, and checks each required shutdown limit has a polling fallback.

All 14 base/merged board DTBs compile and pass the audit, with 102 thermal zones,
28 matched shutdown limits and all original cooling mappings preserved per tree.
The fan states remain 0/3700/4500/5000/5500 RPM; battery fan trips remain
45/49/53°C. Evidence: `output/thermal-dt-audit-v1/` and
`output/thermal-dt-shutdown-v1/`. This proves the configuration and references;
it does not validate physical temperature readings, fan RPM, calibration,
shutdown timing or sustained-load safety.

The diskless thermal regression loads a simulated sensor/cooling device into the
actual QEMU variant of the vendor kernel. It tests 45°C activation, 2°C cooling
hysteresis, the below-critical boundary and 65°C shutdown. Its first enabled
run passed the cooling transitions but exposed an inherited Android setting:
`CONFIG_STATIC_USERMODEHELPER=y` with an empty helper path suppresses Linux
usermode helpers. Orderly power-off never ran; the existing 100 ms emergency
fallback shut the VM down instead. The Linux configuration now disables that
static-helper override, preserving the emergency cutoff. The regression also
checks cutoff when the power-off helper deliberately stalls. Neither test has a
disk, network or physical device, and both fixture and runner reject non-QEMU
targets. Run it with:

```sh
just test-kernel-thermal output/kernel/quest3/qemu-abi OUTPUT_DIRECTORY
```

## What the existing Armada image actually boots

The sibling Armada checkout currently pins ROCKNIX ABL **1.1.8** and kernel
**7.2.3**. Its implementation is traceable through:

- `../armada/abl/release.env` and `../armada/abl/releases.tsv`: approved ABL payloads;
- `../armada/build_files/40-vendor-system-files.sh`: installs SM8250/SM8550/SM8650/SM8750 payloads;
- `../armada/system_files/usr/lib/armada/supported-dtbs`: 22 handheld DTBs, no Quest/Pico;
- `../armada/post_process/make-bootimg.sh`: gzip kernel plus concatenated DTBs,
  initramfs, Android header v0, 2048-byte pages, placed at `/KERNEL` on FAT;
- `../armada/system_files/usr/libexec/armada/armada-bootimg-update`: regenerates
  that same layout when the OSTree deployment changes;
- `../armada-packages/kernel/scripts/build-kernel.sh`: kernel.org source plus
  Armada patches, configuration and handheld DTS files.

Consequently, selecting “SM8550” or “SM8250” does not add headset support. The
current kernel, DTB selection, boot container and installer must all agree with
an actual board. Generic Qualcomm UFS, USB, interrupt, power and MSM DRM support
already exists in Armada, so “every single driver is missing” overstates the
problem. Its complete headset board support is missing.

[ROCKNIX's ABL documentation](https://github.com/ROCKNIX/abl) explicitly excludes
locked consumer devices. The public
[release workflow](https://github.com/ROCKNIX/abl/blob/0e755e154874f7e874919ab7e080935c1444df68/.github/workflows/release-abl.yaml)
checks out a separate `ROCKNIX/LinuxLoader` repository using a deploy key; that
repository was not publicly accessible during this investigation. The public
ABL repository contains release plumbing, not the full loader implementation.
The [1.1.8 release](https://github.com/ROCKNIX/abl/releases/tag/v1.1.8) identifies
loader commit `22ac43cef216dfd2caefef26cd46db0d8e0d4d71`.
Porting that loader requires access to its matching source and a device whose
boot chain accepts it. A filename containing `signed` proves neither condition.

## Quest 3 source baseline

Meta's official source is pinned at
[`dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f`](https://github.com/facebookincubator/oculus-linux-kernel/tree/dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f),
Linux **5.10.246**. These are vendor-kernel bindings, not mainline bindings.

The [Eureka build list](https://github.com/facebookincubator/oculus-linux-kernel/blob/dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f/arch/arm64/boot/dts/oculus/eureka/Makefile)
contains one base tree and 13 overlays spanning development, EVT, DVT, PVT and
Sloane variants. The compiled base describes six CPU nodes, `qcom,anorak`,
`oculus,eureka` and Qualcomm platform ID 549. These source observations are not
a measurement of the inspected unit and do not identify its selected overlay.
In particular, do not substitute a Snapdragon 8 Gen 2 phone DTB or copy Armada's
eight-core SM8550 affinity settings into this six-core source platform.

[`build.config.anorak`](https://github.com/facebookincubator/oculus-linux-kernel/blob/dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f/build.config.anorak)
sets boot header **v3**, base address `0x80000000` and 4096-byte pages.
This differs from Armada's v0 `/KERNEL` packaging. It is a published build
configuration, not proof that every Quest firmware uses identical packaging.
[AOSP's vendor_boot format](https://source.android.com/docs/core/architecture/partitions/vendor-boot-partitions)
separates vendor ramdisk and DTB from the v3/v4 boot image. Inspect matching stock
`boot`, `vendor_boot` and `dtbo` artifacts before designing a replacement.

| Subsystem | Concrete source in Meta's pinned tree | What remains |
|---|---|---|
| GPU | `drivers/gpu/msm/kgsl.c`; `CONFIG_QCOM_KGSL`; DT `qcom,adreno-gpu-gen7-6-0` | Choose vendor KGSL or a mainline MSM port; prove Vulkan allocation, synchronization and presentation |
| Panels/backlight | `eureka-panel*.dtsi`; BOE/JDI/Sharp variants; BLU timing and power sequencing | Match the unit's panel and firmware; implement low-persistence presentation and distortion |
| Sensor MCU | `mcu/syncboss/syncboss_spi_core.c`, `syncboss_timesync.c`, UAPI `linux/syncboss.h` | Firmware, packet decoding, calibrated IMU/camera timestamps and a Monado hardware driver; transport is not SLAM |
| Cameras | `eureka-camera.dtsi`, vendor camera bindings and clocks | Sensor configuration, ISP/firmware, synchronized frames and calibration; the DTS alone supplies no 6DoF tracking |
| Thermal/fan | `eureka-thermal.dtsi`, `pwm-tach-fan.c`, virtual sensors | Preserve control loops and panel dependencies; validate thermal response before sustained load |
| Charging/IPD/sensor interlock | `cypd3177.c`, `as5510.c`, `sensorlock.c`, regulators | Port ABI dependencies and preserve device-specific firmware and protected GPIO constraints |

Driver paths in this table are under
[`drivers/staging/oculus`](https://github.com/facebookincubator/oculus-linux-kernel/tree/dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f/drivers/staging/oculus)
unless otherwise stated. The original subset supported source inspection; the
complete pinned Meta source now builds an ARM64 kernel and 272 modules with the
resolved Linux userspace configuration described below. These are vendor-kernel
builds, not ports to Armada's newer kernel. Compilation does not establish which
modules or firmware the inspected unit needs, or that its boot chain accepts them.

The [Freedreno/Turnip project](https://docs.mesa3d.org/drivers/freedreno.html)
provides useful graphics foundations. Reusing Mesa does not port the downstream
panel driver, sensor MCU or tracking runtime. Android firmware and HAL services
also remain separate dependencies from the GPL kernel source.

The complete source gives a more precise GPU match: `anorak-gpu.dtsi` identifies
`Adreno740v3`, chip ID `0x43050b00`. Its KGSL core requests `a740v3_sqe.fw`,
`gmu_gen70200.bin` and the `a740v3_zap` image. Those names identify dependencies;
the build does not supply firmware or verify its acceptance by the secure world.
At Mesa commit `9f955af118d0cdabae776d08e36f99f7aa92317b`,
[`freedreno_devices.py`](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/9f955af118d0cdabae776d08e36f99f7aa92317b/src/freedreno/common/freedreno_devices.py)
explicitly lists that same chip as `FD740v3` / Quest 3. Its
[`tu_knl_kgsl.cc`](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/9f955af118d0cdabae776d08e36f99f7aa92317b/src/freedreno/vulkan/tu_knl_kgsl.cc)
implements dma-buf memory and sync-file synchronization. The inspected sync type
does not implement opaque-FD import/export hooks and uses an emulated timeline.
Consequently, this source is a candidate Vulkan backend but does not establish
the opaque-FD semaphore path required by the tested SteamVR compositor.
Graphics synchronization is part of the port, alongside display presentation.
The two inspected Mesa files and hashes are cached under
`output/hardware-research/mesa-9f955af118d0/`; this is a specific source snapshot,
not a claim about every Mesa version or a driver run on the headset.

Syncboss's `syncboss_miscfifo.c` exposes `syncboss_stream0` and
`syncboss_control0`. The stream's `miscfifo_fop_read_many` can concatenate
complete records in one read. The UAPI contains v2/v3 headers, validity states
and signed timestamp offsets, followed by type/sequence/length packet framing.
A replayable adapter must preserve those boundaries and reject invalid timing;
it cannot treat these packets as calibrated poses. MCU payload decoding,
calibration, camera alignment and a Monado hardware adapter remain explicit
driver-port work. The FIFO decoder and replay tests below implement the first
part of that adapter's input path.

## Reference Quest OTA inspection, 2026-09-10

The public [Quest firmware mirror](https://cocaine.trade/) supplied reference
archive `q3_52433670036000520.zip`, 1,403,092,751 bytes. Range downloads retrieved
only the relevant boot, vendor and ODM partitions. Payload metadata hashes,
individual operation hashes and reconstructed partition SHA-256 values match
the archive's own metadata. The manufacturer's signatures and the complete
payload hash were not verified. This is a reference artifact, not authenticated
recovery media or a match to the inspected headset.

Its metadata identifies `eureka`, Android 14/API 34 and security patch
2026-06-03. Both `boot` and `vendor_boot` use **header v4**, with AVB footer magic;
the source build configuration's v3 value cannot be used as the packaging
contract. The vendor table contains one platform ramdisk fragment, no name,
and sixteen zero board-ID fields. Its base DTB is 696,565 bytes, SHA-256
`3a2d0102f9ae1c0daf434a9d6e7940c90bb64fc2f71c277d87671d10d7a319e1`,
exactly matching our compiled Eureka base. This does not identify the DTBO
selected by a particular unit. The stock kernel identifies itself as
`5.10.246-gd7102a837402`, built with Android Clang 14.0.7/r450784e.

Read-only inspection of the vendor ramdisk, `vendor_dlkm` and `odm_dlkm`
found **403 module instances and 272 unique module filenames**. The compiled
Linux headset build contains exactly those 272 names, with none missing or
extra. All inspected binaries are AArch64. Stock vermagic includes the above
Git suffix; ours is `5.10.246 SMP preempt mod_unload modversions aarch64`.
Name coverage is not module ABI compatibility, successful loading, firmware
acceptance or functioning hardware. Stock modules must not be mixed into the
new kernel on this evidence alone.

The stock kernel also contains its compressed configuration. Comparing its
resolved options with `linux-build-v3` finds 24 differences, all compiler/tool
identifiers, ThinLTO selection or the intentionally enabled Linux userspace
facilities. There are no driver-option differences in that comparison.
The config and full diff are retained as `stock-kernel.config` and
`output/reference-module-audit/stock-config-diff.json`. The later build adds the
filesystem/console requirements found by full-desktop testing; neither build is
a byte-for-byte reproduction of the stock kernel.

The reference vendor image includes `a740v3_sqe.fw`, `gmu_gen70200.bin` and
the `a740v3_zap` image family. ODM includes `syncboss.bin`, its application/SPL
images, `santana_manager.bin`, and controller firmware archives. These files
remain local research data under `output/`, outside the runtime images. They
were neither executed nor installed. The stock `fw_init.cfg` points to
`/odm/firmware/syncboss.bin`, a Syncboss streaming interlock, and a separate BLU
update protocol. Those paths describe vendor dependencies, not authorization
to run their firmware updater. Sensor configuration names an 800 Hz default
IMU rate; it does not supply the sensor packet layout or calibration.

The stock fstab confirms logical A/B system/vendor partitions and
hardware-wrapped file and metadata encryption for both userdata and `vision`.
It mounts `persist` separately; other configuration references calibration
there. Placing a Linux filesystem file in encrypted userdata does not make it
accessible to a replacement early-boot environment. A rootfs handoff must
preserve these dependencies and account for the actual partition/slot state.

Evidence: `output/reference-ota-52433670036000520/`,
`output/reference-firmware-52433670036000520/`,
`output/reference-odm-52433670036000520/` and
`output/reference-module-audit/module-comparison-complete.json`.
The maintained boot inspector reproduces section hashes and v4 fragment checks
from local images. Temporary range-download and filesystem-inspection scripts
are retained in `.agents/`; they are not installers.

## KGSL Vulkan userspace

`just build-turnip` builds Mesa 26.1.8's AArch64 Turnip driver with its KGSL
kernel backend, from the checksum-pinned source in `profiles/mesa.json`.
The source explicitly identifies the Quest 3's FD740v3 variant, chip ID
`0x43050b00`, in
[the GPU table](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.1.8/src/freedreno/common/freedreno_devices.py).
The build enables X11/Wayland WSI and runs the five upstream Freedreno
disassembler, instruction-delay, texture-layout and XML-include tests offline.

The clean `output/mesa/turnip-v2/` build passed all five tests in 84.42 seconds,
with a 4 GiB/two-CPU cap and 1,385,644,032-byte peak memory. The 15,558,728-byte
driver's SHA-256 is
`b08b72716e4c70d1203689a7fc54665be4ac62bf7ee4627a44d756865ce59aa9`.
Independent checks verified all four exported artifact hashes and AArch64 ELF
architecture. A loader trace confirms initialization of this library; without
`/dev/kgsl-3d0` it correctly reports no Vulkan device. No driver is installed
on the headset or host by this workflow.

The subsequent `turnip-fences-v2` build includes
`patches/mesa/turnip/0001-kgsl-fence-ownership.patch`. The original merge path
used an uninitialized descriptor when combining timestamps from different
queues, and exported the wrong object when combining a timestamp with a file
descriptor. Export failures could also produce a successful result with the
signaled `-1` sentinel. The patch preserves each dependency, propagates failures
and releases owned descriptors and submission allocations on error.

The build extracts the patched helper functions and runs 252 merge/fault cases
plus timestamp-wrap, empty-wait, failed-export and descriptor-lifetime checks
under AddressSanitizer and UndefinedBehaviorSanitizer. The original source
fails the regression. The patched code passes on macOS ARM64 and Linux ARM64;
the complete driver also compiles and passes all five Freedreno tests. Its four
exported artifacts and AArch64 architecture were independently verified.
Build time was 64.42 seconds, peak memory 1,445,490,688 bytes, with no OOM event.
Driver SHA-256: `a345bf75ad3ca3f1cabda880e1fb34a5597c0b6208cd3168d02e4b2058b1b011`.
Evidence: `output/mesa/turnip-fences-v2/` and `output/kgsl-fences-baseline.txt`.

These regression tests use a software descriptor/dependency model. They do not
execute GPU ioctls or prove physical display synchronization. The separate
opaque-FD/timeline bridge described below still requires GPU validation.

`0002-kgsl-wait-any.patch` corrects the separate CPU wait path. The original
iteration reads one pointer beyond the supplied array; an FD-only wait also
tries to export a timestamp through a null queue. Its `poll` result handling
reverses readiness and timeout, and its same-queue optimization selects the
latest timestamp instead of the earliest. The patch bounds iteration, handles
FD-only waits, preserves borrowed descriptors, checks allocations/exports and
reports actual polling results. The baseline array overrun is reproduced under
AddressSanitizer in `output/kgsl-waits-baseline-v3.txt`.

The maintained `tests/kgsl-wait-tests.py` extracts the driver helper and tests
it with real OS pipes and `poll`, covering readiness, timeout, delayed signals,
EINTR/EAGAIN retries, timestamp ordering/wrap, multiple queues and injected
allocation/export/poll failures. Both macOS ARM64 and the Linux ARM64 build
run it under ASan/UBSan. The pipe fixture checks CPU wait behavior and FD
ownership; it supplies no evidence about actual KGSL fence signaling.

The complete build with both patches, `output/mesa/turnip-fences-v3/`, passes
both sanitizer suites and all five Freedreno tests in 67.69 seconds. Peak
memory is 1,446,649,856 bytes with no OOM event. All four artifact hashes and
ARM64 ELF headers were checked independently. The resulting driver's SHA-256
is `4ff04fb1cc28346e6f2b2aee4045a4266847fed7d5e48308380a435dfe472cfa`.
A diagnostic copy of the original source with only its overrun/null-queue
hazards removed still fails the real-poll timeout assertion, isolating the
reversed result handling (`output/kgsl-waits-poll-regression.txt`).

An ARM64 C ABI comparison compiled the actual Mesa and Quest KGSL headers
separately. All 16 ioctl numbers used by this Mesa backend, 27 relevant structure
sizes and 115 member offsets match. The vendor ioctl table includes both
`GPUMEM_BIND_RANGES` and `GPU_AUX_COMMAND`. Results and source hashes are in
`output/hardware-research/turnip-kgsl/abi-result.json`. This checks the binary
interface layout; it does not execute ioctls or validate GPU behavior.

The upstream source audit identifies a SteamVR requirement:
[`vk_kgsl_sync_type`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.1.8/src/freedreno/vulkan/tu_knl_kgsl.cc)
implements sync-file import/export and an emulated timeline, without opaque-FD
semaphore callbacks. The upstream Quest display DRM driver also omits
`DRIVER_SYNCOBJ`.

`0003-kgsl-drm-syncobj.patch` now implements an opt-in bridge selected with
`TU_KGSL_DRM_SYNC=/dev/dri/renderD128`. It probes the actual DRM node for binary,
timeline and pending-wait support, then uses Mesa's DRM synchronization type.
Each submitted wait is transferred from its DRM timeline point to a real
`sync_file` consumed by KGSL. Actual KGSL completion is exported back through
a temporary binary DRM object into each requested output point. Empty submits
retain the native backend's last-queue-fence and merged-wait behavior. A failure
to publish completion after submission marks the Vulkan device lost; it cannot
silently report completed work. Without the environment variable, the native
KGSL path remains selected.

`patches/kernel/linux-userspace/0001-sde-syncobj.patch` enables the generic DRM
binary/timeline ioctls on the vendor SDE driver. This does not add a GPU scheduler
or make CPU signaling stand in for hardware completion: the userspace bridge
must import the actual KGSL fence. QEMU's separate kernel configuration enables
`CONFIG_SW_SYNC` and `CONFIG_DEBUG_FS` solely to test with real pending kernel
software fences. These debug facilities are not added to the headset profile.

The historical `output/mesa/turnip-drm-v5/` build passes all three
ASan/UBSan suites and all five Freedreno tests, but a September 11 audit found
that its final library compiled out the bridge because libdrm was disabled.
The corrected build is `output/mesa/turnip-drm-v7/`; see
[the build correction and actual-library checks](turnip.md). The old build also compiles
`tests/kgsl-drm-sync.c` against the exact bridge header and exports both artifacts.
The latter passed on the rebuilt QEMU ABI kernel using actual pending kernel
software fences. Checks cover binary/timeline round trips, cross-process opaque
handles, merged work that waits for both dependencies after its source FDs close,
future timeline publication before completion, descriptor ownership, 128 cleanup
cycles and invalid handles. The test does not execute the KGSL kernel driver.

`tests/kgsl-submit-tests.py` separately extracts the actual bridge submission
wrapper and exercises 39 cases under ASan/UBSan on macOS and ARM64 Linux. It
checks wait values/stages, multiple signal destinations, empty work, descriptor
ownership and allocation/import/export failures. Errors before submission must
prevent native submission; completion-publication errors after submission must
mark the device lost. DRM calls and native submission are modeled in this suite;
the kernel-fence test above supplies the separate real DRM evidence.

For an already running development VM booted with `qemu-abi-v5` or a newer
verified QEMU ABI kernel, the final fence test can be reproduced with:

```sh
python3 -B tools/vm_control.py output/desktop put \
  output/mesa/turnip-drm-v5/kgsl-drm-sync /var/tmp/kgsl-drm-sync-test
python3 -B tools/vm_control.py output/desktop exec \
  'mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug'
python3 -B tools/vm_control.py output/desktop exec \
  'chmod 755 /var/tmp/kgsl-drm-sync-test; /var/tmp/kgsl-drm-sync-test /dev/dri/renderD128'
```

The copy command refuses existing destinations. These commands address only
the development VM's local guest-agent socket. No physical GPU completion or
display fence has yet been verified. Scanout, panel sequencing, distortion,
tracking/calibration and thermal operation remain required too.

## Pico source correction

An official [ByteDance Neo3 kernel release](https://github.com/bytedance/neo3-kernel)
was found in this pass. It is archived, describes **SXR2130P / Android 10**, and
uses Linux **4.19.81**, pinned at `9cacc1c1356dd32a048f4cb13bc84af402bd3326`.
This corrects the earlier report that a vendor source package had not been located.

Its complete Git tree lacks `arch/arm64/boot/dts/vendor`, although its DTS
Makefile conditionally includes that directory. The release includes Kona
configuration files, KGSL/display code, and `pvr_kernel_config` switches for
camera wiring, PM8009 power hardware and 72/90 Hz display behaviour. It supplies
no identified Neo3 Pro or Pro Eye DTS in that missing directory. Neither the
release nor the community unlock documentation establishes exact enterprise-SKU
compatibility. The new source profile therefore refuses a Pico DT build.
The [current unlock-tool assessment](pico-firmware.md) supersedes the earlier
Neo3-unconfirmed status. Its generic Neo3 support claim does not validate either
enterprise SKU or this project's custom Linux image.

The old kernel also needs a userspace compatibility strategy. For comparison,
[current systemd upstream](https://github.com/systemd/systemd/blob/main/README)
requires at least Linux 5.10. A stock 4.19 vendor kernel cannot simply be assumed
to host a current Fedora/SteamOS stack; verify the actual packaged userspace and
backports, or port the board to a maintained kernel.

The actual desktop container currently contains `systemd-259.8-1.fc44` and
`glibc-2.43-8.fc44`, both AArch64. The
[systemd 259 requirements](https://github.com/systemd/systemd/blob/v259/README)
set a 5.4 minimum and 5.7 recommended kernel baseline, unlike the newer upstream
main branch cited above. Its libc ELF ABI tag names Linux 3.7.0. Quest's 5.10
source clears those nominal version floors; Pico's 4.19 source does not meet
the systemd 259 floor. This is package/ABI inspection, not userspace execution on
either vendor kernel. The exact output is `output/hardware-research/userspace-abi.txt`.

## Boot strategy and integration order

The working design is to keep the headset boot integration separate from the
Armada userspace. A stock ABL that demonstrably accepts a custom Android boot
image could load Linux without replacing ABL. That is a candidate route,
**not a confirmed capability of either headset**. Root, Android boot-image
unlocking, critical bootloader replacement and EDL recovery are distinct checks.
The [Quest root authors](https://github.com/FreeXR/eureka_panther-adreno-gpu-exploit-1/wiki)
explicitly say their GPU exploit does not unlock the bootloader. The newer root
work reviewed in [research.md](research.md) does not close that gap.

Implementation sequence:

1. **Done here:** pin actual board source, compile vendor DTs and overlays, retain
   selection IDs, and inspect Android boot formats offline.
2. Match exact firmware, stock boot headers, bootloader state, DT/DTBO selection,
   panel identity, module versions and recovery availability. Pro and Pro Eye
   remain separate targets. Inventory can proceed without root or reboot.
3. Establish a kernel baseline: reproduce the matching vendor build/configuration
   or create a reviewed mainline driver/DT port. Build modules and check userspace
   syscall/graphics compatibility before producing a boot candidate.
4. Implement a headset initramfs/rootfs handoff using the verified Android image
   format. Replace Armada's `/KERNEL` update/finalization path and exclude its
   handheld ABL updater and repartitioning installer from any headset target.
5. After the [VR software milestones](vr-milestones.md) and exact-device recovery
   gates pass, validate a supported temporary boot before planning persistent
   installation. No current tool attempts this.

Internal UFS is not a blank PC disk. An existing-userdata filesystem image might
avoid repartitioning, but encrypted userdata access and early mounting remain
unresolved. A dedicated internal layout needs a verified restore path and
preservation of all relevant LUNs, slots, vendor firmware and calibration.
An SD slot is not required by Linux; its absence removes one convenient recovery
and iteration option. No partition layout or ABL replacement is selected here.

## Reproduce the implemented work

Requirements: Python 3.11+, a C preprocessor (`cc`), and the `dtc` package including
`fdtoverlay`. This workflow does not require a VM, Android SDK, root, USB access or a complete kernel checkout.

```sh
just fetch-kernel quest3
just build-device-trees quest3
just fetch-kernel pico-neo3
just inspect-boot /path/to/local-stock-boot.img
```

The fetcher downloads a small checksum-pinned subset into
`output/kernel/PROFILE/source`, preserves licensing files and resolves the four
upstream include aliases within that directory. Quest has 143 pinned files;
Pico has 14. Downloads are limited to four at a time, with per-file size/time
limits. Modified cached files are rejected rather than replaced.

The DT build is offline and sequential. Each compiler invocation has a 60-second
timeout. It creates a base DTB, 13 DTBOs, 13 independently merged DTBs, per-artifact
JSON inventories, compiler logs and `build.json` containing provenance, commands,
tool versions and output hashes. This is not an Android `dtbo.img` partition
container. Overlay-root board IDs are **selection metadata**; `fdtoverlay` does
not copy those root properties into the base. Both inventories are retained so
that a merged tree's base model is not mistaken for its selected board revision.

Use a new output directory for another build:

```sh
python3 -B tools/kernel-source.py build-dt quest3 --output output/kernel/quest3/dt-next
```

The current verified run is `output/kernel/quest3/dt-verified/build.json`:
**27 artifacts built, all 13 overlay applications passed, 167 vendor DTC warnings
retained, no forced compilation**. This is neither a mainline DT schema pass nor
a hardware-boot result. A node enabled in the JSON means its DTS status and
ancestor statuses permit it; it does not mean a driver bound or hardware worked.

The boot inspector accepts regular files only. It reports v0–v4 boot or v3–v4
vendor_boot geometry and rejects malformed/truncated layouts. It never extracts,
repackages, signs or flashes. AVB footer magic is reported only as an observation;
no signature, bootloader acceptance or headset compatibility is verified.
Section hashes and v4 vendor ramdisk fragment names, types, ranges and board IDs
are retained. Fragment overlap and out-of-range entries are rejected.
AOSP `mkbootimg` generated nine synthetic fixtures for validation; their arbitrary
payload bytes are not executable headset kernels. Results are in
`output/boot-format-validation/results.json`.

No hardware driver operation, installer or SteamVR completion is claimed by
these offline results. Vulkan semaphore sharing now passes in the VM; actual
SteamVR presentation remains under investigation in [steamvr.md](steamvr.md).

## Complete vendor kernel and Linux userspace configuration

The Quest profile now pins the complete Meta source archive: 223,149,821 bytes,
SHA-256 `4f8bf49b25361856002718a25e4c725342ae5a814d7e70673ae79c671b0906e7`.
Its 87,298 entries expand to 1,259,168,451 payload bytes. Every regular file,
executable flag and symlink target can be checked against the archive. This is
separate from the small DTS source subset.

The full source must live on a case-sensitive filesystem. The Linux tree contains
both `xt_CONNMARK.h` and `xt_connmark.h`; extracting it onto the development Mac's
case-insensitive volume overwrote one. Verification rejected that tree and the
initial compilation was stopped. `output/kernel/quest3/full-source` and
`build-vendor` are invalid initial diagnostics, not build inputs or results.
The supported workflow uses the Docker volume `armada-vr-quest3-kernel`, with no
physical device passthrough. The archive cache and exported reports stay on macOS.

```sh
just build-kernel-tools
just configure-headset-kernel
just build-headset-kernel
```

`build-kernel-tools` installs Debian's native ARM64 Clang/LLVM 14, linker and
kernel build dependencies inside `localhost/armada-vr:kernel`. This differs from
[Meta's documented x86 AOSP toolchain](https://github.com/facebookincubator/oculus-linux-kernel/blob/dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f/README.meta.md).
A successful build with it is a source compilation baseline, not byte-for-byte
stock reproduction. The container base is digest-pinned; package repositories
are rolling, so reports record compiler, image and package identities.

The vendor build merges Meta's `oculus_anorak_defconfig` and
`oculus_eureka_defconfig`, exactly as that README specifies. Full LTO, CFI and the
shipped `vmlinux.profdata` sample profile remain enabled. Kconfig resolution is
recorded; the vendor fragments contain six assignments that no longer resolve
as requested, including removed symbols and different zswap defaults.

The Linux userspace variant adds `profiles/kernel/linux-userspace.config` and
`quest3-build.config` to those same vendor fragments. The vendor configuration disables `DEVTMPFS`, user
namespaces, IPC namespaces and the cgroup PID controller. The layer enables
device management, namespaces and process accounting, along with filesystem,
FEX and userspace input prerequisites. `SYSVIPC`/`POSIX_MQUEUE` are explicit
dependencies of `IPC_NS` in this kernel. Every requested userspace option must
survive Kconfig resolution or the command fails. This is a configuration check,
not proof that Steam or systemd boots on this kernel. See
[systemd's kernel requirements](https://github.com/systemd/systemd/blob/main/README).
The build fragment selects **ThinLTO** while retaining CFI and sample PGO.
The unmodified full-LTO link exceeded the 4 GiB cap twice, including with one
linker thread/partition, and also exceeded 6 GiB in `vendor-build-v4`.
Those failures remain recorded in `vendor-build-v2` through `vendor-build-v4`;
they are not successful vendor builds. The Linux variant
therefore deliberately differs in optimization mode. `PYTHON=python3` fixes an
earlier missing-interpreter failure in Meta's linker script without changing
upstream source. The linker uses one thread/partition in both variants.

Vendor firmware loading, panel presentation, camera/MCU protocols and runtime
driver integration still need implementation and validation.

Builds run offline with two CPUs, **6 GiB total RAM/swap**, 256 tasks and a
45-minute compilation timeout. Configuration-only checks default to 4 GiB.
`KERNEL_BUILD_MEMORY=4g` can lower the build cap, but this source's BTF generation
exceeded it even with one worker. `pahole` uses one worker and retains the
upstream enum64 compatibility flag. The successful 6 GiB run kept BTF enabled
and recorded no OOM kills. Use a Linux engine with sufficient memory; the tested
Docker VM has 8 GiB. Configuration and compilation use separate output trees
for the vendor and Linux variants. Source and build identity checks protect
incremental reuse; a new userspace fragment gets a separate cache directory.
Choose a new report directory to resume a compatible build or rerun a check:

```sh
just configure-headset-kernel output/kernel/quest3/linux-config-next
just build-headset-kernel output/kernel/quest3/linux-build-next
```

Each report includes the resolved configuration, its changes, command logs and
`build.json`. A completed compilation exports an ARM64 `Image`, device trees,
stripped module archive, module ABI/dependency/alias metadata and hashes.
Module firmware metadata is not an exhaustive inventory of runtime firmware
requests. It does not assemble `boot.img`, select an
overlay for the inspected device, include proprietary firmware or install anything
on a headset. The stock-ABL acceptance and recovery work remains in scope after
the prerequisite software validation.

The completed run is `output/kernel/quest3/linux-build-v3/build.json`:
**40,503,652-byte ARM64 Image, 272 AArch64 modules, one base DTB and 13 DTBOs**.
Source verification passed before and after compilation. `artifact-check.json`
verifies all 28 exported artifact hashes, module ELF architecture and vermagic,
and the presence of KGSL, SDE display, camera, Syncboss, fan, sensorlock and Wi-Fi
modules. The module metadata parser was corrected from colon-labelled output to
`modinfo -0`'s `NAME=value` fields; regeneration verified unchanged module hashes
and is explicitly recorded in the report.

`just build-vendor-kernel` remains available to attempt Meta's original full-LTO
configuration. On this development setup it is a failing diagnostic baseline,
not the normal path to the compiled Linux kernel above.

## Sensor transport replay

`tools/syncboss-replay.py` decodes saved **FIFO read data** using the v2/v3 driver
headers and type/sequence/length framing. It accepts regular files up to 64 MiB,
parses incrementally and writes optional JSONL records. It supports concatenated
and fragmented records, signed offsets and driver messages; invalid/error timing
states yield no usable offset. Sensor payload bytes remain opaque. Raw SPI
transactions and Android direct-channel buffers use different formats and must
not be passed off as FIFO captures.

```sh
just test-syncboss
python3 -B tools/syncboss-replay.py /path/to/capture.bin --records /path/to/new-records.jsonl
```

The test uses the already-fetched full source in the Linux volume. It verifies
that source, compiles `tests/syncboss_uapi_fixture.c` against Meta's actual UAPI
header, emits three synthetic records and checks the decoder's output. The
container has no network or device passthrough and is capped at 256 MiB/one CPU.
`output/syncboss/` contains the fixture, decoded records and summary. This passed
on ARM64 Linux; malformed/fragmented-input tests also pass on macOS.
It establishes the transport format, not real IMU decoding, sensor calibration,
6DoF tracking or a complete Monado headset driver.

[The tracking protocol follow-up](quest3-tracking-protocol.md) traces Meta's
packet-80 IMU routing and the distinct 104-byte direct-channel record format.
Its 64-byte payload contains a driver header and opaque MCU data; the union's
float declaration is not a calibrated sample schema. Sample layout, units,
sensor timestamp and factory camera/IMU calibration remain required inputs.

## Vendor-kernel QEMU userspace acceptance, 2026-09-10

`profiles/kernel/qemu-abi.config` adds PL011, generic PCI and virtio transports
to a separate build of the same pinned Meta kernel and Linux userspace config.
It does not replace the headset configuration or emulate Qualcomm peripherals.

```sh
just build-kernel-abi
just test-kernel-abi
```

The build in `output/kernel/quest3/qemu-abi-v1/` compiled 272 modules in 1,452.13
seconds, with a 6 GiB maximum, two CPUs and no OOM events. The build verifies
the source before and after compilation and records the virtual configuration
separately. The boot test verifies both image manifests and hashes, uses a
snapshot of the existing lab disk, has no network or physical passthrough, and
powers off automatically.

`output/kernel/quest3/abi-test-v1/result.json` passed in 17.27 seconds. Its boot
log shows Linux 5.10.246 running Fedora 44, 120 valid stereo/HMD/controller
pose samples, a rendered stereo image with 41,600 changed pixels and 701 colors,
and the x86-64 FEX test. The VM then unmounted its filesystems and powered off.
This closes the basic vendor-kernel/userspace execution gap for these tests.
It does not validate Quest display, KGSL, firmware, tracking, bootloader or UFS
installation. Later builds extended the userspace validation as follows.

The v4 QEMU build adds virtual consoles, automount, Zstd SquashFS, PL031 RTC and
CPU-managed DRM sync objects. The full signed-in development desktop boots with
correct UTC time and its FEX rootfs mounted. The clean Runtime 4/Proton image
passes native stereo, 600-sample controller actions, x86-64 CPU execution and
both Windows OpenXR Vulkan bindings on this kernel in 63 seconds, followed by
clean poweroff. See `output/runtime-vm-v4/acceptance-quest-kernel.txt`.
Native Vulkan binary/timeline opaque-FD cross-process tests also pass.

`patches/kernel/qemu-abi/0001-virtio-cpu-syncobj.patch` is scoped to the virtual
kernel. It exposes existing DRM core CPU synchronization; it does not connect
virgl GPU submissions or implement Quest KGSL synchronization. Patched source
is copied into a separate configuration/patch-identity cache, applied without
fuzz and fingerprinted before/after the build. The original source remains
verified unchanged. The 1,405.54-second v4 build used 6 GiB/two CPUs with no OOM.
Its 28 exported hashes and 272 module architectures pass independent checks in
`output/kernel/quest3/qemu-abi-v4/verification.json`.

FreeXR's public [bootloader replica](https://github.com/FreeXR/eureka-bootloader-replica)
models token verification in Python; it is not a virtual Quest board with panel,
GPU, sensor and UFS emulation. The [userdebug converter](https://github.com/Lumince/Quest_Userdebug_Converter)
targets userdebug-to-user firmware conversion, not an established retail unlock
path. Neither supplies the missing exact-device boot acceptance evidence.
