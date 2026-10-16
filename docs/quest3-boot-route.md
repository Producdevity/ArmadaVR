# Quest 3 exact-build boot and recovery status

Updated October 16, 2026. The retained stock firmware contains the CPU-timer
code affected by CVE-2026-64560. No working Quest privilege-escalation or custom
boot result follows from that static finding. Independent recovery and custom
kernel acceptance remain unverified; see [device installation requirements](device-installation.md).

## Firmware identity and source coverage

The last recorded headset build is `52433670036000520`, kernel
`5.10.246-gd7102a837402`, with locked/green Verified Boot. The Quest was absent
from ADB on October 16. Its current installed release must be checked again
before applying any firmware-specific conclusion. Slot B previously reported
unbootable; it is not a verified fallback.

Meta's repository defaults to the Oculus Go branch. Quest 3 research must use
[`oculus-quest3-kernel-master`](https://github.com/facebookincubator/oculus-linux-kernel/tree/oculus-quest3-kernel-master).
The latest published Quest 3 commit inspected was
[`bcbad930`](https://github.com/facebookincubator/oculus-linux-kernel/commit/bcbad93099abe941ec56a53d2bd6e1a66e951981),
named build `5243367.4880.520`. Its parent is the project's pinned
[`dbc2bc83`](https://github.com/facebookincubator/oculus-linux-kernel/commit/dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f),
named `5243367.3600.520`. The newer commit changes four networking files to
preserve shared-fragment state and avoid unsafe in-place ESP processing. This
corroborates Fugu's published patch boundary; it does not fix the separate
CPU-timer code inspected below.

## Supplied projects and their limits

| Source | Verified scope | Relevance to this firmware |
|---|---|---|
| [CVE-2025-21479](https://nvd.nist.gov/vuln/detail/cve-2025-21479) and [FreeXR, `603347dd`](https://github.com/FreeXR/eureka_panther-adreno-gpu-exploit-1/blob/603347dda31a6d33664ce3740cecbd97b49b3a25/README.md) | GPU-firmware memory corruption and runtime root. FreeXR marks Quest 3 builds from `51154110129000520` fixed. | Historical access route; no support for the recorded 524 build. |
| [DiscordTonyStark/root, `c34903fc`](https://github.com/DiscordTonyStark/root/tree/c34903fc72e2a1b976bc38ee01816da439266164) | The repository contains only a README; its referenced implementation files are absent. | No independent implementation or newer firmware coverage. |
| [CyberMeowfia, `94ae8ab3`](https://github.com/NebuSec/CyberMeowfia/tree/94ae8ab301b2ec9f36bdd66321b6e11209ae60d1) | IonStack plus newer Android vulnerability research. The newer demonstrations target Pixel 10 Pro, Android 17 and Linux 6.6. | Supplies source-level leads, not a validated Quest port. |
| [IonStackQuest3, `81faf794`](https://github.com/F-19-F/IonStackQuest3/blob/81faf7942576fd826bb097fb2b4fae76e090a103/README.md) | CVE-2026-43499; documented target `52168470043600520`, kernel `5.10.240-g69827d40d782`. Quest 3 builds from `52345320040100520` are marked fixed. | Not a route for the recorded 524 release. New symbol addresses cannot restore a fixed primitive. |
| [Fuguquest, `2324ce26`](https://github.com/Henry1887/fuguquest/blob/2324ce262e674504ad41ec82abcda3bf09dd01e6/README.md) | CVE-2026-43284. Author reports `52433670036000520` as the last vulnerable Quest 3 build and `52433670048800520` as the first patched build. | Potential temporary-root research on the last recorded release; previous local execution/quiescence limitations remain open. |
| [Singularity, `d56705a0`](https://github.com/Lumince/singularity/blob/d56705a0025af5dcc532f802f1eef11f247360db/README.md) | Uses Fugu; latest listed confirmed Quest 3 release remains `52433670036000520`. Root-on-boot reapplies access within stock Android. | Does not establish unlocked cold boot. The repository does not expose the complete application implementation. |

The [Android August 2025 bulletin](https://source.android.com/docs/security/bulletin/2025-08-01#2025-08-05-security-patch-level-vulnerability-details)
lists the GPU issue under closed-source Qualcomm components. Generic Android
patch levels alone do not establish Meta firmware compatibility. The FreeXR
code's strict greater-than version check also differs from its README's
inclusive fixed boundary; passing that guard is not proof of support.

IonStack's configuration generator changes symbol addresses while target
structure offsets remain compiled in. Its temporary su daemon is research
infrastructure, not an appropriate production privilege service: the inspected
socket is world-writable and its accept path lacks peer-credential authorization.
The [FreeXR FAQ](https://github.com/FreeXR/eureka_panther-adreno-gpu-exploit-1/wiki)
explicitly distinguishes runtime root from bootloader unlock.

## CPU-timer issue: source and stock binary verified

The [Linux CNA record](https://cveawg.mitre.org/api/cve/CVE-2026-64560) identifies
CVE-2026-64560, fixed in the 5.10 stable series at 5.10.262. The
[5.10 backport](https://github.com/gregkh/linux/commit/67aa823e3e8c229c6d374df79c804f6721cb83b6)
corrects task lookup and signal-state ordering during concurrent CPU-timer
operations and non-leader exec. This investigation checked the source and
compiled behavior rather than inferring applicability from the version number.

For the authenticated `52433670036000520` stock image:

- The retained kernel matches the boot image payload byte for byte. Its
  reconstructed ELF preserves that entire payload. The embedded kernel
  configuration matches the retained configuration and enables POSIX timers,
  SMP and preemption; native ARM64 timer syscalls are compiled in.
- Timer deletion performs one PID-to-task lookup. If that task's signal-handler
  state disappears, the compiled path can complete without a new task lookup
  or removal from the CPU-timer queue. The newer upstream retry is absent.
  The corresponding timer-set and timer-rearm paths retain the old behavior.
- The native syscall table, CPU-clock callback table and CFI jump stubs connect
  the timer-delete syscall to the inspected function. Process/thread clock
  creation installs the common CPU-clock callbacks; their initial clock tables
  do not themselves contain a deletion callback.
- The compiled exec path still replaces and releases the previous group leader.
  The inspected signal/exit paths also match the source predating the fix.

These establish that the affected implementation is present in that stock
firmware. They do not establish a successfully triggered race, runtime syscall
policy, reliable privilege escalation or compatibility with a later release.
The [public Android demonstration](https://github.com/NebuSec/CyberMeowfia/blob/94ae8ab301b2ec9f36bdd66321b6e11209ae60d1/security-research/Ndays/Android-CVE-2026-64560/README.MD)
is for a different device and kernel. No exploit was built, ported or executed
for this check.

Six relevant files are byte-identical between Meta's 360 and 488 source
revisions: `fs/eventpoll.c`, `fs/exec.c`, `kernel/exit.c`, `kernel/signal.c`,
`kernel/time/posix-cpu-timers.c` and `kernel/time/posix-timers.c`.
Thus the published 488 source retains the CPU-timer issue too; its shipped
binary was not inspected. No claim is made for newer unpublished source or
firmware. In contrast, [CVE-2026-43074](https://cveawg.mitre.org/api/cve/CVE-2026-43074)
is excluded for these source revisions: they lack the implicated eventpoll
path introduced by the Linux 6.4 refcount conversion.

Local evidence is preserved under
`output/quest-access-research-20261016-v1/stock-timer-applicability-v1/`:
authentication bindings, embedded configuration, source hashes, disassembly,
review notes and the rejected initial clock-table assumption. The kernel SHA256
is `db61c073962bf1df38cbb09cbb01f03e5595822754ffea1dc75166555f7d545e`.
The byte-preserving ELF reconstruction is an analysis artifact, not a boot image.

## Firmware archive and recovery implications

The [Cocaine.trade Quest 3 index](https://cocaine.trade/Quest_3_firmware) currently
lists releases through `52641960021500520`, built October 8, 2026. Its hash for
`52433670036000520` matches the already retained, independently authenticated OTA:
`e4c57710fbb067051698b8df4a98fd19ed7e76cb6387f8f21aca8377f49d2e04`.
No duplicate firmware download was needed. The archive is useful for locating
stock packages and comparing metadata; it does not establish downgrade
acceptance or recovery after a modified boot chain. Package availability,
manufacturer signature verification, rollback acceptance and successful restore
are separate checks. [AOSP rollback protection](https://source.android.com/docs/security/features/verifiedboot/verified-boot#rollback-protection)
explains the mechanism, not this unit's actual stored rollback state.

The next decision needs a fresh exact-build inventory. If the Quest still runs
360, the CPU-timer finding and prior Fugu research remain relevant to bounded
temporary-access investigation. If it runs 488 or newer, do not retry Fugu or
older fixed routes by changing offsets. Match the new stock binary and source
before judging the CPU-timer issue. In either case, usable temporary access,
Quest-specific RAM handoff and independently verified restoration are still
required before persistent installation. The existing
[Quest-KeXec porting work](https://github.com/YusufOzmen01/quest-kexec/blob/e61edbd7fbdf6dc4976a979c61cc545e04738422/HOW_TO_PORT.md)
has no newly demonstrated Quest 3 acceptance; Quest Pro memory constants cannot
be reused without live RAM, DMA and peripheral-ownership evidence.

## Historical September 10 investigation

The following findings retain their original date and firmware scope. The
October findings above and installation document supersede their current-state
claims; earlier failures and evidence remain preserved.

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
