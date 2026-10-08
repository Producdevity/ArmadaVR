# Pico Neo3 Pro and Pro Eye firmware and boot access

Checked October 8, 2026. Both enterprise models are development targets; Quest 3
remains the priority. Per-unit installed builds, board revisions, firmware
regions and secure-boot variants have not been inventoried.

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
| Firmware and recovery | Current-build OTA authenticated; exact root target audited. Both-slot/unit backups and independent restore remain | Per-unit inventory, authentic stock package, matching programmer, partition/calibration backups and independent recovery all remain |
| Temporary custom boot | Quest3-specific loader ABI/CFI, live RAM allocation and peripheral handoff remain | Stronger public unlock lead; exact enterprise unlock persistence and accepted custom boot remain untested |
| Kernel and board description | Vendor kernel, modules and selected overlays compile; Linux userspace patches and QEMU ABI tests pass | Official 4.19.81 source is pinned, but its vendor board DTS is missing; no exact enterprise board build exists |
| GPU, display and platform I/O | KGSL/Turnip and display integration compile; actual panel/GPU, DSP, audio, radios, storage and power acceptance remain | Adreno650/display and the same platform-I/O categories need an exact board port and hardware acceptance |
| Tracking and controllers | Syncboss/IMU protocol research and virtual actions exist; calibrated camera/IMU 6DoF, pairing and physical input/haptics remain | Camera/IMU transport, calibration, 6DoF and controller integration remain; Pro Eye adds eye cameras, calibration and runtime support |
| Installation | No accepted boot/recovery or persistent installer | No accepted boot/recovery or persistent installer |

The [official Neo3 kernel](https://github.com/bytedance/neo3-kernel/tree/9cacc1c1356dd32a048f4cb13bc84af402bd3326)
includes vendor graphics and platform code but omits the board DTS directory;
the [source profile](../profiles/kernel/pico-neo3.json) refuses that incomplete
DT build. Frame's actual systemd 257.7 is a userspace candidate for 4.19;
the current Fedora/systemd 259 environment needs a newer kernel or a different
userspace baseline. See the [Frame firmware comparison](steam-frame.md#october-8-firmware-comparison).

Both targets share the remaining native SteamVR compositor, stock Library
interaction, renderer/shutdown reliability, Lepton graphics/XR and clean
shutdown, complete-image, game, audio and sustained-performance work. Existing
virtual acceptance is retained in [VR milestones](vr-milestones.md).

A minimal Linux USB/console boot with a demonstrated return to stock could be
an earlier Pico milestone if recovery and unlock checks pass. Prefer the ordinary
Pro for that experiment because it excludes the Pro Eye's extra eye-tracking
dependency; verify its separate memory and calibration assumptions. A usable
standalone VR system still needs the large display/tracking/controller port.
Public unlock access alone does not establish that Pico reaches that milestone
sooner. Estimate further implementation only after actual boot and sensor/display
access establish which vendor components can run under Linux.
