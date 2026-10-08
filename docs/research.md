# Quest / Pico SteamOS feasibility

For the October 8 update and current implementation priorities, see
[device installation requirements](device-installation.md). The findings below
retain their original investigation dates.

Research checked on 2026-09-09. Device firmware and hardware behaviour have not
been measured. This project currently develops the Linux userspace in a virtual
headset. It does not contain a Quest or Pico flash image.

## What is possible now

An ARM64 gaming and VR userspace is feasible. A dependable replacement OS for a
retail Quest 3 remains blocked first by its boot chain, then by hardware bring-up
and tracking. Root access alone does not remove either problem. A headset port is
more than compiling SteamOS for a related Snapdragon chip.

Quest 3 remains the primary hardware target. Neo3 Pro is a candidate for an
earlier minimal Linux boot experiment if its exact firmware and recovery checks
establish a shorter path. Pro Eye needs separate eye calibration and runtime
support. Neither Pico has been proven safe to flash in this project; see the
[current firmware and boot-access assessment](pico-firmware.md).

## Hardware and the Snapdragon comparison

| Device | Relevant hardware | Consequence for this port |
|---|---|---|
| Quest 3 | Snapdragon XR2 Gen 2, Adreno 740, internal UFS | Related GPU generation to Snapdragon 8 Gen 2; requires its own board support and boot path |
| Neo3 Pro | Snapdragon XR2, 6 GB RAM, 256 GB storage, 3664×1920 LCD, 72/90 Hz | Older platform; potential recovery advantages need exact-device verification |
| Neo3 Pro Eye | Snapdragon XR2, 8 GB RAM, 256 GB storage, additional eye tracking | Keep separate from consumer Neo3 and Pico4 hardware profiles |
| Steam Frame | Snapdragon 8 Gen 3; ARM64 SteamOS | Valve controls its board, firmware and complete VR integration |

Pico specifications come from the [manufacturer's Pro/Pro Eye specification sheet](https://business.picoxr.com/us/products/neo3-pro-eye/specs).
Qualcomm documents the XR2 Gen 2's XR-specific GPU, camera and tracking capabilities
in its [platform brief](https://docs.qualcomm.com/doc/87-73689-1/87-73689-1_REV_A_Snapdragon_XR2_Gen_2_Platform_Product_Brief.pdf).
Meta explicitly identifies Quest 3's GPU as Adreno 740 in its
[GPU pipeline documentation](https://developers.meta.com/horizon/documentation/native/android/po-advanced-gpu-pipelines/).

The useful part of the 8 Gen 2 comparison is shared Qualcomm/Adreno technology.
It is not evidence of identical CPU topology, peripheral addresses, panel timing,
firmware or device trees. Meta's source identifies the Quest platform as
**anorak**, with **eureka** board overlays. A Steam Frame or SM8550 phone image
cannot be substituted for these files.

## Root and unlock evidence

### Quest 3

The [FreeXR CVE-2025-21479 implementation](https://github.com/FreeXR/eureka_panther-adreno-gpu-exploit-1)
provides kernel memory access and temporary privilege escalation through Adreno
KGSL. Its [wiki explicitly distinguishes this from a bootloader unlock](https://github.com/FreeXR/eureka_panther-adreno-gpu-exploit-1/wiki).
The authors warn that modifying verified boot/bootloader partitions can leave a
headset unrecoverable through ordinary USB tools, and report that public EDL
reflashing is unavailable because of authentication requirements. A backup is
insufficient if the device refuses the programmer needed to restore it.

There is newer work: [FreeXR's current inventory](https://github.com/FreeXR/exploits)
lists CVE-2026-43499, and [Singularity](https://github.com/Lumince/singularity)
implements a newer root workflow. The latter lists exact tested incremental
builds, including Quest 3 build `52345320035400520` at the time inspected. Its
root-on-boot feature is not evidence that boot ROM/ABL will accept an unsigned
replacement kernel. Compatibility must be checked using the device's actual
incremental build and the exploit's current implementation; a security-patch date
alone is not a reliable exploitability test.

The [older Quest bootloader unlocker](https://github.com/darknight1050/quest-bootloader-unlocker)
targets Quest 1/2 firmware from May 2021. It does not establish a Quest 3 unlock.
[FreeXR's bootloader replica](https://github.com/FreeXR/eureka-bootloader-replica)
is a research model, not a Qualcomm hardware emulator or demonstrated unlock.
Already-unlocked engineering units are a separate case from retail units.

**Finding:** no verified public retail Quest 3 unlock plus dependable restore
procedure was found in the inspected sources. This is an evidence boundary, not
a claim that an unlock can never be developed.

### Pico Neo3 Pro / Pro Eye

[more-picohaxx](https://github.com/264312431/more-picohaxx) and the
[documented tool fork](https://github.com/chaixshot/more-picohaxx-tool) describe a
route involving EDL, an engineering ABL/devinfo, and an unlock token. The fork
now claims generic Neo3 support on firmware 5.11.2 and below. This supersedes its
earlier Neo3-unconfirmed status, without establishing Pro/Pro Eye compatibility.
It also specifies different programmers for different memory hardware. These are
community tools, not a vendor promise that either of these two enterprise
headsets can always be recovered.

Pico's official [splash customization documentation](https://sdk.picovr.com/docs/SplashCustomize/chapter_two.html)
mentions Neo3 Pro/Pro Eye and a splash unlock operation. That narrowly scoped
feature is **not** evidence of an unlocked kernel/AVB boot chain.

Before any unlock attempt: obtain the exact SKU, firmware, board and memory
identity; verify a matching programmer can perform read-only operations; preserve
all UFS LUN partition tables, both boot slots, vendor firmware and device-specific
calibration; then demonstrate a supported restore path. Do not borrow Pico4 ABL,
DDR settings or recovery images based only on the Snapdragon family. Unlocking
can erase userdata. Relocking an altered image is not a recovery procedure.

## Drivers: useful source exists, but a complete headset port does not

Meta publishes an [official Quest 3 Linux kernel branch](https://github.com/facebookincubator/oculus-linux-kernel/tree/oculus-quest3-kernel-master).
The inspected commit `dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f` is Linux 5.10.246.
Its [eureka board directory](https://github.com/facebookincubator/oculus-linux-kernel/tree/dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f/arch/arm64/boot/dts/oculus/eureka)
contains BOE/JDI/Sharp panel variants, camera, audio, regulators, sensorlock, fan
and thermal configuration. The tree also contains `drivers/gpu/msm/kgsl*` and
`arch/arm64/configs/vendor/oculus_eureka_defconfig`. These are concrete starting
points, not a validated mainline device tree or complete redistributable firmware
bundle. Match the source and overlays to the actual unit and firmware.

| Subsystem | Reuse opportunity | Remaining proof/work |
|---|---|---|
| CPU, memory, interrupts, UFS, USB | Qualcomm and vendor kernel support | Boot exact board; preserve UFS layout and USB recovery |
| GPU | Mesa Freedreno/Turnip, vendor KGSL | Correct kernel backend, firmware, allocation and synchronization; actual Vulkan rendering |
| Display | Vendor DSI/panel configuration | Panel revision, timing, low persistence, DRM presentation, distortion and synchronization |
| Head tracking | Monado abstractions and open tracking algorithms | Synchronized IMU/camera streams, calibration, prediction, robust 6DoF |
| Controllers | OpenXR actions, device interfaces | Pairing, radio protocol, camera tracking, haptics, reconnect behaviour |
| Audio and networking | Vendor drivers and firmware | Correct routing, DSP/codec loading, suspend and reconnect |
| Power and thermals | Board-specific regulator/thermal/fan source | Battery/charging safety, sustained load, suspend and wake |
| Pico eye tracking / Quest passthrough | Vendor Android runtime works on stock OS | No verified replacement Linux implementation for these exact units |

Mesa's [Freedreno documentation](https://docs.mesa3d.org/drivers/freedreno.html),
[A7xx changes](https://docs.mesa3d.org/relnotes/24.3.0.html), and
[kernel backend options](https://chromium.googlesource.com/external/github.com/Mesa3D/mesa/+/464e8aaff4704166aee07cbf140c43bc9b227121/meson_options.txt)
show real reusable graphics work. Turnip supports both MSM and KGSL paths, but
an Android Vulkan library or working game renderer does not automatically provide
Linux display scanout or a VR compositor. Android Bionic libraries and glibc
programs cannot simply be mixed. Mainline MSM DRM and downstream KGSL require
different integration. A7xx support does not certify every headset revision.

Follow-up research located the official, archived
[ByteDance Neo3 kernel release](https://github.com/bytedance/neo3-kernel), pinned
at `9cacc1c1356dd32a048f4cb13bc84af402bd3326`, Linux 4.19.81. It contains VR-specific
driver/configuration changes but omits the vendor DTS directory referenced by
its build. Exact Pro/Pro Eye source matching and dependable recovery remain
unverified. See [headset bring-up](headset-bringup.md) for the source inventory,
Armada ABL/boot-format comparison, and the implemented Quest DT build.

## Valve's ARM stack and what we can reuse

Valve documents three distinct paths on [Steam Frame](https://partner.steamgames.com/doc/steamhardware/steamframe/compatibility):
Windows applications through Proton, x86 instructions through FEX, and Android
applications through the Lepton container. FEX forwards supported graphics API
calls to native ARM libraries. None of these components supplies Quest tracking
or unlocks a bootloader.

There is now public OS groundwork. Collabora's July 17, 2026
[Holo Core announcement](https://www.collabora.com/news-and-blog/news-and-events/building-an-arch-linux-aarch64-port-for-holo-core.html)
provides ARM64 Arch-derived sources, packages and a development container created
with Valve for Steam Frame. The inspected repository alias resolves to
`mash-20251118.3`; it exposes Mesa 25.2.7, OpenXR 1.1.53 and software Vulkan, plus a
rootfs. Monado was not present in the inspected extra-package listing. This is a
preview package foundation, not a universal Steam Frame recovery image.

Concrete entry points:

- [Holo Core source](https://gitlab.steamos.cloud/holo/holo-core-aarch64-preview)
- [Holo Core package snapshot](https://holo-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3/)
- `registry.gitlab.steamos.cloud/holo/holo-core-aarch64-preview/base-devel`
- [Steam's public ARM64 beta manifest](https://client-update.steamstatic.com/steam_client_publicbeta_linuxarm64), verified reachable; client version `1788652215` during this pass
- [Steam Runtime 4 ARM64 artifacts](https://repo.steampowered.com/steamrt4/images/4.0.20260415.225012/)

The client manifest mixes architectures. The checksum-verified
`steam_linuxarm64.zip.eca3384aa92d366d0c994f5000963191e9fc8e72` contains an
**ELF32 i386** legacy bootstrap at `ubuntu12_32/steam`. The separate
`bins_linuxarm64_linuxarm64.zip.dc817d33f8308815bf4fde6a3cb61fd4529728c8`
contains **ELF64 AArch64** `steamrtarm64/steam`, `steamclient.so`, `steamui.so`,
`steamwebhelper` and other native libraries. Its client requests
`/lib/ld-linux-aarch64.so.1`; all its direct ELF dependencies resolve in the lab.
Package names alone are insufficient to select an architecture. The optional lab
installer now fetches 17 pinned native/resource packages and the offline sandbox
has rendered the native client's sign-in screen. This does not establish login,
Steam Runtime, Proton or game operation; see the validation record.

The inspected ARM64 `steamwebhelper.sh` also hardcodes `taskset 0x7c`. That CPU
mask fits neither every ARM machine nor a two-vCPU QEMU guest. Docker's CPU quota
keeps the host CPU numbering, so it works in this host's container test. The
interactive VM removes that hardcoded mask and has rendered the native client's
sign-in UI. The runtime launcher and architecture-specific diagnostic helpers
still need integration.

[Proton's source](https://github.com/ValveSoftware/Proton/tree/5b89db940e0ebe3a137a6009a3589232fe084c09)
supports an ARM64 build host and `--target-arch=arm64`. Its documentation explicitly
says the resulting build cannot run under an x86 Steam client emulated by FEX.
[FEX 2604](https://fex-emu.com/FEX-2604/) and the
[Proton release notes](https://github.com/ValveSoftware/Proton/releases)
document the ARM64EC integration. This is a distinct configuration from running
an entire x86 userspace and conventional x86 Proton under FEX.

[Steam Runtime documentation](https://github.com/ValveSoftware/steam-runtime)
identifies Runtime 4 as the current native-game target and the runtime for Proton
11+. Use matching ARM64 client, compatibility-tool and runtime architectures;
verify Vulkan and OpenXR library/IPC boundaries. A native Linux OpenXR application
is the simplest first acceptance target. Next come a native Steam client, an x86
Linux program through FEX, and a specific Windows VR title through matching
Proton. Each needs its own proof. Lepton does not imply that Meta store services,
DRM or platform extensions work unchanged.

The sibling Armada repository already packages Fedora ARM64, FEX, Mesa and gaming
sessions. This lab follows that Fedora ecosystem for iteration and can carry the
same application tests onto a Holo Core image later. RPMs cannot be installed into
the Arch preview as if they were native packages. No sibling repository was
modified for this prototype.

## Virtual development and lower-risk hardware work

[Monado's simulated and qwerty/remote drivers](https://monado.freedesktop.org/developing-with-monado.html)
exercise OpenXR without a headset. QEMU `virt` exercises ARM64 Linux boot and
userspace. Neither models XR2 peripherals, AVB, UFS recovery, Meta camera DSPs,
radio protocols or proprietary tracking. A VM passing cannot justify flashing.
The [Meta XR Simulator](https://developers.meta.com/horizon/documentation/native/xrsim-getting-started/)
is useful for application-level Meta API tests, not for bootloader/kernel work.

[WiVRn](https://github.com/WiVRn/WiVRn) lists Quest3 and Pico Neo3 clients. It offers
a practical interim hardware path: retain the stock Android compositor/tracking
and stream from Linux. The exact Neo3 Pro/Pro Eye behaviour still requires
measurement. Streaming is useful incremental testing, but is not a replacement
OS and is not presented as the requested final result.

## Internal storage and implementation order

The absence of an SD slot changes installation and recovery, not the CPU
architecture problem. Treat UFS as a multi-LUN Android device with slots, verified
partitions, protected state and calibration, not a blank PC disk.

1. Native OpenXR tracking/rendering and FEX acceptance tests now pass in
   containers and an ARM64 VM. A separate persistent desktop VM also renders the
   ARM64 Steam client and virtual VR scene. Next integrate matching Steam
   Runtime 4/Proton components and test an application launch in that VM; this
   work does not depend on headset unlocking.
2. Collect each headset's inventory without root, reboot or writes. Identify
   firmware/overlay/panel differences and the exact available recovery process.
3. On a demonstrably unlocked and recoverable device, prefer an in-memory boot
   before altering boot partitions. `fastboot boot` and kexec support are
   hypotheses until confirmed. Temporary kernel root does not prove either.
4. For initial persistent userspace, consider a filesystem image under existing
   userdata or an Android-hosted container. Android encryption, mount access and
   kernel/driver ABI are additional constraints. This retains the vendor boot
   chain but is not yet standalone SteamOS.
5. Bring up display and thermals, then tracking/controllers, then graphics and
   native OpenXR on hardware. Reuse exact-device calibration without publishing it.
6. Carry the VM-tested Steam/Runtime 4/Proton ARM64 stack onto the working
   hardware platform and measure individual games. Only then design an internal
   installer and rollback process.
   An A/B scheme is useful only if boot firmware can actually select and recover
   both images. Preserve factory recovery and calibration throughout.

This is a hardware-porting project with research gates, not a short repackaging
job. The unlock/recovery gate has no credible fixed completion estimate. Once
resolved, a display-only Linux boot and a usable standalone 6DoF gaming headset
are substantially different milestones.
