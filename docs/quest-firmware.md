# Quest firmware packaging

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
and service-mapper integration now have separate virtual acceptance. DSP authentication,
charging, USB role negotiation, physical root discovery and exact-device
recovery remain unverified. These images are not flash-ready.

October 8, current build `52083180032000520`: the independently authenticated OTA
and verified payload identify vendor image SHA-256
`6d126f9a3db39a7f8bed2268b7fbd3e2f6acb35a4ae768c83c8690f97f7c25d6`.
`output/quest-adsp-firmware-52083180032000520-v1/` contains 48 files totaling
13,448,003 bytes; its source-image hash is unchanged after extraction.
Twenty-eight files differ from the earlier reference bundle, while all four
ADSP service declarations are unchanged. The matching physical-kernel
[initramfs](headset-initramfs.md) includes this bundle. Outer OTA authentication
and payload integrity do not establish direct firmware authentication,
secure-world acceptance or physical operation.


## GPU firmware

Use the same preparer with `--component gpu` to select the KGSL firmware from
the reference vendor image. The default component remains ADSP, and the
initramfs's ADSP input rejects GPU bundles.

```sh
python3 -B tools/prepare-quest-firmware.py VENDOR_IMAGE \
  --sha256 EXPECTED_VENDOR_SHA256 --reference-build BUILD_NUMBER \
  --component gpu --output NEW_GPU_FIRMWARE_DIRECTORY
python3 -B tools/build-headset-root.py KERNEL_BUILD STARTUP_INITRAMFS \
  --firmware ADSP_BUNDLE --gpu-firmware NEW_GPU_FIRMWARE_DIRECTORY \
  --turnip VERIFIED_TURNIP_BUILD --image RUNTIME_IMAGE --output NEW_ROOT_DIRECTORY
```

The `gen7_6_0` KGSL core requests `a740v3_sqe.fw`, `gmu_gen70200.bin` and
`a740v3_zap.mdt`. The vendor MDT loader additionally requests the ZAP loadable
and signing segments selected by the MDT headers. For this reference those
are `a740v3_zap.b01` and `.b02`. The alternative `.elf`, `.mbn` and `.b00`
files are not inputs to this loader path.

The preparer validates SQE header/instruction alignment and walks the complete
GMU block stream, checking lengths, Gen7 address ranges, metadata order and
preallocations for non-TCM payloads. It preserves firmware bytes. These
structural checks and file hashes do not authenticate firmware or prove that
the GPU or secure world accepts it. The root builder also requires the GPU
and ADSP bundles to declare the same source-image hash and reference build.

`output/quest-gpu-firmware-v1/` contains five files totaling 157,952 bytes from
reference `52433670036000520`; all match the earlier retained reference
extraction. The completed roots `output/headset-root-qemu-v3/` and
`output/headset-root-v3/` contain all five verified GPU files alongside the
48-file ADSP bundle and corrected Turnip library.

`output/gpu-fw-kernel-v1/` tests files extracted from the completed QEMU root.
A diskless guest loads the exact vendor `qcom-scm.ko` and `mdt_loader.ko`, then
requests all five files through the real kernel firmware API and compares
sizes/checksums. The real `qcom_mdt_get_size` returns a 4 KiB region, and
`qcom_mdt_read_metadata` reads the split signing file and reconstructs the
expected 6,284-byte metadata. The test does not call `qcom_mdt_load`, PAS
initialization/authentication or GPU execution. QEMU identity guards reject
use on a physical headset.

The QEMU root also passes the native/Windows VR lab and mapper-failure poweroff
with an unchanged backing image (`output/quest-startup-gpu-firmware-v1/`).
The lab uses software rendering. Real panel scanout, GPU completion, firmware
acceptance and exact-device recovery remain open.

The exact-current vendor image also supplies
`output/quest-gpu-firmware-52083180032000520-v1/`: five files totaling 157,952
bytes. The ZAP MDT and `.b02` differ from the older reference; the SQE version
and file set are unchanged. Structural checks pass, but this new bundle has
not been executed or packaged into a newly validated root filesystem. The
older reference bundles and root images remain preserved.
