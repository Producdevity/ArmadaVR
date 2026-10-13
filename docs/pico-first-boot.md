# Pico first boot

The immediate target is a bounded Linux diagnostic boot on the inventoried
Neo3 Pro Eye, followed by a demonstrated return to stock. Display and tracking
bring-up follow that milestone. This is not yet an installation procedure.

## Recovery and authorization

1. Establish cold EDL entry on this unit without Android or fastboot commands.
   Verify its ROM identity, reuse the already accepted signed programmer for
   bounded reads, and verify the known Firehose reset returns stock Android.
   Cold stock recovery and fastboot menu entry have already passed and should
   not be repeated as substitutes.
2. Define the smallest bootloader transition and its restoration procedure.
   Bind every affected partition to verified original bytes, GPT extent and
   replacement size/hash. Preserve XBL, XBL configuration, GPTs, stock recovery
   and unrelated firmware. Do not perform a full firmware downgrade.
3. Verify the physical restoration mechanism before installing ArmadaVR.
   An actual restoration rehearsal writes storage; it cannot be silently
   treated as a read-only test under the current no-flash constraint. No
   deliberate bootloader corruption is required. A plan and readable backups
   alone are not evidence of successful restoration.
4. After a verified restoration path and the minimum bootloader transition,
   test command authorization and signed stock RAM boot before unlocking.
   Inspect any refusal instead of running the complete community script.
   Unlocking is a separate, data-erasing transition. Verify stock return before
   attempting the bounded Linux diagnostic image.

The current b1977 ABL has no RAM-boot handler. The authenticated older ABL has
one, but must first be installed; its matching signing root does not prove
rollback acceptance. The original community procedure replaces ABL, while the
fork also replaces devinfo. Whether replacement of devinfo is necessary on this
unit remains unverified. Authorization, unlocking, and unsigned-image
authentication are separate operations. Do not run a complete unlock/factory
reset script merely to test command availability.
[Original procedure](https://github.com/264312431/more-picohaxx/blob/f911e67f1714e189ba1dd8a46111065482dd6681/more-picohaxx.py),
[fork](https://github.com/chaixshot/more-picohaxx-tool/blob/c182399937107dd290cbc1ec2fdb06116af83a9d/picounlock.ps1).

A 9008 cable is a candidate for cold entry, not a guaranteed solution for Pro
Eye. Community Neo3 results are mixed. The EDL client's deep-flash cable method
depends on intact XBL/SBL, so a successful test would establish recovery within
that preserved early boot chain, not recovery from arbitrary storage damage.
[Device reports](https://github.com/chaixshot/more-picohaxx-tool/issues/5),
[EDL client](https://github.com/bkerler/edl#run-edl-examples).

## Available originals

The October 13 audit passes for all 21 saved partitions and all six pairs of
GPT copies. ABL, ABL backup, devinfo, boot, recovery and calibration originals
are available. The audit identifies five additional state partitions without
backups: `misc`, `frp`, `metadata`, `uefivarstore` and `secdata`. This does not
establish that a proposed transition writes all five. Audit the actual write
set and acquire any affected originals before proceeding. The existing
`bootbak`/`vbmetabak` mismatch remains preserved; those partitions are not a
verified fallback.

`tools/verify-pico-backup.py` reports missing partitions with their independently
checked LUN, GUIDs, sector size, LBA range and byte offset. Missing entries have
no backup path or content hash. This supports preparing a bounded acquisition
without treating a known partition layout as an acquired backup.

The missing `misc` read is prepared privately in
`output/pico-state-backup-20261013-v1/`, using the revalidated stock GPT extent.
The compiled helper permits only UFS configuration, six geometry queries,
bounded GPT reads, the single `misc` extent after host confirmation, and reset.
Its sanitizer fixture passes 532 allowed and 315 rejected commands without USB
access. The device reader has not run and is stored without execute permission;
no new `misc` original has been acquired. Recheck live ROM identity, geometry
and both GPT copies before using it. A successful reset after failed acquisition
must still report acquisition failure.

The community unlock flow expects a factory reset. File/application exports
and encryption implications need resolution before that step; raw userdata
alone is not a proven way to restore usable data after key changes. There is
no full userdata backup. A Pico4 owner's failed userdata restoration is a
relevant limitation, not proof of identical Pro Eye behavior.
[Owner report](https://github.com/chaixshot/more-picohaxx-tool/issues/4).

## Confirmed unlock effects

Static analysis of the authenticated 4.6.3 ABL and this unit's saved XBL narrows
the transition requirements:

| Operation | Confirmed implementation | Recovery implication |
| --- | --- | --- |
| Command authorization | Adds the restricted fastboot handlers in memory | Handler registration does not itself unlock or establish unsigned-image acceptance |
| Ordinary or critical unlock | Writes the corresponding flag through the Verified Boot protocol | Either command can change persistent state; do not assume critical unlock is needed for a RAM-boot test |
| Device-state storage | XBL uses Keymaster/RPMB when its security checks and secure reads succeed; other branches use `devinfo` | A raw `devinfo` backup is not a complete backup of the possible state stores |
| State reset | Calls Keymaster with request `0x10e` before scheduling recovery | Treat this as destructive key-state reset, independently of filesystem erasure |
| Recovery request | Writes a 1,088-byte message containing `--wipe_data` to `misc` | Acquire the missing `misc` original before an unlock; on this unit the helper read-modifies-writes one 4,096-byte sector |

The protocol header identifies the reset operation as a delete command to
Keymaster. Both unlock paths in the exact older ABL reach it; its build-time
guards enable the call. Reset or subsequent `misc` failure does not guarantee
unchanged state. Restoring ABL, devinfo or userdata bytes is not proven to
restore deleted secure keys.
[Qualcomm protocol definition](https://gitlab.com/Codeaurora/abl_tianocore_edk2/-/blob/797918c3bf59e2d65b4acb2acdfd6883a0605ee1/QcomModulePkg/Include/Protocol/EFIVerifiedBoot.h).

The preserved raw `devinfo` already has both flag bytes set, despite the
observed locked, green stock boot. This is consistent with a separate state
source but does not identify the live secure-storage branch. Replacing that
partition is therefore not an established prerequisite or sufficient unlock.
The older ABL's initialization also writes defaults if the state magic is
invalid. The raw backup has valid magic; the live secure record remains
unverified. Merely starting a different ABL must not be described as read-only.

These findings cover the traced authorization, initialization and unlock paths,
not every bootloader side effect or the secure application's implementation.
No direct write to `frp`, `metadata`, `uefivarstore` or `secdata` was established
by this trace. Do not erase or replace them speculatively. Private module
extractions, function exports, GPT correspondence and hashes are retained in
`output/pico-flash-path-20261013-v1/{abl-state,xbl-state}/`.

## Initial Linux acceptance

Use the compiled Pico kernel and authenticated stock DTB pack with an
initramfs-only diagnostic. It should reach init, provide bounded USB diagnostics
and power off, without mounting internal filesystems or starting the VR
runtime. Verify USB controller operation and controlled return before adding
the external root filesystem, display/mux, DSP and tracking services.

`tools/build-pico-diagnostic.py` now builds that separate initramfs. It packages
one static ARM64 init executable and an explicit bootloader serial; no shell,
modules, firmware, root filesystem or disk-mount service is included. Init
requires PID 1, a RAM root, `qcom,kona`, an exact diagnostic command-line token
and matching `androidboot.serialno`. Missing or conflicting identity refuses
USB setup. The virtual test requires a separate `qemu` serial and test token.
Read `ro.boot.serialno` on the identified stock device for this input. On the
inventoried Pro Eye it differs from `ro.serialno`, the serial shown by ADB.
Using the ADB serial would make the diagnostic refuse startup.

```sh
python3 -B tools/build-pico-diagnostic.py PHYSICAL_PICO_KERNEL_DIRECTORY \
  --boot-serial BOOT_SERIAL --output NEW_DIAGNOSTIC_DIRECTORY
```

Assemble the unsigned Android boot container from that diagnostic and the
authenticated full stock boot-partition backup:

```sh
python3 -B tools/assemble-pico-diagnostic.py STOCK_BOOT_BACKUP \
  PHYSICAL_PICO_KERNEL_DIRECTORY NEW_DIAGNOSTIC_DIRECTORY \
  --reference-sha256 RETAINED_STOCK_SHA256 --mkbootimg PINNED_AOSP_TOOL_DIRECTORY \
  --output NEW_CONTAINER_DIRECTORY
```

The assembler requires the diagnostic's physical serial, current source and
matching kernel receipt. It reconstructs the ramdisk to reject extra payloads
or substitution of the virtual test ramdisk, verifies the stock payload
roundtrip, and checks the new kernel/ramdisk/stock DTB sections after unpacking.
The reference hash must come from independently retained authentication or
backup evidence. The output remains unsigned, with boot/recovery and physical
DTB compatibility explicitly unverified; this command never accesses a device.

It configures the kernel's existing `gser` USB function, waits for the primary
`a600000.dwc3` UDC, and verifies its device-tree compatible, register extent and
USB core ID before binding. Stock firmware selects this controller for the
primary port; the enabled secondary `a800000.dwc3` controller may also register
a UDC. The diagnostic ignores that secondary controller and refuses mismatched
primary metadata. After binding, it waits for the primary UDC to reach the
configured state before opening the serial device. The kernel can buffer a
short report before configuration and discard it when the serial device closes.
The diagnostic transmits bounded kernel/system diagnostics and accepts no host
commands.
A 90-second alarm attempts power-off if the accepted physical diagnostic stalls;
this cannot recover a hung kernel or replace independent recovery. The
`1d6b:0104` Linux gadget identifier is for this development diagnostic only.
The host collector additionally requires the exact manufacturer, product,
serial and interface descriptors, without detaching a kernel driver:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror tools/pico-diagnostic-log.c \
  $(pkg-config --cflags --libs libusb-1.0) -o NEW_LOG_RECEIVER
NEW_LOG_RECEIVER --serial BOOT_SERIAL > NEW_LOG_FILE
```

The receiver waits up to 90 seconds for enumeration and configuration, retrying
when a matching device has no active diagnostic interface yet. It refuses
duplicate matching devices and rejects incomplete or wrong-device reports. A received report proves neither physical shutdown nor
return to stock; those require separate observation. The diagnostic does not
load an SELinux policy, so the enforcing kernel arguments alone are not a
claim of userspace policy enforcement.

October 13 offline verification uses the unchanged physical Pico kernel
`913b4a013bcd35900e104aca29ec0ccf3e356c2d77581af699c115dda9223f08`.
The same init binary creates the real configfs serial function in diskless
QEMU, observes no UDC, and powers off normally; the complete harness finishes in 5.53 seconds. ARM64
sanitizer checks reproduce the previous refusal with two UDCs and verify primary
selection, missing or mismatched controller properties, PID refusal,
exact/conflicting command-line identities and bounded report storage; host sanitizer checks cover log identity, truncation and eight
discovery scenarios, including delayed configuration and duplicate devices.
These tests do not exercise physical USB enumeration or log transfer.

The diagnostic Android-v2 container is assembled and unpacked with matching
kernel, initramfs and unmodified stock DTB hashes. Its initramfs is about 1.4 MiB,
instead of the full runtime's 35 MiB. Private build, assembly and VM receipts
are in `output/pico-usb-20261013-v1/` (`build-v2`, `boot-v2`, `vm-v2` and
`host-v2`). These supersede its initial build and the earlier
`output/pico-diagnostic-20261013-v1/` images. The builder also requires the
primary controller, PHY, charger/PD, clock and EUD drivers to be built in,
because the diagnostic contains no modules. No diagnostic has been booted on
the headset.

The later build through kernel patch `0034` uses Image
`0c9d1c19e7ae046e6a18fc72376d39d5f0c0db487a580868352be0766f7b5c7a`.
Its USB-only command line also requires `msm_drm.modeset=0`. Pico's vendor
display configuration enables MSM DRM separately from the top-level kernel
configuration. Its normal registration can restore the continuous splash and
enable panel regulators before userspace starts. The existing modeset parameter
stops MSM DRM master registration and that splash path.

After validating the diagnostic identity, init checks the exact, non-conflicting
command-line option, the kernel's `N` parameter value and absence of the
`msm_drm` platform driver. A failed check requests power-off before USB setup.
ARM64 sanitizer tests cover missing and malformed parameter values and an
unexpected driver entry. Diskless boots of the actual kernel verify both the
disabled-KMS diagnostic and controlled shutdown with missing/conflicting
options. Other platform drivers still probe; this is not proof of an untouched
physical display or a replacement for recovery.

The corresponding diagnostic artifacts are in
`output/pico-panel-load-20261013-v1/diagnostic-build-v3` and `diagnostic-boot-v2`.
They supersede the earlier USB diagnostic builds. The matching full initramfs,
root and runtime container retain display support; the KMS restriction belongs
only to the initial USB diagnostic. Full-root filesystem and runtime-loader
checks pass with the rebuilt modules and unchanged firmware and runtime
libraries. The original root image remains intact. The runtime container is
`output/pico-boot-container-20261013-v2`; it remains an offline, unsigned image.

The exact older ABL's default ARM64 placement also passes an offline size
check: the kernel's `0x80000` text offset matches, and its declared runtime size
leaves 41,734,144 bytes before the diagnostic's calculated DTB boundary in the
default 86 MiB allocation. Actual UEFI `KernelBaseAddr`/`KernelSize`, live RAM
reservations and bootloader DTB selection still require physical verification.
Container header addresses alone do not describe this dynamic placement.

The current full-root container is assembled and roundtrip-verified offline,
but its external root transport and physical device-tree compatibility are
untested. Kernel/VM results do not replace those checks. Native USB diagnostics,
stock return and the complete recovery sequence remain physical acceptance
requirements.

Private audit and current-source receipts:
`output/pico-flash-path-20261013-v1/`. No image has been flashed.
