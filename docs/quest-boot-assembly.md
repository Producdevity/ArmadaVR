# Quest boot container assembly

`tools/assemble-quest-boot.py` creates unsigned Android v4 containers for offline
inspection. It uses a verified reference boot set to preserve its container
layout, vendor addresses, command line, bootconfig and overlay order, then
substitutes the compiled Linux kernel, patched base DTB and matching initramfs.
It does not install an image, sign it or establish bootloader acceptance.

Fetch the pinned AOSP tools once into a new directory:

```sh
python3 -B tools/fetch-avb.py output/avb-tools
python3 -B tools/fetch-mkbootimg.py output/boot-tools
```

Build the [initramfs](headset-initramfs.md), then assemble into a new directory:

```sh
python3 -B tools/assemble-quest-boot.py REFERENCE_DIRECTORY KERNEL_BUILD INITRAMFS_BUILD \
  --reference-build BUILD_NUMBER \
  --avbtool output/avb-tools/avbtool.py \
  --mkbootimg output/boot-tools \
  --output NEW_ASSEMBLY_DIRECTORY
```

The reference directory must satisfy the [boot-set verifier](boot-set-verification.md).
The build number is a declaration checked against the supplied reference record;
it is not a certificate of device compatibility. The assembler refuses QEMU
kernels, mixed kernel/initramfs builds, changed artifacts, altered AOSP tools,
unexpected tool dependencies and existing outputs.

Before assembling Linux containers it unpacks and rebuilds the original boot
and vendor_boot payloads with AOSP's tools. Both must match the original unsigned
payload byte-for-byte. It preserves empty arguments from the unpacker's NUL
format and never evaluates generated arguments through a shell.

The Linux outputs are:

- `boot-linux-unsigned.img`: the selected Linux Image and dracut initramfs, with
  Android OS-version fields cleared and the dedicated read-only ext4/RAM-overlay
  root policy. `--root-label` can change the filesystem label. A Quest-service
  initramfs also adds `armada.quest=usb-root`; the firmware bundle must match the
  reference build. Firmware-only and firmware-free variants omit this flag.
- `vendor_boot-linux-unsigned.img`: the patched base DTB and an empty platform
  CPIO fragment, retaining the reference vendor metadata. Android vendor-ramdisk
  modules are replaced by the initramfs's matching Linux modules.
- `dtbo-unsigned.img`: the reference table without its AVB trailer. Every entry
  must match a compiled overlay exactly, cover every compiled board once and
  preserve the reference order and identifiers.

The assembler inspects the new containers, requires partition-size headroom,
rejects retained AVB/GKI signatures, and extracts their payloads again to verify
the exact kernel, ramdisk, DTB hashes and Linux command line. `report.json` records input identities,
tool pins, commands and results; `logs/` retains command output. Failed runs stay
in their output directory for diagnosis.

## Recorded verification

September 11: `output/quest3-boot-assembly-v2/` used reference firmware
`52433670036000520`, `linux-build-v7` and `headset-initramfs-v3`. Its three Linux
artifacts exactly match the earlier independently recorded v1 experiment:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| boot | 70,262,784 | `4b8cf866f134e0b8e8ebaafdcfb0d3b3f4ca815545be3a3178cdadd42de82884` |
| vendor_boot | 716,800 | `837a38a7d8fbf04765f282b2739d1b1fecb9dbc09915c5e192835a636115a98b` |
| dtbo | 12,564 | `a02fdeede1c1466f6bd97a7deadb0f034a80e011e8fd06706fa94dbfa54f29e8` |

All 13 overlays match. Stock boot/vendor payload roundtrips and Linux payload
extraction pass; original reference hashes remain unchanged. The AOSP tool pin
is commit `d2bb0af5ba6d3198a3e99529c97eda1be0b5a093` with per-file hashes in
`profiles/mkbootimg.json`.

October 8: `output/quest3-boot-assembly-current-20261008/` repeats assembly against
reference build `52083180032000520`, extracted from the previously authenticated
current-device OTA. A new read-only device query confirms the same build and
locked/green boot state. All 13 compiled overlays match that reference exactly,
including PVT1.1 at table index 8. Stock boot/vendor payload roundtrips, Linux
payload extraction, partition headroom and unchanged reference inputs pass.
The boot image matches the earlier hash; vendor metadata and the DTBO table
reflect the current reference. The independently rehashed outputs are:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| boot | 70,262,784 | `4b8cf866f134e0b8e8ebaafdcfb0d3b3f4ca815545be3a3178cdadd42de82884` |
| vendor_boot | 716,800 | `6f0ac21d29ccae115f2dedecc2d4e728fbbe7c41734fa4a142888389ab2842d4` |
| dtbo | 12,564 | `227b03c7b0bf2f052ec65fb97b6991c2e3135ab513d41c6908c3abb574beff9b` |

This uses the existing `linux-build-v7` kernel and firmware-free
`headset-initramfs-v3`; no kernel rebuild or physical execution occurred. The
custom kernel is 5.10.246, while the running stock kernel is 5.10.237. Matching
container metadata and overlays do not establish stock module ABI compatibility,
peripheral handoff or bootloader acceptance. Authenticated recovery, an accepted
custom boot route, firmware/calibration and physical driver operation remain
missing. These unsigned files are not flash-ready. The QEMU initramfs/runtime
results exercise Linux userspace and archive handling, not the Quest bootloader.
