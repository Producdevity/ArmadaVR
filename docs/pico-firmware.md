# Pico Neo3 Pro and Pro Eye firmware and boot access

Checked October 8, 2026. Neo3 Pro Eye is the initial hardware bring-up target,
followed by Quest 3. The connected unit was identified physically as Pro Eye;
its generic Android model alone does not distinguish enterprise variants. Neo3
Pro requires its own inventory and calibration backups.

## Current Pro Eye baseline

The connected Pro Eye reports global/overseas 5.8.4.0, internal build
`c000_rf01_bv1.0.1_sv5.8.4.0_202310092224_neo3_b1977_user`, incremental
`smartcm.1696861506`, Android 10 and Linux `4.19.81-perf+`. It has about 8 GB
RAM, Adreno 650 v3, a `SHARP493` panel property and eye-tracking support property.
These are inventory observations, not physical driver acceptance.

The exact [global b1977 stock OTA](https://static.us-pui.picovr.com/5.8.4.0-202310092224-RELEASE-user-neo3-b1977-e4688d78c8.zip)
was authenticated against the public OTA certificate copied independently from
the running headset. Its SHA-256 is
`88669fe996510a879305d980ed4ba626c908ccc06ecead466393c3eadb58a8b6`.
The maintained verifier now supports its legacy `BLOCK` format and nonnumeric
incremental without weakening the default Quest A/B checks:

```sh
python3 -B tools/verify-ota.py STOCK_OTA.zip \
    --device-certificates STOCK_OTACERTS.zip \
    --expected-device PICOA7H10 --expected-build smartcm.1696861506 \
    --ota-type BLOCK \
    --expected-fingerprint 'Pico/A7H10/PICOA7H10:10/5.8.4.0/smartcm.1696861506:user/test-keys' \
    --expected-secure-boot-tag SE
```

This authenticates the archive and selected metadata. It does not execute or
validate the block updater, establish rollback acceptance, or prove restoration.
The package has an Android v2 boot image, six SoC base DTBs plus an RTIC data FDT,
and sixteen board overlays. Its signed VBMeta matches the boot and DTBO hashes;
the images' own `NONE` authentication headers do not authorize unsigned boot.
There is no standalone recovery image in this OTA, although VBMeta chains to
recovery. Per-unit recovery and calibration backups remain required.

Stock config enables KGSL, Binder, FUSE and OverlayFS, but omits SysV IPC,
user/PID/UTS namespaces, `binfmt_misc` and `devtmpfs`. It enforces module
signatures and has module versioning. A compatible custom kernel is needed for
the Linux userspace; stock modules cannot simply be reused with a changed ABI.
No storage mutation commands have been sent to the unit.

## EDL identity and partition backups

A serial- and USB-port-guarded software EDL entry and Sahara query succeeded on
the b1977 Pro Eye. The ROM reports MSM ID `0x000ce0e1`, OEM ID `0x013a` and
model ID `0x0ae8`. Its full 48-byte root-key hash matches the Pico root
certificate in the official engineering programmer. That root and attestation
certificate also match the authenticated stock XBL, XBL configuration and ABL.

The programmer was acquired from the
[official Neo3 engineering package](https://static.us-pui.picovr.com/SEKSA-pico_rls_neo3-mol-tob-pui-4.8.0-20220622-falconcv3-user-20221017-225305-32g-b1981.zip).
Its size is 675,320 bytes and SHA-256 is
`d05711c81d07e426ab44a25dd9f4a391c0ddcbe251a935babb2532ca84ebdce1`.
Signature, certificate-chain and segment hashes pass offline checks. ROM identity
matching does not prove programmer execution, storage reads, restore capability,
or acceptance of a custom boot image. No successful programmer transfer has
been observed yet.

The tested Sahara reset acknowledged the request but did not leave EDL, and
subsequent USB descriptor reads failed. Do not rely on this command as a proven
return-to-stock path. PICO documents a hardware reboot by holding Power for
[more than ten seconds](https://www.picoxr.com/cn/neo3/pdf/PicoNeo3UserGuide.pdf).
Independent cold EDL entry and restoration still need validation on this unit.
macOS also requires approval when the Pico first changes to its Qualcomm USB
identity; a pending accessory decision prevents libusb enumeration.

Validate acquired primary and backup GPT headers and entry arrays before using
their extents for partition acquisition:

```sh
python3 -B tools/inspect-gpt.py primary-header.bin primary-entries.bin \
    backup-header.bin backup-entries.bin \
    --sector-size LOGICAL_BLOCK_BYTES --total-sectors ACTUAL_LUN_LOGICAL_SECTORS
```

These are regular host files: each header occupies one logical sector; each
array contains all its complete sectors. The tool checks header/array CRCs,
locations, capacity, matching tables, partition GUIDs, bounds and overlap. It
reports duplicate labels as ambiguous rather than selecting one. It never
opens a device or repairs a table. Entry arrays are capped at 1 MiB.
The format follows the [UEFI GPT specification](https://uefi.org/sites/default/files/resources/UEFI_Spec_2_8_final.pdf).

Use independently observed LUN geometry. Linux sysfs `size` counts 512-byte
units; Firehose counts logical sectors. Convert through bytes before comparing
them. Synthetic 512/4096-byte-sector validation does not establish this unit's
geometry or a restorable backup. Per-unit recovery, persist, picocfg, sensor/eye
calibration and boot-chain acquisition remain necessary before persistent changes.

## Update decision

Postpone the offered 5.9.9.0 update until each unit has a read-only inventory and
matching recovery inputs. This is a preservation decision, not evidence that
5.9.9.0 disables the public unlock route.

[PICO's official OS page](https://www.picoxr.com/global/software/pico-os)
groups Neo3, Pro and Pro Eye together. Its embedded enterprise entry advertises
global 5.9.9.0 dated September 15, 2024, with two packages:

| Package | Build timestamp / number | Community-reported variant |
|---|---|---|
| [Primary download](https://static.us-pui.picovr.com/5.9.9.0-202409100009-RELEASE-user-neo3-b3003-0eb48d3eb2.zip) | `202409100009` / `3003` | `SEK` |
| [Alternate download](https://static.us-pui.picovr.com/5.9.9.0-202409100359-RELEASE-user-neo3-b3006-12c1d440db.zip) | `202409100359` / `3006` | `K` |

The variant assignments come from the original
[community firmware catalogue](https://pico.crx.moe/docs/picoos-research/version-table/#pico-neo-3-series).
It associates `SE` with `ro.secure.boot.tag=true` and `K` with user builds.
These packages have not been downloaded or signature-authenticated by this
project. A common version number is insufficient to select between them or
between global and Chinese firmware.

PICO's [Neo3 offline-upgrade instructions](https://sdk.picovr.com/docs/FAQ/chapter_ninepointone.html)
explicitly permit upgrades from lower to higher versions. They do not promise
arbitrary downgrades. The community catalogue describes a build-date check too.
The normal signed updater and an EDL partition rewrite are different operations.

PICO's [enterprise feature list](https://sdk.picovr.com/docs/FAQ/chapter_twentyfive.html)
also names customized ROM installation for Pro and Pro Eye. It does not publish
an unrestricted unsigned-kernel unlock procedure or establish how this project
would obtain an accepted customized ROM. Treat that as a separate vendor-supported
route to investigate before replacing critical boot-chain components.

Use the existing inventory tool with an explicit serial and a new report path:

```sh
python3 -B tools/probe-device.py --serial SERIAL --output output/pico-inventory-new.json
```

The report includes `ro.pico.tag`, `ro.secure.boot.tag`, `ro.oem.state`, the full
build identity and boot-state observations. Missing properties remain unknown;
they are not interpreted as an unlocked device. Pro and Pro Eye need separate
reports and calibration backups.

## What more-picohaxx-tool contributes

The inspected source is
[`c182399937107dd290cbc1ec2fdb06116af83a9d`](https://github.com/chaixshot/more-picohaxx-tool/tree/c182399937107dd290cbc1ec2fdb06116af83a9d).
Its README now claims Neo3 support through 5.11.2. This supersedes the earlier
Neo3-unconfirmed assessment. The number 5.9.9.0 falls within that claimed range,
but the README supplies no exact Pro/Pro Eye firmware test record.

The project provides useful references for EDL backup, GPT-derived partition
geometry, image extraction, unlock-state checks and restoring the original ABL.
Its unlock flow writes engineering ABL and devinfo, then unlocks and factory
resets the device. Its rollback flow replaces multiple boot-chain and system
partitions. Neither is a RAM-only experiment. The original Python explanation
describes testing on Pico4 and assuming similar Neo3 behavior; that historical
comment is narrower than the newer fork's support claim.

The manufacturing diagnostic recognizes Neo3 Pro and prints firmware-variant
checks. It is a separate menu action, not a mandatory package/device guard in
the rollback workflow. Do not treat its availability as verified compatibility.

[Issue 5](https://github.com/chaixshot/more-picohaxx-tool/issues/5)
documents failed Neo3/Neo3 Link downgrades after boot-chain writes. A wrong
`super` size was [fixed](https://github.com/chaixshot/more-picohaxx-tool/commit/603907b2440d3ff43267599adebf6298040ab2c6)
by reading actual partition geometry. Subsequent reports still needed external
EDL entry: one recovered with a 9008 cable, another through test points after
the cable failed. These are useful recovery reports for those devices, not
proof that either enterprise unit always accepts the same programmer or can be
restored without opening it.

Reuse the documented protocol and backup lessons after verifying their exact
device assumptions. Do not import its Windows automation or bundled firmware
into ArmadaVR's installer, or run its rollback flow merely to test access.

## Work remaining compared with Quest

| Workstream | Quest 3 | Neo3 Pro / Pro Eye |
|---|---|---|
| Firmware and recovery | Current-build OTA authenticated; exact root target audited. Both-slot/unit backups and independent restore remain | Connected Pro Eye inventoried and exact stock OTA authenticated; matching programmer, partition/calibration backups and independent recovery remain. Pro needs separate inventory |
| Temporary custom boot | Quest3-specific loader ABI/CFI, live RAM allocation and peripheral handoff remain | Stronger public unlock lead; exact enterprise unlock persistence and accepted custom boot remain untested |
| Kernel and board description | Vendor kernel, modules and selected overlays compile; Linux userspace patches and QEMU ABI tests pass | Official 4.19.81 source is pinned and stock DTB/DTBO acquired; vendor board DTS is missing and source/build compatibility is unverified |
| GPU, display and platform I/O | KGSL/Turnip and display integration compile; actual panel/GPU, DSP, audio, radios, storage and power acceptance remain | Adreno650/display and the same platform-I/O categories need an exact board port and hardware acceptance |
| Tracking and controllers | Syncboss/IMU protocol research and virtual actions exist; calibrated camera/IMU 6DoF, pairing and physical input/haptics remain | Camera/IMU transport, calibration, 6DoF and controller integration remain; Pro Eye adds eye cameras, calibration and runtime support |
| Installation | No accepted boot/recovery or persistent installer | No accepted boot/recovery or persistent installer |

The [official Neo3 kernel](https://github.com/bytedance/neo3-kernel/tree/9cacc1c1356dd32a048f4cb13bc84af402bd3326)
includes vendor graphics and platform code but omits the board DTS directory;
the [source profile](../profiles/kernel/pico-neo3.json) refuses that incomplete
source DT build. Authenticated stock trees provide board inputs, but do not
establish compatibility with the older public source. Frame's actual systemd
257.7 is a userspace candidate for 4.19;
the current Fedora/systemd 259 environment needs a newer kernel or a different
userspace baseline. See the [Frame firmware comparison](steam-frame.md#october-8-firmware-comparison).

Both targets share the remaining native SteamVR compositor, stock Library
interaction, renderer/shutdown reliability, Lepton graphics/XR and clean
shutdown, complete-image, game, audio and sustained-performance work. Existing
virtual acceptance is retained in [VR milestones](vr-milestones.md).

A minimal Linux USB/console boot with a demonstrated return to stock could be
an earlier Pico milestone if recovery and unlock checks pass. The connected Pro
Eye is the selected development unit; preserve its eye calibration even before
eye tracking is integrated. A usable
standalone VR system still needs the large display/tracking/controller port.
Public unlock access alone does not establish that Pico reaches that milestone
sooner. Estimate further implementation only after actual boot and sensor/display
access establish which vendor components can run under Linux.
