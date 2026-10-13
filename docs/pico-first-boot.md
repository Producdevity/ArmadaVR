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
3. Verify the physical restoration mechanism before installing Reframed VR.
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

The later full runtime has a separate maintained assembler:

```sh
python3 -B tools/assemble-pico-boot.py STOCK_BOOT_BACKUP \
  PHYSICAL_PICO_KERNEL_DIRECTORY INITRAMFS_DIRECTORY ROOT_DIRECTORY \
  --reference-sha256 RETAINED_STOCK_SHA256 \
  --root-manifest-sha256 VERIFIED_ROOT_MANIFEST_SHA256 \
  --mkbootimg PINNED_AOSP_TOOL_DIRECTORY --output NEW_RUNTIME_CONTAINER
```

Take the root manifest hash from the completed offline root verification. The
assembler verifies every recorded artifact, the matching kernel, initramfs and
firmware, current Pico startup sources, and the ext4 label and geometry. It
binds the installed component receipts and rechecks inputs after assembly.
Both assemblers unpack the supplied stock backup themselves; neither depends
on another build's extracted DTB or saved command arguments. The full runtime
container leaves display enabled and requires its separately verified external
root image. Assembly does not establish that root transport, DTB selection,
physical drivers or recovery work, and it does not replace the initial bounded
diagnostic.

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
A 90-second alarm is armed before mounting the virtual filesystems or reading
identity. Mount and identity failures immediately request power-off before USB
setup; a validated virtual test shortens the alarm to 10 seconds. Neither the
timer nor a shutdown request can recover a hung kernel or replace independent
recovery. The `1d6b:0104` Linux gadget identifier is for this development diagnostic only.
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

## Offline verification

The current diagnostic uses the physical Pico kernel
`22177b1d0003c2873172f8ba4e080c59e4ebf8ba9b3adf20b8e53295385b85d0`
and the current Reframed init. It has not been booted on a headset. The unsigned
Android-v2 image was assembled and unpacked with matching kernel, initramfs and
unmodified stock DTB hashes. Its initramfs is about 1.4 MiB. Build, assembly,
VM and host receipts are in `output/pico-diagnostic-shutdown-20261013-v1/`;
use `build-v2` and `boot`. Earlier diagnostic artifacts remain historical.

Seven diskless QEMU boots use that same init binary and physical-config kernel:
normal startup, wrong serial, missing serial, wrong diagnostic token, missing
`/proc`, and missing/conflicting KMS-disable options. Each reaches controlled
power-off. Normal startup creates the real configfs serial function and finds
no UDC. The earlier identity-refusal hang was reproduced before the fix and its
evidence is retained. These tests do not exercise physical USB enumeration,
log transfer, display or recovery.

Linux ARM64 sanitizer tests cover early mount failures, missing or invalid
identity, a non-RAM root, alarm setup failure, PID refusal, USB controller
selection, display guards and bounded report storage. Run them with the existing
Mesa compiler image:

```sh
just test-pico-diagnostic-init NEW_TEST_DIRECTORY
```

Host sanitizer tests cover log identity, truncation and eight USB discovery
scenarios, including delayed configuration and duplicate devices. The matching
host receiver is compiled in the `host` subdirectory; its tests use mocked USB.

The initial diagnostic requires `msm_drm.modeset=0`. Pico's vendor display
configuration enables MSM DRM separately from the top-level kernel
configuration; normal registration can restore continuous splash and enable
panel regulators before userspace. The existing modeset parameter stops MSM
DRM master registration and that splash path. After validating identity, init
checks the exact command-line option, the kernel's `N` parameter value and
absence of the `msm_drm` platform driver. Other platform drivers still probe;
this does not establish an untouched physical display.

The authenticated older ABL's default ARM64 placement also passes the updated
diagnostic size check, with more than 39 MiB between the kernel runtime extent
and calculated DTB boundary in its default 86 MiB allocation. Actual UEFI
`KernelBaseAddr`/`KernelSize`, live RAM reservations and bootloader DTB selection
remain unverified. Container header addresses do not describe this dynamic
placement. The full runtime retains display support and needs a separate,
verified external root filesystem.

The earlier full-root container was assembled and roundtrip-verified offline,
but its external root transport and physical device-tree compatibility are
untested. Kernel/VM results do not replace those checks. Rebuilding its
userspace under the new project name is also pending
(see [project naming](project-naming.md)). Native USB diagnostics, stock return
and the complete recovery sequence remain physical acceptance requirements.

Private audit and current-source receipts:
`output/pico-flash-path-20261013-v1/`. No image has been flashed.
