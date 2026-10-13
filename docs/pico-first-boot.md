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
4. Establish acceptance of the minimum authorization change, then boot the
   exact stock image through the newly available boot path and verify return.
   Only then attempt the bounded Linux diagnostic image.

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

## Initial Linux acceptance

Use the compiled Pico kernel and authenticated stock DTB pack with an
initramfs-only diagnostic. It should reach init, provide bounded USB diagnostics
and power off, without mounting internal filesystems or starting the VR
runtime. Verify USB controller operation and controlled return before adding
the external root filesystem, display/mux, DSP and tracking services.

The current full-root container is assembled and roundtrip-verified offline,
but its external root transport and physical device-tree compatibility are
untested. Kernel/VM results do not replace those checks. Native USB diagnostics,
stock return and the complete recovery sequence remain physical acceptance
requirements.

Private audit and current-source receipts:
`output/pico-flash-path-20261013-v1/`. No image has been flashed.
