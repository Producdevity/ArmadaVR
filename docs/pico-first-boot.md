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
one static ARM64 init executable and an explicit device serial; no shell,
modules, firmware, root filesystem or disk-mount service is included. Init
requires PID 1, a RAM root, `qcom,kona`, an exact diagnostic command-line token
and matching `androidboot.serialno`. Missing or conflicting identity refuses
USB setup. The virtual test requires a separate `qemu` serial and test token.

```sh
python3 -B tools/build-pico-diagnostic.py PHYSICAL_PICO_KERNEL_DIRECTORY \
  --serial HEADSET_SERIAL --output NEW_DIAGNOSTIC_DIRECTORY
```

It configures the kernel's existing `gser` USB function, waits for one UDC,
and transmits bounded kernel/system diagnostics. It accepts no host commands.
A 90-second alarm attempts power-off if the accepted physical diagnostic stalls;
this cannot recover a hung kernel or replace independent recovery. The
`1d6b:0104` Linux gadget identifier is for this development diagnostic only.
The host collector additionally requires the exact manufacturer, product,
serial and interface descriptors, without detaching a kernel driver:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror tools/pico-diagnostic-log.c \
  $(pkg-config --cflags --libs libusb-1.0) -o NEW_LOG_RECEIVER
NEW_LOG_RECEIVER --serial HEADSET_SERIAL > NEW_LOG_FILE
```

The receiver waits up to 90 seconds for enumeration and rejects incomplete or
wrong-device reports. A received report proves neither physical shutdown nor
return to stock; those require separate observation. The diagnostic does not
load an SELinux policy, so the enforcing kernel arguments alone are not a
claim of userspace policy enforcement.

October 13 offline verification uses the unchanged physical Pico kernel
`913b4a013bcd35900e104aca29ec0ccf3e356c2d77581af699c115dda9223f08`.
The same init binary creates the real configfs serial function in diskless
QEMU, observes no UDC, and powers off normally in 5.57 seconds. ARM64 sanitizer
checks cover PID refusal, exact/conflicting command-line identities and bounded
report storage; host sanitizer checks cover log identity and truncation.
These tests do not exercise physical USB enumeration or log transfer.

The diagnostic Android-v2 container is assembled and unpacked with matching
kernel, initramfs and unmodified stock DTB hashes. Its initramfs is about 1.4 MiB,
instead of the full runtime's 35 MiB. Private build, assembly and VM receipts
are in `output/pico-diagnostic-20261013-v1/`. No diagnostic has been booted on
the headset.

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
