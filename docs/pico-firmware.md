# Pico Neo3 Pro and Pro Eye firmware and boot access

Checked October 12, 2026. Quest 3 remains in scope; Neo3 Pro Eye
bring-up is active on the connected development unit. It was identified
physically as Pro Eye; its generic Android model alone does not distinguish
enterprise variants. Neo3 Pro requires its own inventory and calibration backups.

## Current Pro Eye baseline

The connected Pro Eye reports global/overseas 5.8.4.0, internal build
`c000_rf01_bv1.0.1_sv5.8.4.0_202310092224_neo3_b1977_user`, incremental
`smartcm.1696861506`, Android 10 and Linux `4.19.81-perf+`. It has about 8 GB
RAM, Adreno 650 v3, a `SHARP493` panel property and eye-tracking support property.
These are inventory observations, not physical driver acceptance.

The October 10 [GPU tests](turnip.md#physical-android-gpu-acceptance) now verify
32 buffer/fence checks and 16 offscreen shader-rendered frames with a source-built
Turnip driver on this Pro Eye's stock Android system. API 29 tracing compatibility
is required to load the driver. DRM render-node access remains denied to the
ordinary shell. These tests do not boot the Armada kernel or validate its panel,
tracking, audio, controller or eye-tracking drivers.

The exact [global b1977 stock OTA](https://static.us-pui.picovr.com/5.8.4.0-202310092224-RELEASE-user-neo3-b1977-e4688d78c8.zip)
was authenticated against the public OTA certificate copied independently from
the running headset. Its SHA-256 is
`88669fe996510a879305d980ed4ba626c908ccc06ecead466393c3eadb58a8b6`.
The maintained verifier now supports its legacy `BLOCK` format and nonnumeric
incremental without weakening the default Quest A/B checks:

```sh
python3 -B tools/verify-ota.py STOCK_OTA.zip \
    --device-certificates STOCK_OTACERTS.zip \
    --expected-device PICOA7H10 --expected-build smartcm.1696861506 \
    --ota-type BLOCK \
    --expected-fingerprint 'Pico/A7H10/PICOA7H10:10/5.8.4.0/smartcm.1696861506:user/test-keys' \
    --expected-secure-boot-tag SE
```

This authenticates the archive and selected metadata. It does not execute or
validate the block updater, establish rollback acceptance, or prove restoration.

The full system partition can now be reconstructed offline from the authenticated
package using the bounded BLOCK OTA extractor:

```sh
python3 -B tools/extract-block-ota.py STOCK_OTA.zip system \
    --output output/pico-stock-system \
    --expected-sha256 88669fe996510a879305d980ed4ba626c908ccc06ecead466393c3eadb58a8b6 \
    --expected-device PICOA7H10 --expected-build smartcm.1696861506
```

This requires the `brotli` command and a new output directory. Authenticate the
OTA first; the supplied hash binds extraction to those checked bytes. The tool
accepts only full version-4 `new`, `zero` and `erase` transfers, checks complete
nonoverlapping block coverage and refuses incremental data. It streams one image
at a time with a partition-size-plus-2-GiB free-space check. Discarded ranges are
represented as zero-filled host holes; that does not specify their physical
contents after a device discard.

The b1977 system image reconstructs to 4,442,832,896 bytes with SHA-256
`8a2437adb10135fe0ed5c4a3a0db4e50caffe4a87ad5c799f2e4c73b695eacbb`.
Its data and stored hash tree verify against the signed `vbmeta_system` descriptor,
whose signing key matches the authenticated parent VBMeta chain. A changed root
digest is rejected. FEC repair, updater execution and device restoration remain
untested. Evidence is in `output/pico-block-ota-20261011-v1`. Selected driver files
are retained privately; all three earlier device-read controller libraries match
their reconstructed system copies byte for byte.

The package has an Android v2 boot image, six SoC base DTBs plus an RTIC data FDT,
and sixteen board overlays. Its signed VBMeta matches the boot and DTBO hashes;
the images' own `NONE` authentication headers do not authorize unsigned boot.
There is no standalone recovery image in this OTA, although VBMeta chains to
recovery. The unit's recovery and calibration partitions have now been acquired
separately, as described below.

Stock config enables KGSL, Binder, FUSE and OverlayFS, but omits SysV IPC,
user/PID/UTS namespaces, `binfmt_misc` and `devtmpfs`. It enforces module
signatures and has module versioning. A compatible custom kernel is needed for
the Linux userspace; stock modules cannot simply be reused with a changed ABI.
No storage mutation commands have been sent to the unit.

## Pico GPU firmware

The published Adreno 650 driver requests `a650_sqe.fw`, `a650_gmu.bin` and
the split `a650_zap` image. These files are present in the exact b1977 vendor
partition. Its reconstructed SHA-256 is
`5907191cfa33227c0a2ad93f1a9aad8bb18ebdeb80ff16cc287225814a57b249`.
The complete data and stored hash tree verify against the authenticated top-level
VBMeta descriptor; changing the root digest is rejected. Vendor uses the
top-level VBMeta, unlike system's chained `vbmeta_system`. FEC repair and device
restoration are outside this check.

Prepare the private bundle after authenticating and reconstructing the vendor
image:

```sh
python3 -B tools/prepare-pico-firmware.py VENDOR.img \
  --output NEW_FIRMWARE_DIRECTORY \
  --sha256 5907191cfa33227c0a2ad93f1a9aad8bb18ebdeb80ff16cc287225814a57b249 \
  --reference-build smartcm.1696861506
```

The preparer uses bounded, read-only `debugfs` extraction in an offline container.
It includes only the MDT, its two required segments, SQE and GMU files. It checks
segment lengths, aligned SQE words and the A650 GMU address ranges, allocations
and block boundaries. The GMU user range differs from Quest's Gen7 range.
The manifest records the source identity and each output hash; it does not claim
that a per-file signature or hardware compatibility was independently verified.
Firmware binaries remain outside Git.

Pass this bundle to `tools/build-headset-initramfs.py --pico-firmware DIRECTORY`.
The builder refuses a different kernel source revision, a Quest kernel or Quest
startup services. The physical kernel through patch `0030` now has an initramfs
containing all 38 matching modules and all five GPU firmware files. Its SHA-256 is
`7d029ef1e3a96f50fbd017a2505717a50ad3f9db6793f8f67f59cded241ffd1b`.
The separate QEMU variant passes volatile-root handoff and both missing-root
shutdown cases with unchanged backing disks. All 327 host tests pass, including
malformed bundles and cross-target refusal. Evidence is in
`output/pico-firmware-20261012-v1/`.

These checks close the early GPU firmware packaging gap. They do not execute
the firmware on the replacement kernel, prove panel output, supply radio
firmware or establish boot/recovery readiness.

## Audio, compute and sensor firmware

`tools/prepare-pico-dsp.py` extracts ADSP, CDSP and SLPI files from the locally
supplied, checksum-pinned `NON-HLOS.bin`. The 5.8.4.0 b1977 image omits the
unused tail of its FAT16 volume. The reader checks both allocation tables,
every file's declared bytes and cluster ownership before extraction; it rejects
truncation, cycles, shared chains and unreferenced allocations. It does not
modify or pad the input.

```sh
python3 -B tools/prepare-pico-dsp.py NON-HLOS.bin \
    --sha256 8d9a542b56549f17c3699caae2931189dd38cd11ff7f321bc557d31a19048e81 \
    --reference-build smartcm.1696861506 --output OUTPUT_DIRECTORY
```

The actual image's 3,730 allocated clusters are accounted for. All 55 packaged
files (28,635,187 bytes) match an independent 7-Zip extraction of a derived
sparse volume. Split MDT segment lengths and signing-metadata copies match;
the five service declarations identify ADSP instance 74, CDSP 76 and SLPI 90.
The selected stock device tree requests `adsp`, `cdsp` and `slpi` through
Qualcomm PIL with authentication IDs 1, 18 and 12 respectively. These are
firmware and interface checks, not verification that the replacement kernel
can start the DSPs or deliver audio and sensor streams.

Patch `0031-dsp-restart-handle.patch` rejects error-valued handles in the ADSP
and SLPI restart attributes. A failed firmware acquisition can leave such a
handle behind; the original handlers accept it and the restart entry
dereferences it. Tests using the actual source routines exercise all 4,095
kernel error-pointer values per handler: 8,190 invalid dispatches before the
fix, zero afterward. Valid restart and error-return behavior are preserved.
macOS and ARM64 Linux address/undefined-behavior sanitizer checks pass.

The full physical-configuration kernel through `0031` builds with all 38
modules. Artifact hashes, module signatures, imported symbol versions,
dependency closure and a modified-signature rejection check pass. The exact
Image also passes the diskless virtual ABI fixture and normal shutdown.
Evidence is retained under `output/pico-dsp-firmware-20261012-v1/` and
`output/kernel/pico-neo3/linux-dsp-restart-20261012-v1/`. Physical firmware
authentication, DSP startup, routing, sensor transport and recovery remain
unverified.

Pass `--dsp-firmware DSP_DIRECTORY` to `prepare-pico-firmware.py` to include
these files with the GPU bundle. The two reference builds must match. The
combined manifest retains both source-image identities and the DSP service
declarations; changing either component is rejected before boot assembly.
`build-headset-initramfs.py --pico-firmware` and the Pico root builder accept
this combined bundle and verify its complete file inventory after packaging.

The resulting physical initramfs contains 38 matching modules and 60 firmware
files. Its SHA-256 is
`128de360fbebdce14d5adfbaa63e22c3cbd4247b63de72fde80672320218732c`.
The existing root was cloned and updated offline, preserving the original;
the new root SHA-256 is
`6803b65ae04307b43fed298cd8e10bc8b536724bf02682bb75897a966dd69f3e`.
Re-extraction and filesystem checks pass. Its separate QEMU-module derivative
passes the packaged-root test with all 60 firmware files, 541 runtime entries,
128 command dependencies, user-session startup, service isolation and zero
backing-disk writes. The normal and two missing-root initramfs cases also pass.
The virtual board does not execute the physical DSP firmware.

The updated unsigned boot container passes stock roundtrip and component
checks, with 26,873,856 bytes of partition headroom. These artifacts are under
`output/headset-initramfs-pico-dsp-20261012-v1/`,
`output/headset-root-pico-dsp-20261012-v1/`,
`output/pico-root-dsp-acceptance-20261012-v1/` and
`output/pico-boot-container-20261012-v5/`. All 362 host tests pass. No image
has been installed on the headset, and no automatic DSP startup service has
been enabled by this packaging change.

## EDL identity and partition backups

A serial- and USB-port-guarded software EDL entry and Sahara query succeeded on
the b1977 Pro Eye. The ROM reports MSM ID `0x000ce0e1`, OEM ID `0x013a` and
model ID `0x0ae8`. Its full 48-byte root-key hash matches the Pico root
certificate in the official engineering programmer. That root and attestation
certificate also match the authenticated stock XBL, XBL configuration and ABL.

The programmer was acquired from the
[official Neo3 engineering package](https://static.us-pui.picovr.com/SEKSA-pico_rls_neo3-mol-tob-pui-4.8.0-20220622-falconcv3-user-20221017-225305-32g-b1981.zip).
Its size is 675,320 bytes and SHA-256 is
`d05711c81d07e426ab44a25dd9f4a391c0ddcbe251a935babb2532ca84ebdce1`.
Signature, certificate-chain and segment hashes pass offline checks. On October
9 the exact programmer was also accepted and executed in RAM on this unit.
Fresh ROM identity checks preceded each transfer. Firehose initialized UFS,
read geometry and bounded partition ranges, then acknowledged a reset. Stock
Android returned with the same firmware, locked boot state and enforcing SELinux.
No program, erase, patch, provisioning, unlock or slot-change command was sent.

All six UFS LUNs use 4,096-byte logical sectors. Both GPT copies passed header
and entry-array CRC, capacity, GUID and nonoverlap checks. Their 93 partitions
matched all 93 distinct Android by-name links. Boot, recovery, verification
metadata, early bootloaders, `persist`, `picocfg`, DDR configuration and their
selected backups were acquired: 21 partitions totaling 403,316,736 bytes.
Each partition matched two device reads and an independent host SHA-256 check.
These private backups are a subset of the device, not a complete storage image.

The current boot image matches the authenticated OTA exactly; DTBO and VBMeta
match their package payloads with additional partition padding. Recovery's
RSA4096 signature and image hash verify, and its key matches the signed VBMeta
chain. The preserved `persist` filesystem contains Tobii calibration, camera/IMU
calibration and sensor registry data. These files must remain private and
specific to this unit.

The `bootbak` bytes differ from current boot and fail the boot hash in
`vbmetabak`, which contains the current metadata. Preserve this original state;
do not assume the backup-named partitions form a working fallback. Verified
reads and a successful programmer reset do not establish restoration, unsigned
kernel acceptance or recovery after a failed boot-chain write.

The earlier Sahara reset acknowledged the request but did not leave EDL, and
subsequent USB descriptor reads failed. The owner subsequently returned to
Android by holding Power + Volume Down. The later Firehose reset round trips
succeeded; do not substitute the failed Sahara command. PICO documents a
hardware reboot by holding Power for
[more than ten seconds](https://www.picoxr.com/cn/neo3/pdf/PicoNeo3UserGuide.pdf).
Independent cold EDL entry and restoration still need validation on this unit.
macOS also requires approval when the Pico first changes to its Qualcomm USB
identity; a pending accessory decision prevents libusb enumeration.

The backed-up b1977 recovery is a user build with `ro.debuggable=0`; its init
configuration disables ADB by default and comments out fastbootd. Rebooting into
recovery does not establish an unattended USB return path. PICO's documented
[recovery entry](https://sdk.picovr.com/docs/FAQ/chapter_five.html) uses Power +
Volume Up + Home from power-off, followed by Power + Volume Up at the Android
robot. On October 11 the owner entered this menu from power-off using the
buttons and selected **Reboot system now**. The Pico returned to its normal
home screen. ADB then confirmed boot completion, unchanged 5.8.4.0 firmware and
kernel, a new boot ID, locked/green verified boot and enforcing SELinux. No Pico
ADB connection appeared during the observed recovery menu. Evidence is retained
in `output/pico-cold-recovery-20261011-v1`.

This verifies button access to the installed stock recovery and a normal return
to Android. It does not verify recovery after damaged boot-chain partitions,
cold EDL entry, partition restoration or unsigned custom boot. Those checks
remain required before flashing.

On October 12, holding both volume buttons while reconnecting the ordinary
USB cable did not establish EDL entry; the owner reported normal boot with
uncertain button timing. Power + Volume Down + Home from power-off then reached
the actual fastboot menu. USB remained on the same physical port but changed
its serial, which must be selected separately from the Android ADB serial.
Read-only queries reported `product: kona`, `secure: yes`, `unlocked: no` and
`is-userspace: no`.

The Neo3 community tool's inspected `reboot-edl` command was acknowledged with
`OKAY`, but no 9008 endpoint was observed during the following 25 seconds. The
Pico returned to unchanged 5.8.4.0 stock Android, locked/green and enforcing.
No RAM programmer was uploaded and no partition operation was issued. This
establishes button-entered fastboot and normal return, not EDL or restoration
after early-bootloader damage. Receipts and the statically inspected community
client are retained in `output/pico-cold-edl-20261012-v1`; the downloaded Windows
client was not executed.

A subsequent RAM-only boot test used the exact current 96 MiB stock `boot`
backup, already matched to the authenticated b1977 OTA. Fastboot accepted the
download, then rejected execution with `oem command is not allowed without
unlocktoken authority`. A normal fastboot reboot returned the unit to the same
5.8.4.0 firmware, locked/green verified boot and enforcing SELinux; the connected
Quest did not reboot. No flash, unlock, erase or slot-change command was issued.
The response alone does not distinguish an authorization failure from an
unsupported command. The subsequent static analysis below resolves that
ambiguity. The image hash, command responses and return checks are in
`output/pico-stock-ram-boot-20261012-v1/result.json`.

When multiple ADB servers are running, the Pico can reconnect to a different
server after reset. Check the existing servers for its exact serial before
diagnosing a failed return to Android.

Validate acquired primary and backup GPT headers and entry arrays before using
their extents for partition acquisition:

```sh
python3 -B tools/inspect-gpt.py primary-header.bin primary-entries.bin \
    backup-header.bin backup-entries.bin \
    --sector-size LOGICAL_BLOCK_BYTES --total-sectors ACTUAL_LUN_LOGICAL_SECTORS
```

These are regular host files: each header occupies one logical sector; each
array contains all its complete sectors. The tool checks header/array CRCs,
locations, capacity, matching tables, partition GUIDs, bounds and overlap. It
reports duplicate labels as ambiguous rather than selecting one. It never
opens a device or repairs a table. Entry arrays are capped at 1 MiB.
The format follows the [UEFI GPT specification](https://uefi.org/sites/default/files/resources/UEFI_Spec_2_8_final.pdf).

Use independently observed LUN geometry. Linux sysfs `size` counts 512-byte
units; Firehose counts logical sectors. Convert through bytes before comparing
them. The physical reads above establish this unit's geometry and the recorded
backup subset. Independent emergency recovery, remaining restoration inputs and an
actual restore procedure remain necessary before persistent changes.

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

Offline analysis of the exact b1977 ABL finds 15 registered fastboot commands
and 14 additional OEM commands. Neither table contains `boot`. The dispatcher
at `0x309e4` uses the observed unlock-token error for unmatched commands too;
it is not evidence that a token would enable RAM boot. Commands use prefix
matching, and the registered `reboot` handler requests a normal reboot without
inspecting the remaining text. This explains the acknowledged `reboot-edl`
followed by stock Android. The separate `oem reboot-edl` handler sits behind
the OEM authority check. Repeating the plain command cannot verify recovery.

The official Neo3 4.6.3 b678 ABL was acquired using bounded ZIP ranges and
matched byte-for-byte to the pinned community tool's bundled ABL. Its SHA-256
is `446eb09a169eb8e8a25f3ecb9865c08b7ac8cd7636d366a8c450d59dfdbe118a`.
All ELF segment hashes, its RSA-PSS signature and certificate chain verify;
modified signed data is rejected. Its root matches both the authenticated
b1977 images and the unit's previously observed Sahara root hash. The old and
current ABLs also carry identical signed OEM metadata. These checks establish
provenance, not live rollback acceptance, Pro Eye boot compatibility or restore
capability. Static exports and signature receipts are retained in
`output/pico-stock-abl-20261012-v1/`; current-source research and acquisition
receipts are in `output/pico-unlock-research-20261012-v1/`.

The early ABL has a distinct command path: its serial-derived authorization
handler at `0x32268` registers nine additional commands, including `boot` at
`0x33a64`. That handler itself registers commands and acknowledges the request;
ordinary and critical unlocking are separate handlers. The boot handler still
loads and authenticates the image, and contains an A/B-slot metadata operation
on applicable devices. Command availability must not be confused with unsigned
boot acceptance or absence of persistent effects. The early ABL must first be
installed before this path exists, so it does not remove the independent
recovery requirement. A complete community unlock/factory-reset script is not
required merely to investigate that distinction offline.

## Offline boot container

A diagnostic Android-v2 container now combines the verified `0030` kernel,
its matching initramfs and the unmodified current stock DTB pack. Pinned AOSP
tools unpack and repack the authenticated stock payload byte-for-byte before
assembling the Linux payload. Pico retains a nonzero `second_addr` despite
`second_size=0`; current AOSP packing clears that inactive field. The assembler
explicitly preserves only that reference field, then requires exact equality.
Both earlier failed roundtrip attempts remain in the evidence.

The resulting unsigned container is 62,119,936 bytes, leaving 38,543,360 bytes
within the observed boot partition size. Unpacking it reproduces the selected
kernel, matching initramfs and original DTB bytes exactly. Its SHA-256 is
`d5a4f16356edf96e766640d6f134095245e4243021bf1d79ac3854d02630515a`.
The initramfs includes the selected kernel's modules and the previously tested
Pico-compatible systemd runtime. Its command line requests a read-only labeled
root with a volatile overlay and powers off on root-mount failure.

This is an offline packaging diagnostic, not an installable OS release. It
does not include a root filesystem, activate the candidate panel/mux binding,
verify live DTB selection, or establish that the early ABL accepts the image.
No device was contacted during assembly. The script, source hashes, command
logs and roundtrip receipt are retained in
`output/pico-boot-container-20261012-v3/`; the matching initramfs is in
`output/headset-initramfs-pico-regulator-20261012-v1/`.

## Kernel build and stock driver dependencies

The pinned public source now builds an ARM64 `Image` and 38 modules with Debian
Clang 14.0.6. The build uses the published `kona-perf_defconfig`, not the captured
stock configuration. It takes about ten minutes on two host cores, with a
measured peak of 4.60 GiB under a 6 GiB limit. Reproduce it in a new output
directory after building the kernel tools image:

```sh
just build-vendor-kernel output/kernel/pico-neo3/vendor-build pico-neo3
```

The build verifies the source archive, applies platform portability/correctness
patches, checks that source files stay unchanged, and records image, module,
configuration and toolchain hashes. The patches include Python 3 compiler-wrapper
support, driver error cleanup, packed-type symbol-version generation and
freestanding compilation. Module versioning and the vendor warning guard remain
enabled. Fault-injection checks cover LED registration and fan GPIO cleanup.

The build includes the 27 external audio modules named in the stock loaded-module
inventory, alongside 11 in-tree modules. It copies the published audio source to
an isolated build directory, resolves its three kernel-header links, and uses
the vendor's Kona configuration. One module-linking pass resolves the audio
modules' circular imports. The fixed-size debug buffer uses a C integer constant
expression so Clang accepts it without disabling the vendor warning policy.
The unconfirmed `PICOVR_US_EURO_HEADSET` manufacturing flag remains unset.

All 38 module signatures verify against the certificate embedded in the built
kernel; a modified signed payload is rejected. Imported symbol versions and
module dependencies also pass offline checks. `Module.symvers` and
`audio-Module.symvers` are exported for subsequent ABI checks. These results do
not establish compatibility with stock modules, DSP firmware, audio routing,
calibration or physical playback/capture.

This configuration differs from the captured stock configuration at 80 requested
symbols. The public defconfig's `VXR7200_I2C` request disappears during Kconfig
resolution; stock instead has `DISPLAY_VXR7200_I2C`, whose implementation is also
absent. Stock controller-station and Pico RTT support are absent from that build.
Later [stock-kernel analysis](pico-display-protocol.md) identifies the bridge's
register protocol and board controls, and classifies RTT as crash diagnostics.
The maintained register transport passes offline comparisons, but a bound
replacement bridge driver remains incomplete. The
published board DTS is absent, so this build deliberately produces no device
trees. A separate QEMU variant enables PL011, virtio-mmio, block, network,
console, random and RTC transports without applying Quest-specific patches.
It builds 40 modules; these virtual transports are excluded from hardware
builds. None of these artifacts has booted on the headset.

The maintained SPI transport patch fixes an uninitialized IRQ cookie, ignored
GPIO/IRQ failures, incomplete removal, and the camera helper overwriting the
character device's owned transmit buffer. Probe failures release only acquired
resources and preserve deferred-probe errors. Camera transfers serialize with
removal; detached readers wake with an error, and a replacement controller cannot
bind until old descriptors close. Failed user copies cannot change GPIO or MCU
power. The vendor's singleton controller protocol and its GPIO bindings remain.

`tools/test-pico-controller.py PINNED_SOURCE/drivers/spi/spidev.c --output NEW_DIRECTORY`
checks the original source hash, applies the maintained patch and compiles the
actual changed functions into a fault-injection harness. Its 133 cases pass with
address/undefined-behavior sanitizers on macOS and ARM64 Linux, including 64
concurrent camera-write/removal cycles. The unchanged original camera function
fails separate missing-device and buffer-ownership controls. This tests resource
lifetime and API behavior; it does not verify physical SPI transfers, pairing,
controller tracking or station interrupts.

The subsequent [station notification driver](pico-controller-protocol.md#station-interrupt-driver)
reconstructs the stock GPIO/poll interface with deferred-probe handling and
safe open-file lifetime across removal. Its source passes sanitizer lifecycle
tests, ARM64 object compilation and configuration resolution. A subsequent full
physical-configuration build includes it as `CONFIG_PICO_STATION=y`; its Image
SHA-256 is `0e9a9cff603acc08107b7332f6e1aa39520dfa3b54f25367e4b68f8936287a53`.
All 38 module signatures, 2,715 imports and dependencies verify. The unchanged
diskless ABI fixture passes in 5.539 seconds after verifying the loaded Image.
This does not exercise physical GPIOs or establish headset boot/recovery.
Evidence is retained in `output/pico-station-kernel-20261011-v1` and the build
in `output/kernel/pico-neo3/linux-station-20261011-v1`.

The physical-configuration Linux build including this patch passes all 38 module
signatures, 2,715 imported symbol versions and dependency checks. Its exported
Image has SHA-256
`ba312ea1e248da1a8ede6b052a5ab96f17382235185702d27255405bf7b1d32c`.
A diskless virtual boot verifies the complete loaded Image and passes the
Android namespace, Binder and tagged-pointer/FUSE regression in 5.654 seconds,
then powers off normally. The build excludes QEMU transports. Results are in
`output/pico-controller-lifecycle-20261011-v1`; this does not establish physical
controller operation or headset boot.

The maintained Linux variant adds the common Linux-userspace configuration and
an Android 4.19 BinderFS backport. Build it with:

```sh
just build-headset-kernel output/kernel/pico-neo3/linux-build pico-neo3
```

This variant also compiles all 38 modules. Signature verification, all 2,715
imported symbol versions, dependency checks and rejection of a modified signed
payload pass against its own kernel. All requested Linux configuration options
resolve. The public source and patched source remain unchanged during the build.
This is a kernel build with Linux/container interfaces, not a complete headset OS.

The Binder port uses Google's [Android 4.19 source](https://android.googlesource.com/kernel/msm/+/500b3a2059c1eec0fec08ec56add06320ea901a8/drivers/android/),
with the associated credential-based SELinux hooks and `wake_up_pollfree`
backports. The Linux variant disables Android's special internet-group check,
which otherwise rejects ordinary Linux users' sockets. The vendor variant keeps
that restriction, with a scoped correction for kernel-created IPv4/IPv6 sockets:
IGMP namespace initialization must not be rejected by the calling user's group
check. Tests confirm the vendor restriction remains effective for user sockets.

Diskless execution also exposed a vendor linker defect: a second `.bss` output
placed RTIC's `selinux_state` after `_end`, outside the mapped kernel. The
Pico patch uses `BSS_FIRST_SECTIONS` to preserve its page alignment and `KEEP`
while including the state in BSS initialization and the Image's memory extent.
The build checks all allocated ELF sections against the Image bounds and
requires uninitialized sections to fall within the zeroed BSS range. The
original failing ELF is rejected; corrected Pico and existing Quest kernels pass.

The actual Linux build boots a static PID 1 and passes the shared container ABI
fixture under QEMU TCG. An unprivileged user creates independent namespaces;
IPv4/IPv6 datagrams and isolated loopback traffic work, while raw sockets remain
denied. Two BinderFS mounts provide independent contexts; real request/reply,
file-descriptor passing, sender identity and 32 epoll/thread-exit cycles pass.
Shared-memory seals and seccomp also pass, followed by normal shutdown. Native
rootless OverlayFS still returns `EPERM`. Rootless Podman with `fuse-overlayfs`
passes the separate [writable-mount lifecycle](android-containers.md): three
overlay views, persistent copy-up/whiteouts, independent contexts, unchanged
lower files and complete helper cleanup. These tests do not boot Android or
Lepton on the Pico kernel.

The fixture needs an emulated secure monitor, 2 GiB RAM to cover the vendor's
fixed crash-log reservation, and an explicit exclusion of `hwinfo_init`, which
otherwise dereferences absent Qualcomm boot-information memory. WALT topology
and missing SoC-information warnings remain. These VM accommodations do not
establish physical driver compatibility.

The QEMU variant is built with `just build-kernel-abi OUTPUT pico-neo3`.
A bare `earlycon` boot argument exposed a vendor NULL dereference before
userspace: Pico's console-off extension accessed the optional argument before
upstream device-tree/SPCR fallback handling. The common Pico patch now checks
for NULL while preserving `earlycon=off`. Three isolated boots with bare,
explicit PL011 and off arguments each reach the Frame service probe described
below and shut down normally. The original panic remains in the local evidence.

The current Fedora initramfs exposed two additional compatibility failures on 4.19.
A syscall trace shows `statx` succeeds without mount IDs, followed by an
unsupported file-handle lookup and an unavailable `/proc/self/fdinfo` fallback
before procfs is mounted. Systemd cannot mount its early API filesystems.
A diagnostic-only procfs pre-mount advances startup, but every service executor
fails: glibc's `pidfd_spawn` probes `waitid(P_PIDFD)`, which returns `EINVAL`,
and reports `ENOSYS` to systemd.

The Linux variant now backports mount IDs and mount-root attributes from
upstream Linux, preserving the 256-byte `statx` ABI. Process descriptors use
the final Linux 5.4 stable implementation, adapted to Pico's fork and signal
interfaces: open, clone, poll, signal and `waitid(P_PIDFD)` work in both ARM64
and ARM32 compatibility mode. The virtual regression covers permission and
signal checks, close-on-exec, repeated exit/reap cycles, bad-pointer and full
descriptor-table cleanup, and a thread-group leader exiting before its last
thread. The older Pico kernel fails the negative control; the Quest kernel and
patched Pico kernel pass. This does not add `clone3` or change the vendor-only
build.

The physical configuration also compiles with these backports and all 38
modules. Its unchanged Image passes the ARM64 and ARM32 process/mount tests in
diskless QEMU, with normal powerdown and no added kernel taint. The diagnostic
reads the RAM log because this configuration has no virtual PL011 console;
it does not add virtual transports to the headset kernel.

The subsequent build also includes the
[tagged-address string and memory corrections](android-containers.md).
`output/kernel/pico-neo3/linux-tagged-memory-20261011-v1` compiles all 38 modules;
its Image has SHA-256
`b2cc8d1ee4c5a38d37fe1e274ef01f3f89d105445cdb0e595a9d7f68f8ca07f8`.
The diskless test repeats the ARM64/ARM32 process checks and adds unprivileged
tagged exec and memory checks, including execution of generated ARM64 code.
It passes in 5.574 seconds with unchanged taint and normal powerdown
(`output/pico-physical-memory-20261011-v1/result.json`). This is execution of
the headset configuration on a virtual board, not a physical boot or recovery
test.

The page-pinning correction also compiles with the physical configuration and
all 38 modules (`output/kernel/pico-neo3/linux-tagged-fuse-20261011-v1`). Its
Image SHA-256 is
`fe7cfbc1dcab6abb64226fbf3c653f6ab80239a9c5af7af7d034003f449e2fdc`.
The diskless Android ABI fixture passes in 5.501 seconds, including tagged
execution, memory and FUSE buffers, with unchanged taint and normal powerdown
(`output/pico-physical-fuse-20261011-v1/android-abi/result.json`). No virtual
transport drivers were added to this configuration. The separate transport
build passes the [two-launch Android storage test](android-containers.md#pico-application-storage-and-launcher-shutdown);
neither test boots a physical headset.

Fedora now mounts its API filesystems, starts service executors, and switches
to a RAM overlay root with zero backing-disk writes. The overall boot test is
still rejected: systemd259 credential setup calls `fsopen`, which this kernel
does not implement. Journal, sysctl and temporary-file setup consequently fail.
The missing-root case powers off without changing its disk, but its expected
diagnostic is unavailable because logging failed. The test runner now rejects
failed services even when the root-handoff marker is present. No production
pre-mount workaround or disabled credential protection is used.

The unchanged Pico Linux Image also runs the official Frame repair image's
systemd `257.7-2.2` as PID 1 in a separate diskless fixture. Its manager and
executor start a service in cgroup v2 with `NoNewPrivileges`, private network
and mount namespaces, private temporary files and `ProtectSystem=strict`.
The probe verifies that a write under `/usr` fails with `EROFS` and its temporary
file is absent from PID 1's view. Systemd then synchronizes, detaches filesystems
and powers down. The complete test passes in 5.7 seconds under TCG.

The same Valve binaries also pass a credential-isolation test on the patched
virtual kernel: the service reads its supplied credential, attempts to truncate
it and receives `EROFS`, then passes the existing namespace and filesystem
checks before normal shutdown. This 5.6-second test establishes a compatible
credential path, not a complete Frame root filesystem boot.

A subsequent diagnostic initramfs uses those matched Valve systemd commands,
their private dependency closure and original service units with the existing
dracut scripts. It passes normal root handoff and the missing-root shutdown
case, with no failed services and byte-identical backing disks. Journald, udev,
sysctl and temporary-file setup start successfully. The normal case also
reports zero block writes. Small static ELF launchers retain each command's
argument zero and select its private loader; shell wrappers fail because
systemd opens its executor through a close-on-exec file descriptor.

The maintained initramfs builder now accepts a prepared private systemd runtime
with `--systemd-runtime`. Preparation pins the original Frame repair filesystem,
resolves the ARM64 library closure, compiles static launchers and records each
command, unit, mode, link and source checksum. Installation requires an exact
match with the original command inventory and the units selected for replacement.
Dracut's emergency and rescue units are preserved so `rd.emergency=poweroff`
continues to use its emergency handler. Replaced units are separated from any
hardlinks before installation, and the final archive is unpacked and verified
again. Unexpected files, changed inputs and escaping links are rejected.

A build through this path passes both virtual boot cases with no failed services
and unchanged disks. A follow-up host repack verifies preservation of dracut's
emergency units and correct dispatch to `systemd-shutdown` when the launcher runs
as PID 1. Both boot cases pass again in 5.58 and 10.96 seconds. Fifteen focused
runtime/launcher tests and the complete 271-test host suite pass. The newer, unused
`systemd-validatefs` helper remains from Fedora and is explicitly recorded;
Frame257 does not supply it. None of these results establishes physical boot,
recovery or flash readiness.

The builder also requires `sysroot.mount` before starting the volatile-root
service. Without that dependency, an early device timeout could start it after
the root mount failed. The regression now forces both timeout paths separately:
dracut's initqueue deadline and systemd's device deadline. The preserved old
initramfs fails the latter case; the corrected image passes normal handoff and
both failures with no failed services, unchanged disks and normal power-off.
Receipts: `output/pico-tagged-root-handoff-negative-20261011-v1` and
`output/pico-tagged-root-handoff-20261011-v3`.

To prepare this input locally, use Python with `dissect.btrfs`, Zig, the preserved
original Frame `rootfs-A.img`, and an unpacked initramfs from the same builder
image. The source image must match the checksum pinned in the preparation tool.
Keep the resulting bundle private; the repository contains the preparation and
installation tools, not Valve's binaries.

```sh
mkdir initramfs-template
tar -xf path/to/base-initramfs/initramfs.img -C initramfs-template
python3 tools/prepare-systemd-runtime.py path/to/rootfs-A.img \
  --template initramfs-template --output output/pico-systemd-runtime
python3 tools/build-headset-initramfs.py path/to/pico-kernel \
  --systemd-runtime output/pico-systemd-runtime \
  --image matching-builder-image:tag --output output/pico-initramfs
```

The initial fixture was a minimal manager/service test. The shipped binary warns
that its required kernel baseline is 5.4; the vendor 4.19 kernel therefore still
needs compatibility testing beyond individual successful services.

### Full-root runtime

Preparation now also accepts `--scope root`. This variant keeps Fedora's native
loader and library precedence so its existing PAM modules load without changing
authentication policy. The installer pins the native loader and libc, requires
the matching original command inventory, and preserves non-systemd service files
and local overrides. It masks four newer varlink sockets absent from Frame257.
Eleven optional Fedora commands remain explicitly inventoried; their additional
features are not covered by this acceptance test.

```sh
python3 tools/prepare-systemd-runtime.py path/to/rootfs-A.img \
  --scope root --template path/to/matching-root --output output/pico-root-systemd
python3 tools/systemd-runtime.py output/pico-root-systemd \
  --install path/to/offline-root
```

The prepared bundle in `output/pico-root-runtime-20261011-v1` has manifest SHA-256
`d608c920591985cce0304caf53d54e3f47de278782c7e756afc6d4df5a96fd26`.
A fresh Pico VM boots directly into this runtime, reaches multi-user with zero
failed services, and starts `user@1000` through unchanged PAM. All 541 installed
files and links and all 128 command dependency closures pass verification.
Service tests confirm `NoNewPrivileges`, a read-only `/usr`, and a private `/tmp`.
The fourteen-patch Lepton bundle then boots Android in 91.14 seconds under CPU
emulation and shuts down cleanly in 13.96 seconds, removing its temporary data.
No post-boot launcher replacement or manager re-execution is needed. All 277 host
tests pass, including 19 focused runtime and launcher checks.

Evidence is retained in `output/pico-root-runtime-20261011-v1` and
`output/pico-lepton-acceptance-20261011-v8`. This tests the maintained runtime in a
RAM overlay on the preserved Linux root. A complete Pico image builder, board
startup, hardware drivers, graphical performance, physical boot and independent
recovery remain required.

A separate test on the same Linux build loads the unchanged recovery SELinux
policy. A normal kernel-to-init transition succeeds; the enforcing policy denies
a root write to a DAC-writable tmpfs file, preserves its bytes and powers off.
A legacy-node build of the Binder port also verifies context-manager denial
using the current caller and each handle's opening credentials. That fixture
uses an allowed character-device label to isolate the Binder hook; production
BinderFS labeling and Android startup under the stock policy remain unverified.
No policy or enforcement setting is relaxed.

Stock OTA Wi-Fi modules identify version `2.0.81.1H` for QCA6390 and QCA6490,
matching the version in Pico's published driver source. The public release
omits its required `wlan/fw-api` headers. The tested Xiaomi `cmi-r-oss` header
set lacks roaming and datapath-statistics interfaces required by Pico's source;
it is incompatible and is not included in the maintained build. Obtain a
coherent matching release rather than removing those interfaces to make it compile.

A separate QCA6390 experiment now compiles against the stock-derived Linux
kernel using newer
[Xiaomi `dagu-s-oss` headers](https://github.com/MiCode/vendor_qcom_opensource_wlan/tree/b7eb630ef0f4ef2058bc94cff72f65168a2c2ae3/fw-api)
and pointer-width corrections in three driver source files. All 480 imported
symbol versions match; the signed module verifies against the built kernel's
embedded certificate and rejects a modified payload. The exact stock firmware
message ABI, active chip variant and radio operation remain unverified; reading
the unit's PCI identifiers is denied. QCA6490 has not been compiled. This
experiment is retained separately and is not part of the maintained build.

Read-only stock observations narrow the hardware work:

| Component | Observed on this Pro Eye | Remaining integration |
|---|---|---|
| Graphics/display | KGSL, both DSI controllers and `vxr7200_i2c` at I2C `0-0039` are bound | Resolve the missing bridge implementation and actual panel/mode ownership; driver binding alone does not prove a replacement display path |
| Controllers | SPI `spidev` transport, `/dev/stationdev`, and a running `pxrcontrollerservice`; [SLPI key decoder and rebuilt station driver](pico-controller-protocol.md) pass offline checks | Complete SSC transport; verify physical station IRQs, inputs, pairing, haptics and optical tracking |
| IMU/tracking | Qualcomm SSC, TDK `icm4x6xx` sensor entries, and running `pvrtrackingservice` | Establish calibrated sample/camera timing and the Linux tracking interface |
| Eye tracking | Running `pxreyetrackingservice`; Tobii/Pico libraries and preserved calibration | Establish camera ownership, service dependencies and the gaze interface |
| Audio/WLAN | 29 loaded stock modules: 27 audio, WLAN and `msm_11ad_proxy` | Audio modules now build and pass offline ABI/signature checks; routing, DSP and physical audio remain. Wi-Fi needs matching firmware API headers and validation |

Camera HAL advertises five logical devices while six camera sensor nodes are
bound. Do not infer a one-to-one physical camera mapping from those counts.
The current shell cannot read the live FDT or DRM status/modes; exact tree
selection and display topology remain unresolved. Vendor runtime files and
per-unit calibration stay outside the public repository.

## Work remaining compared with Quest

| Workstream | Quest 3 | Neo3 Pro / Pro Eye |
|---|---|---|
| Firmware and recovery | Current-build OTA authenticated; exact root target audited. Both-slot/unit backups and independent restore remain | Exact Pro Eye OTA authenticated; programmer execution, all GPTs and 21 boot/calibration backups verified. Button entry to stock recovery and return to Android pass. Cold EDL and damaged-boot restoration remain. Pro needs separate inventory |
| Temporary custom boot | Quest3-specific loader ABI/CFI, live RAM allocation and peripheral handoff remain | Stronger public unlock lead; exact enterprise unlock persistence and accepted custom boot remain untested |
| Kernel and board description | Vendor kernel, modules and selected overlays compile; Linux userspace patches and QEMU ABI tests pass | Linux 4.19.81 builds 38 modules, including audio. Kernel ABI, enforcing-policy denial, maintained systemd root startup and Android lifecycle tests pass in QEMU; stock DTB/DTBO acquired. Board DTS, missing drivers and physical compatibility remain unverified |
| GPU, display and platform I/O | KGSL/Turnip and display integration compile; actual panel/GPU, DSP, audio, radios, storage and power acceptance remain | Adreno650/display and the same platform-I/O categories need an exact board port and hardware acceptance |
| Tracking and controllers | Syncboss/IMU protocol research and virtual actions exist; calibrated camera/IMU 6DoF, pairing and physical input/haptics remain | Camera/IMU transport, calibration, 6DoF and controller integration remain; Pro Eye adds eye cameras, calibration and runtime support |
| Installation | No accepted boot/recovery or persistent installer | No accepted boot/recovery or persistent installer |

The [official Neo3 kernel](https://github.com/bytedance/neo3-kernel/tree/9cacc1c1356dd32a048f4cb13bc84af402bd3326)
includes vendor graphics and platform code but omits the board DTS directory;
the [source profile](../profiles/kernel/pico-neo3.json) refuses that incomplete
source DT build. Authenticated stock trees provide board inputs, but do not
establish compatibility with the older public source. Frame's actual systemd
257.7 now passes full Linux startup and Android lifecycle tests on 4.19 through
the maintained root provider described above. Unmodified Fedora/systemd 259
requires kernel APIs absent from this vendor tree. See the
[Frame firmware comparison](steam-frame.md#october-8-firmware-comparison).

Both targets share the remaining native SteamVR compositor, stock Library
interaction, renderer/shutdown reliability, Lepton graphics/XR and clean
shutdown, complete-image, game, audio and sustained-performance work. Existing
virtual acceptance is retained in [VR milestones](vr-milestones.md).

A minimal Linux USB/console boot with a demonstrated return to stock could be
an earlier Pico milestone if recovery and unlock checks pass. The connected Pro
Eye is the selected development unit; preserve its eye calibration even before
eye tracking is integrated. A usable
standalone VR system still needs the large display/tracking/controller port.
Public unlock access alone does not establish that Pico reaches that milestone
sooner. Estimate further implementation only after actual boot and sensor/display
access establish which vendor components can run under Linux.
