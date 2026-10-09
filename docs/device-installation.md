# Device installation requirements

Updated October 9, 2026. ArmadaVR has offline kernel, driver and virtual-runtime
tests, but no validated Quest installation or recovery procedure.

## Current Quest 3

A read-only ADB check finds an authorized USB-connected Quest 3 (`eureka`):

| Property | Reported value |
|---|---|
| Incremental build | `52083180032000520` |
| Android | 14 |
| Kernel | `5.10.237-g16c343ceed81` |
| Verified Boot | `green` |
| Flash lock / vbmeta state | `1` / `locked` |

These properties do not establish root, permission to boot another kernel, or a
usable restore path. Earlier reports inspected build `52083180031500520`;
exact-build assumptions must be checked against the current unit.

## Offline preparation completed

The exact-build stock OTA now authenticates against the release certificate
copied read-only from the stock headset. Payload hashes and the extracted boot
set's AVB integrity checks pass; see [boot-set verification](boot-set-verification.md).
The current table's DTBO index 8 selects Eureka PVT1.1. The stock kernel enables
module versioning and enforced Clang CFI, so a loader built for a nearby release
cannot be considered compatible merely by changing its version string.

The pinned Fuguquest target's two carrier modules and init configuration match
the authenticated OTA byte for byte. Its four relative kernel offsets match
the extracted stock symbol table; credential and SELinux offsets match the
kernel's BTF layouts. The readable device injection library and configuration
match the OTA, and library offsets agree with the target. These are static
compatibility checks, not evidence that the exploit works on this unit.
The [runner](https://github.com/Henry1887/fuguquest/blob/2324ce262e674504ad41ec82abcda3bf09dd01e6/rust/src/orchestrate.rs)
also changes persistent Android settings and only warns on a build mismatch.
Any device test needs strict identity/binary guards and explicit restoration of
settings and temporary files; the upstream defaults are unsuitable as an
unattended installation workflow.

The owned-file primitive has now passed on the current Quest: one byte of a
disposable 256-byte fixture was changed, the full file was checked, and the byte
was restored. Partial-allocation tests also verified explicit IPsec API release
and removal of the owned worker, socket and files. The original early process
exit was an uncaught Android API error; compiling against Android stubs exposed
the unsupported `Files.readString` call. Kernel security-association enumeration
and taint remain unreadable, so API release is not independent proof of their
kernel state. This test did not obtain root or load a module.

The exact stock service and complete compiled SELinux policy identify the module
loader as a root process in `init-insmod-sh`, rather than PID 1's `init` domain.
Module initialization runs synchronously in that caller. The policy permits
module loading from `vendor_file` but does not permit this domain to traverse or
write `shell_data_file` output under `/data/local/tmp`. Android logging is allowed;
the current headset also exposes its kernel log buffer read-only. Logging a new
inventory result through that buffer has not been tested on hardware.

An isolated QEMU fixture now loads the unchanged authenticated stock policy and
uses its normal executable-label transitions. With Clang CFI and SELinux
enforcement retained, 16 source-built module initialization attempts verify a
bounded device-tree memory read, an injected allocation-failure path, a missing
property and denied output access. Each returns a deliberate error, leaving no
resident module; caller credentials remain unchanged and normal driver exit is
not called. The read-only fixture filesystem retains its checksum. Evidence is
in `output/quest-policy-20261008-v1/` and
`output/quest-inventory-vm-20261008-v9/`. This uses the 5.10.246 QEMU kernel, not the
stock 5.10.237 kernel; loading a new module adds the expected out-of-tree taint.

The existing trigger still modifies shared executable mappings and restarts a
tracking service. It has no verified protocol to prevent concurrent execution
during replacement and restoration. File readback alone does not prove code
quiescence or instruction-cache visibility. That gap blocks running the trigger
on the headset even though the isolated inventory callback now passes.

The inventory callback now passes on the unchanged stock kernel in QEMU, as
described below. Full loader compatibility, the live reserved-memory map,
independently usable recovery and custom-boot acceptance remain open.
The signed stock package is not a backup of both slots or unit calibration.

The read-only device probe records whole-disk sysfs capacity, logical block size
and SCSI topology when accessible. Linux sysfs `size` uses 512-byte units even
when the logical block size is 4096 bytes. The current Quest permits topology
reads for `sda` through `sdf`, but denies their capacity and block-size reads;
those dimensions remain unknown. A readable SCSI path does not establish
Firehose LUN numbering or provide a GPT backup.

Inspect the selected stock device tree before designing a RAM boot layout:

```sh
python3 -B tools/inspect-boot-memory.py SELECTED_DTB --output NEW_REPORT.json
```

The authenticated current-build PVT1.1 tree has a zero-size `/memory/reg`
placeholder, 35 fixed reservations and 13 dynamically placed reservations.
The bootloader supplies the actual RAM banks; Linux chooses additional dynamic
allocations. The inspector retains unresolved nodes and rejects malformed,
overflowing or overlapping RAM ranges and invalid FDT reservation maps. Its
output does not establish a free staging arena. Obtain the live device tree,
allocator reservations and peripheral ownership before choosing Quest 3 loader
addresses; do not reuse the Quest Pro constants from the public loader.

## Research that changes the plan

**Temporary custom boot is now a concrete research route.**
[Quest-KeXec](https://github.com/YusufOzmen01/quest-kexec/tree/e61edbd7fbdf6dc4976a979c61cc545e04738422)
loads a custom kernel from rooted Android. Its published implementation is tested
only on Quest Pro. The [porting guide](https://github.com/YusufOzmen01/quest-kexec/blob/e61edbd7fbdf6dc4976a979c61cc545e04738422/HOW_TO_PORT.md)
requires device-specific memory staging, interrupt, SMMU, USB and firmware
handoff work. Audit and port the loader for Quest 3 before considering execution.
Separate its RAM-only boot path from installation helpers that write storage.
This route does not unlock the bootloader or establish persistent flash safety.
Its [return script](https://github.com/YusufOzmen01/quest-kexec/blob/e61edbd7fbdf6dc4976a979c61cc545e04738422/tools/return-android.sh)
depends on the target kernel's `/proc/qkx_bootdone`; the uninstaller requires
working rooted stock Android. Neither establishes independent recovery from a
failed Quest 3 handoff. Verify return on this unit before executing a new kernel.

The compiled Quest Linux Image also differs from that loader's assumed geometry:
its ARM64 header declares a zero text offset, little-endian execution and 4 KiB
pages. The loader assumes `0x80000`. Under the [ARM64 boot ABI](https://www.kernel.org/doc/html/v5.10/arm64/booting.html),
the entry address is the declared offset plus a 2 MiB aligned base. Inspect the
actual Image independently of its Android container:

```sh
python3 -B tools/inspect-boot.py KERNEL_BUILD/Image --arm64
```

`--load-address ADDRESS` optionally checks that arithmetic and extent overflow;
it does not establish available RAM. The prior physical and QEMU Images have
2,404 file bytes beyond their declared image size, matching the cached ELF's
orphan `.eh_frame` section at `_end`. The public loader's preparation checks
reject those Images. The maintained [linker patch](../patches/kernel/linux-userspace/0003-arm64-place-cfi-unwind-data.patch)
retains that section in read-only memory and preserves alignment of the early
page tables. An isolated QEMU kernel relink passes both thermal shutdown cases
and all 11 DMA-buffer synchronization checks, including 256 mapping-lifetime
cycles. Clang CFI remains enabled and the thermal test module loads successfully.
The file is now 39,358,976 bytes, with the same 41,025,536-byte declared RAM
requirement and no trailing section. Prior artifacts are preserved; a complete
physical build now also passes: `linux-image-layout-20261008-v1` exports the
5.10.246 kernel, 272 AArch64 modules and 14 device trees. Its Image is 38,947,328
bytes within the declared 40,566,784-byte RAM extent, with retained unwind data
and aligned early page tables. The matching initramfs contains ADSP firmware
from the authenticated current-build OTA; unsigned current-reference container
roundtrips pass. See [boot assembly](quest-boot-assembly.md). Quest boot acceptance
remains unverified. The loader's geometry and bounded-copy assumptions still
need a Quest-specific implementation.

**The current build has a matching public root target.**
[Fuguquest's target file](https://github.com/Henry1887/fuguquest/blob/2324ce262e674504ad41ec82abcda3bf09dd01e6/targets/q3_52083180032000520.json)
names `52083180032000520` and its kernel release. This replaces the earlier
unmatched IonStack target as the relevant source to inspect. A published target
is not a successful test on this unit, and root remains separate from unlock.

**Original Valve components can now be inspected.** Valve's public
[Lepton v3.0.5 source](https://gitlab.steamos.cloud/frame-public/lepton/-/tree/8be10c8a86ed2a6de1c6a2d6587994e957a35515)
includes launcher, host-library, OpenXR-layer and Android image build sources.
The [official recovery index](https://steamdeck-images.steamos.cloud/recovery/)
now lists Steam Frame `20260922.5153644-0.3.0` images. Offline extraction obtained
native ARM64 SteamVR client/server/compositor binaries and the Android ARM64
client. [ABI, native server and virtual-controller checks](steamvr-arm64.md) pass
on the Quest QEMU transport kernel; native compositor presentation and physical
Quest portability remain unverified. The
[Podman/FUSE writable-mount lifecycle](android-containers.md) also passes on the
unchanged Quest test kernel. A separate authenticated Steam installation now
supplies Lepton's matching payload; [Android boot, Binder services, context
persistence, software APK rendering and network provisioning](android-containers.md#acquired-lepton-and-actual-android-boot)
pass with four version-specific integration patches. Normal APK launch and baked
restart also pass with retained data, including a saved counter of 2 that becomes
4 after further input. A synthetic Steam compatibility launch also preserves
that data through default cleanup without retention flags. Actual games, clean
init shutdown, Android XR and hardware graphics remain unverified. Frame images and its
QDL programmer target Frame hardware.

**Standalone game compatibility can be studied on stock Android.**
[GameNative's Quest XR releases](https://github.com/utkarshdalal/GameNative/releases)
and [OpenComposite build](https://github.com/GameNative/opencomposite) provide a
Windows/OpenVR-to-OpenXR path under Wine. The latter embeds the bridge per game
process and has no shared `vrserver`. GameNative's [Windows XR transport](https://github.com/utkarshdalal/GameNative/blob/2963d9942d97fa1e4a8640df0e053cb3df1f1134/app/src/main/cpp/xrimmersive/xr_windows_transport.cpp)
provides ARM64EC, Android HardwareBuffer/dma-buf and fence-handling patterns worth
reviewing. Treat it as a game-compatibility reference;
it does not supply the requested SteamVR dashboard.
[GamePort](https://github.com/gameport-project/gameport-app) instead handles
Steam's Android game builds and controller remapping. Neither demonstrates a
replacement Quest OS. Stock-runtime tests could establish useful controller and
performance baselines before replacing tracking and display services.

**Pico Neo 3 Pro and Pro Eye remain separate hardware bring-up targets.** The current
[more-picohaxx-tool device list](https://github.com/chaixshot/more-picohaxx-tool/blob/c182399937107dd290cbc1ec2fdb06116af83a9d/README.md)
now reports Neo3 support on firmware 5.11.2 and below. That improves the consumer
Neo3 lead; it does not identify a validated Pro/Pro Eye configuration or establish
matching recovery hardware and firmware for those devices.
The public Neo3 kernel is 4.19.81 and lacks the vendor board DTS. Frame's actual
systemd 257.7 package is a userspace compatibility candidate for that kernel;
see the [firmware comparison](steam-frame.md#october-8-firmware-comparison).
Identify each unit's firmware, stock device trees and calibration before choosing
its kernel or boot path. Consumer Neo3 or Pico4 unlock reports do not establish
enterprise-SKU compatibility. The [Pico firmware assessment](pico-firmware.md)
recommends postponing the offered 5.9.9.0 update until that inventory is available,
and distinguishes normal signed upgrades from EDL downgrade and recovery.

## Work required before installation

The October 8 offline module audit found a concrete stock-ABI mismatch: all
twenty dependencies of the passing VM inventory fixture exist in the current
stock kernel, but eleven version CRCs differ, including `module_layout`. The
fixture therefore cannot be treated as a stock-compatible module. On October 9,
isolated genuine Kbuild compilations reproduce all twenty stock CRCs when given
the stock configuration, while the VM configuration reproduces all twenty
original fixture CRCs. The same cached source and compiler are used in both
arms, with CFI and module versioning retained. This explains the eleven checksum
differences through configuration, without altering any version records.
Evidence: `output/quest-stock-abi-compile-20261009-v4/`; the preceding failed
assembly-target invocation and narrower experiments are preserved.

This is interface-checksum agreement, not a stock-compatible module or a full
stock kernel reproduction. The matching named Android Clang 14.0.7/r450784e,
build 8508608, has now been acquired from the
[pinned AOSP prebuilt](https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+/5538393cf30cb39da6d53730a9b85f36958230da).
All 220 selected files match upstream Git blob identities, and the compiler,
linker and their runtime dependencies execute in the isolated build environment.

The unchanged inventory fixture built with that compiler passes sixteen real
module-load cases with ThinLTO and sixteen with full LTO on the source-built
5.10.246 VM kernel. Each run retains the authenticated stock SELinux policy in
enforcing mode, unchanged credentials, failed-init cleanup and unchanged
read-only fixture data. These modules retain the VM's genuine version records;
they are not stock-compatible modules. A separate wrong-type callback test
produces the expected CFI panic before its target executes. The bad call must
be in an ordinary instrumented helper: this vendor tree marks `__init` bodies
with `__nocfi`, so an indirect call inside initialization is not a CFI rejection
test. No kernel protection was changed.

With the captured stock configuration and full LTO, the same compiler also
produces twenty-two selected structure sizes and member offsets matching stock
BTF, including the module entry/checker fields, task credentials and PID.
Evidence: `output/quest-inventory-exact-compiler-20261009-v3/`,
`output/quest-inventory-exact-full-lto-20261009-v1/`,
`output/quest-cfi-negative-20261009-v2/` and
`output/quest-stock-layout-exact-compiler-20261009-v2/`. Earlier failed fixtures
are preserved. These results establish the tested compiler/VM combination and
selected layouts. Meta's public 5.10.237 source candidate still lacks a proven
mapping to this stock build.

### Exact stock-kernel execution

The authenticated, unchanged `5.10.237-g16c343ceed81` Image now boots in an
isolated diskless QEMU guest. Its complete loaded image is hashed before guest
execution. A small initramfs and host-side QMP log capture avoid the stock
kernel's lack of QEMU serial and virtio disk drivers; no kernel patch is needed.
The initial userspace boot and guest-initiated shutdown pass.

The inventory module is also built with the captured stock configuration,
normalized for the public source tree, and the matching Android compiler.
Kbuild compiles twelve real exporter objects; a relocatable
LTO link and unmodified `modpost` generate their version records. All twenty
module imports match the stock audit without edited CRCs or forced loading.
The module still uses newer public headers and explicitly accepts only the
QEMU device tree, so it is not a headset deployment artifact.

On the actual stock kernel, sixteen module loads now pass with the unchanged
stock SELinux policy enforcing: four each for bounded memory inventory,
allocation failure, missing property and denied output access. Each returns its
expected error, leaves no resident module and preserves caller credentials.
The protected output bytes remain unchanged, and the guest shuts down normally.
A separate wrong-type callback test reaches its armed marker and then panics
with a CFI failure before the target executes. The kernel's CFI protections
remain enabled.

Evidence: `output/quest-stock-qemu-20261009-v3/`,
`output/quest-stock-inventory-build-20261009-v1/`,
`output/quest-stock-inventory-vm-20261009-v3/` and
`output/quest-stock-cfi-vm-20261009-v1/`, each with independent validation.
Earlier harness failures are retained. These tests establish the inventory
callback's stock-loader execution and cleanup in a VM. Safe privileged entry,
live memory and DMA ownership, complete handoff-module compatibility, hardware
drivers, accepted Quest boot and independent recovery remain unverified.

### Remaining installation gates

1. **Establish exact-device boot and restoration.** Retain the authenticated
   stock OTA and verified boot artifacts matching the current build. Preserve
   the partition maps, both slots and calibration, and prove that the proposed
   restore method works for this device. Review the matching root implementation
   separately from any bootloader capability.
2. **Audit and port temporary boot.** Build the loader module against the exact
   running stock kernel ABI. Implement Quest 3 memory and peripheral handoff,
   bound input sizes, and test offline failure paths. The first physical target
   should be a minimal RAM-only kernel/initramfs with USB diagnostics and a
   verified return to stock Android, under a separate device-execution plan.
3. **Finish physical kernel and driver support.** Validate the selected panel
   and board overlay, UFS, USB, ADSP services, firmware loading, GPU and display
   fences, audio, Wi-Fi, battery/charging, fan and thermal shutdown. Existing
   compilation and simulated-sensor results do not establish hardware operation.
4. **Implement tracking and controllers.** Integrate synchronized cameras and
   IMU streams, device calibration, 6DoF prediction, distortion and display
   timing. Validate both controllers' pairing, poses, every required input,
   haptics, recentering, disconnect and reconnect behavior.
5. **Resolve native runtime integration.** Native virtual-device input and
   Vulkan sharing pass on the Quest test kernel. Resolve the native compositor
   direct-display crash, then test dashboard interaction and fresh Windows rendering. Keep
   FEX/Proton for game compatibility and preserve the existing translated
   SteamVR baseline for comparison. Reuse applicable source changes while
   retaining Quest-specific KGSL, panel and firmware support.
6. **Complete Lepton integration offline.** Matching rootfs, sysbake and xattrs
   are now preserved. Android boot, framework Binder services, context isolation,
   data persistence, APK installation/rendering/input and network provisioning
   pass. A test APK now retains saves and its package through the default
   compatibility-tool lifecycle. Verify actual game saves, clean init shutdown, Android OpenXR and
   Android/Linux graphics buffer and fence sharing. FUSE handles the writable
   views; native kernel rootless OverlayFS remains unsupported.
7. **Finish repeatable VR acceptance.** The [complete current root](headset-root.md)
   and its QEMU handoff tests now pass. Repeat Windows rendering on a fresh
   SteamVR backend, and verify both-hand stock dashboard
   selection, scrolling, navigation, menu, close/reopen and focus transfer.
   Diagnose renderer exits and allocation failures; test game launch, audio,
   runtime restart and bounded soak. Measure sustained frame times and power on
   real hardware when device testing is authorized.
8. **Design installation and rollback last.** Preserve the vendor partition
   layout and boot chain. Select persistent installation only after accepted
   boot and recovery have been demonstrated. Verify updates, rollback,
   power-loss behavior and failed-boot recovery with the exact chosen layout.

Armada's package/build infrastructure and FEX packaging remain reusable. Its
ROCKNIX-dependent ABL and handheld `/KERNEL` updater layout do not establish a
Quest boot route. See [board support](headset-bringup.md) and
[boot-container assembly](quest-boot-assembly.md). Moving to Holo Core is optional;
a distro change does not remove the device-driver or boot requirements.

Quest 3 is the current physical development priority. Its first boot milestone
is a RAM-only Linux USB/console boot with demonstrated return to stock Android.
Pico development is paused while the Pro Eye's return from EDL remains unresolved;
see the [Pico firmware baseline](pico-firmware.md). Native-runtime and Android
startup work remain in scope. Flashing cannot currently be recommended, and no
installation can be described as eliminating all bricking risk.
