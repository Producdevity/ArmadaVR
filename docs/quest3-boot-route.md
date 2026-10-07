# Quest 3 exact-build boot and recovery status

For the October 7 update and current implementation priorities, see
[device installation requirements](device-installation.md). The findings below
retain their original investigation dates.

Checked 2026-09-10. This is a bounded follow-up to
[headset-bringup.md](headset-bringup.md), using current upstream metadata and
published source. No device command, exploit, reboot, unlock, update or partition
write was run during this investigation.

**No verified unsigned-boot and recovery route was found for the connected
Quest 3.** Its recorded incremental is `52083180031500520`, Android 14, kernel
`5.10.237-g16c343ceed81`; the latest local inventory reports locked/green Verified
Boot and shell UID 2000. Bootloader unlock has not been demonstrated. These
observations do not prove that every possible root method is absent. See the
read-only inventory evidence linked in [the device-state section](headset-bringup.md#establishing-the-exact-device-state).

A targeted refresh at 21:02 UTC on September 10 returns the same build,
`ro.boot.flash.locked=1`, `ro.boot.verifiedbootstate=green`,
`ro.boot.vbmeta.device_state=locked`, slot `_a` and shell UID 2000.
The command reads only identity and properties; its complete result is saved in
`output/device-inventory/quest3-boot-state-20260910T2103Z.json`.

## What the upstream refresh established

| Source inspected | Current concrete evidence | Consequence for this unit |
|---|---|---|
| [IonStackQuest3, commit `81faf794`](https://github.com/F-19-F/IonStackQuest3/tree/81faf7942576fd826bb097fb2b4fae76e090a103) | Latest commit dated 2026-08-06; the source tree contains one target directory, `eureka-52168470043600520` | No supplied target for `52083180031500520` |
| [Exact-kernel issue 7](https://github.com/F-19-F/IonStackQuest3/issues/7) | Direct GitHub API response: open, zero comments, last updated 2026-07-30 | No resolved compatibility report for `5.10.237-g16c343ceed81` in this issue |
| [Singularity, commit `72139bc6`](https://github.com/Lumince/singularity/tree/72139bc63c5bc4378dc8f7aed59a3210e65c82d0) | Latest commit dated 2026-09-05 updates Frida; README still lists Quest 3 `52345320035400520` as its confirmed build | Root-on-boot is documented; this exact build and unsigned kernel acceptance are not established |
| [Meta source commit `ab1c4601`](https://github.com/facebookincubator/oculus-linux-kernel/commit/ab1c46013e3f279a9d033a1c3cf0542c1d32d46c) | Published build `5234532.4010.520` changes `kernel/locking/rtmutex.c` | Confirms a concrete later kernel fix, not an unlock route |

The [pinned IonStack README](https://github.com/F-19-F/IonStackQuest3/blob/81faf7942576fd826bb097fb2b4fae76e090a103/README.md)
identifies Quest 3 `52345320040100520` as patched for CVE-2026-43499. It requires
matching firmware to adapt other builds. A lower incremental number is not
proof that the supplied target, memory layout or exploitation assumptions match
this headset. Its documented outcome is a root shell, not a bootloader unlock.

The [configuration generator](https://github.com/F-19-F/IonStackQuest3/blob/81faf7942576fd826bb097fb2b4fae76e090a103/gen_ionstack_config.py)
operates on an ARM64 kernel ELF. Therefore the useful next offline input is the
exact stock kernel extracted from this build's authenticated boot artifact. A
locally rebuilt kernel with the same `5.10.237` version is not a substitute for
the shipped binary's symbol layout. No configuration or exploit was generated
from a guessed match.

## Exact source and stock image remain separate gaps

The earlier exact mirror URL failure is retained in the bring-up report; no
large firmware download was repeated here. The existing reference OTA
`52433670036000520` is a different build and remains unsuitable as evidence of
this unit's exact recovery contents.

The direct [Meta commit history ending at `063d9fb8`](https://github.com/facebookincubator/oculus-linux-kernel/commits/063d9fb81e3f8d953b86cff0dd7adb4b7432b48e)
shows an April 27 repository resynchronization whose parent is the March 24
build `5194302.6530.520`. That resynchronization's commit message does not name
`5208318.3150.520`. Consequently its April date and Linux version do not close
the source-to-installed-build identity gap. This check supplies provenance for
the candidate snapshot, not proof that the snapshot is wrong.

The public [Meta software update entry point](https://www.meta.com/en-us/help/quest/software_update/)
was fetched without connecting a device. The unauthenticated response exposed
the site shell, with no retrievable exact-build package manifest or restore
contract. This investigation therefore obtained no official download for
`52083180031500520` and did not verify that the tool can recover from a modified
boot chain. It was not started against the headset. An available updater page
alone cannot establish exact-build restoration or downgrade acceptance.

## What closes the boot requirement

Root, permission to flash Android partitions, permission to replace critical
bootloader components, and recovery are distinct capabilities. AOSP specifies
separate [bootloader and critical-section lock states](https://source.android.com/docs/core/architecture/bootloader/locking_unlocking).
This is a protocol model, not evidence that Quest implements a generally
available unlock command. The [FreeXR exploit authors](https://github.com/FreeXR/eureka_panther-adreno-gpu-exploit-1/wiki)
also explicitly exclude bootloader unlocking from their GPU-root exploit.

The outstanding evidence needed to advance this unit is:

1. The actual previously performed unlock/root method, or read-only bootloader
   state collected during a separately authorized bootloader session. An ADB
   connection or developer-mode toggle does not answer this.
2. Authenticated stock artifacts matching the unit, including boot, vendor_boot,
   DTBO and AVB metadata, plus a demonstrated restore path that remains usable
   after the proposed change. Preserve device-specific calibration separately.
3. Proof that its stock boot chain accepts the proposed nonpersistent boot
   method before any installation design depends on that method.

Until those are available, the kernel, driver and virtual SteamVR work can
continue, but a flash-ready designation would be unsupported. No command to
modify ABL, devinfo, AVB, slots or storage follows from this report.
