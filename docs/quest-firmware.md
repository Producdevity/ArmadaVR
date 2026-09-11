# Quest ADSP firmware packaging

`tools/prepare-quest-firmware.py` extracts a checked ADSP bundle from a local
vendor filesystem image. It uses read-only `debugfs` in an offline container;
it does not execute firmware, access a headset or modify the source image.

```sh
python3 -B tools/prepare-quest-firmware.py VENDOR_IMAGE \
  --sha256 EXPECTED_VENDOR_SHA256 --reference-build BUILD_NUMBER \
  --output NEW_FIRMWARE_DIRECTORY
python3 -B tools/build-headset-initramfs.py KERNEL_BUILD \
  --firmware NEW_FIRMWARE_DIRECTORY --output NEW_INITRAMFS_DIRECTORY
```

The image checksum and build number are supplied declarations. The tool checks
the image's bytes before and after extraction; it does not authenticate an OTA,
establish manufacturer-key trust or prove a match to a connected device.

The preparer reads the Qualcomm ELF32 MDT program headers and selects the split
files needed by this kernel's `mdt_loader.c`: loadable segments with file data,
plus the signing-metadata segment. It preserves the MDT itself, rejects malformed
geometry and segment lengths, and omits unreferenced blobs. It also includes
`adspr.jsn`, `adsps.jsn`, `adspua.jsn` and `battmgr.jsn`, validating their ADSP
domains and requiring the `charger_pd` service-registry declaration.

`manifest.json` records every selected file's size and SHA-256, the source-image
identity and service domains. Existing outputs, symlinks, missing files, changed
files and unexpected payload files are rejected. `prepare.json`, `prepare.log`
and `debugfs.log` retain the command and extraction evidence.

The initramfs builder accepts this bundle explicitly through `--firmware`. It
checks the manifest, copies the selected files into `/usr/lib/firmware`, then
unpacks the completed initramfs and checks every firmware hash and the exact
file set. The source bundle is checked again after construction. Without this
option, the builder preserves the original firmware-free behavior.

The boot assembler also rejects an initramfs whose firmware reference-build
declaration differs from the selected boot reference. This prevents a declared
build mismatch; it does not establish physical compatibility.

## Recorded offline evidence

September 11, reference `52433670036000520`:

- `output/quest-firmware-v1/`: 48 selected files, 13,435,395 bytes. The bundle
  contains the MDT, 42 loadable split files, one signing-metadata file and four
  service JSON files. `battmgr.jsn` identifies `msm/adsp/charger_pd`, instance 74.
- `output/headset-initramfs-fw-v1/`: all 272 kernel modules and all 48 firmware
  files verified after extraction. The initramfs is 35,311,821 bytes, SHA-256
  `a254ae27c91e5a3b4e5c5423983ae971ace5ec4e7d5c5bfe8d223f0d79da1ba4`.
- `output/headset-initramfs-default-v5/`: omitting firmware reproduces the
  previous 29,686,256-byte initramfs with SHA-256
  `3a99b84de69b6ed5dfb4f49e4c7aaf84dcfb6634c64efa553ad5122c0e6246f8`.
- `output/initramfs-root-fw-v1/`: the separate QEMU variant boots through the
  vendor CPIO prefix, reaches the RAM-overlay root and shuts down normally.
  A missing-root boot powers off through the dracut emergency path. Both
  writable backing images remain byte-identical.
- `output/quest3-boot-assembly-fw-v1/`: unsigned boot container extraction and
  partition headroom pass. Boot is 75,890,688 bytes, SHA-256
  `34427e89d7196fd140082caa054a68be7d363538bd254dfa3567c5237df47208`;
  vendor_boot and DTBO remain identical to the earlier assembly.

Firmware remains data in these tests. The [early ADSP startup](quest-early-boot.md)
and service-mapper integration are not implemented yet. DSP authentication,
charging, USB role negotiation, physical root discovery and exact-device
recovery remain unverified. These images are not flash-ready.
