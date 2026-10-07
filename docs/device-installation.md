# Device installation requirements

Updated October 7, 2026. ArmadaVR has offline kernel, driver and virtual-runtime
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

Exact loader source/config/toolchain/CFI compatibility, the live reserved-memory
map, independently usable recovery and custom-boot acceptance remain open.
The signed stock package is not a backup of both slots or unit calibration.

## Research that changes the plan

**Temporary custom boot is now a concrete research route.**
[Quest-KeXec](https://github.com/YusufOzmen01/quest-kexec/tree/e61edbd7fbdf6dc4976a979c61cc545e04738422)
loads a custom kernel from rooted Android. Its published implementation is tested
only on Quest Pro. The [porting guide](https://github.com/YusufOzmen01/quest-kexec/blob/e61edbd7fbdf6dc4976a979c61cc545e04738422/HOW_TO_PORT.md)
requires device-specific memory staging, interrupt, SMMU, USB and firmware
handoff work. Audit and port the loader for Quest 3 before considering execution.
Separate its RAM-only boot path from installation helpers that write storage.
This route does not unlock the bootloader or establish persistent flash safety.

**The current build has a matching public root target.**
[Fuguquest's target file](https://github.com/Henry1887/fuguquest/blob/2324ce262e674504ad41ec82abcda3bf09dd01e6/targets/q3_52083180032000520.json)
names `52083180032000520` and its kernel release. This replaces the earlier
unmatched IonStack target as the relevant source to inspect. A published target
is not a successful test on this unit, and root remains separate from unlock.

**Original Valve components can now be inspected.** Valve's public
[Lepton v3.0.5 source](https://gitlab.steamos.cloud/frame-public/lepton/-/tree/8be10c8a86ed2a6de1c6a2d6587994e957a35515)
includes launcher, host-library, OpenXR-layer and Android image build sources.
The [official recovery index](https://steamdeck-images.steamos.cloud/recovery/)
now lists Steam Frame `20260922.5153644-0.3.0` images. Inspect their native
SteamVR libraries, executables, ABI and dependencies offline. The image payload
has not been extracted here; native runtime availability and Quest portability
are not yet verified. Frame images and its QDL programmer target Frame hardware.

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

**Pico remains a separate target.** The current
[more-picohaxx-tool device list](https://github.com/chaixshot/more-picohaxx-tool/blob/c182399937107dd290cbc1ec2fdb06116af83a9d/README.md)
now reports Neo3 support on firmware 5.11.2 and below. That improves the consumer
Neo3 lead; it does not identify a validated Pro/Pro Eye configuration or establish
matching recovery hardware and firmware for those devices.

## Work required before installation

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
5. **Resolve native runtime integration.** Inspect the official Frame image
   before choosing a native Valve runtime or Monado-based game bridge. Keep
   FEX/Proton for game compatibility and preserve the existing translated
   SteamVR baseline for comparison. Reuse applicable source changes while
   retaining Quest-specific KGSL, panel and firmware support.
6. **Complete Lepton integration offline.** Verify the original source and image
   inputs, inspect the real mount lifecycle, and resolve the vendor kernel's
   rootless OverlayFS failure. Test Android boot, Binder transactions, shutdown,
   an openly licensed APK, and Android/Linux graphics buffer and fence sharing.
7. **Finish repeatable VR acceptance.** Build a complete fresh image, repeat
   Windows rendering on a fresh backend, and verify both-hand stock dashboard
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

The useful next development milestone is a reviewed Quest 3 temporary-boot
design plus native-runtime inspection. Flashing cannot currently be recommended,
and no installation can be described as eliminating all bricking risk.
