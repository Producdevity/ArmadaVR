# Pico Neo3 Pro and Pro Eye firmware and boot access

Checked October 9, 2026. Quest 3 remains the overall priority; Neo3 Pro Eye
bring-up is active on the connected development unit. It was identified
physically as Pro Eye; its generic Android model alone does not distinguish
enterprise variants. Neo3 Pro requires its own inventory and calibration backups.

## Current Pro Eye baseline

The connected Pro Eye reports global/overseas 5.8.4.0, internal build
`c000_rf01_bv1.0.1_sv5.8.4.0_202310092224_neo3_b1977_user`, incremental
`smartcm.1696861506`, Android 10 and Linux `4.19.81-perf+`. It has about 8 GB
RAM, Adreno 650 v3, a `SHARP493` panel property and eye-tracking support property.
These are inventory observations, not physical driver acceptance.

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
robot. This sequence and cold EDL entry have not been validated on this unit.

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
backup subset. Independent recovery entry, remaining restoration inputs and an
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
absent. Stock controller-station and Pico RTT support are missing too. The
published board DTS is absent, so this build deliberately produces no device
trees. The separate QEMU-transport variant is still refused for Pico.
None of these artifacts has booted on the headset.

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
rootless OverlayFS still returns `EPERM`. These tests do not boot Android or
Lepton on the Pico kernel.

The fixture needs an emulated secure monitor, 2 GiB RAM to cover the vendor's
fixed crash-log reservation, and an explicit exclusion of `hwinfo_init`, which
otherwise dereferences absent Qualcomm boot-information memory. WALT topology
and missing SoC-information warnings remain. These VM accommodations do not
establish physical driver compatibility.

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
| Controllers | SPI `spidev` transport, `/dev/stationdev`, and a running `pxrcontrollerservice` | Preserve the transport and reconstruct the missing station interface, pairing, input and haptics |
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
| Firmware and recovery | Current-build OTA authenticated; exact root target audited. Both-slot/unit backups and independent restore remain | Exact Pro Eye OTA authenticated; programmer execution, all GPTs and 21 boot/calibration backups verified; stock return after reads passes. Independent cold recovery and restoration remain. Pro needs separate inventory |
| Temporary custom boot | Quest3-specific loader ABI/CFI, live RAM allocation and peripheral handoff remain | Stronger public unlock lead; exact enterprise unlock persistence and accepted custom boot remain untested |
| Kernel and board description | Vendor kernel, modules and selected overlays compile; Linux userspace patches and QEMU ABI tests pass | Vendor and Linux 4.19.81 builds compile 38 modules, including audio. Binder, namespace networking and SELinux denial tests pass in QEMU; stock DTB/DTBO acquired. Board DTS, several stock drivers, physical compatibility and full Linux runtime remain unverified |
| GPU, display and platform I/O | KGSL/Turnip and display integration compile; actual panel/GPU, DSP, audio, radios, storage and power acceptance remain | Adreno650/display and the same platform-I/O categories need an exact board port and hardware acceptance |
| Tracking and controllers | Syncboss/IMU protocol research and virtual actions exist; calibrated camera/IMU 6DoF, pairing and physical input/haptics remain | Camera/IMU transport, calibration, 6DoF and controller integration remain; Pro Eye adds eye cameras, calibration and runtime support |
| Installation | No accepted boot/recovery or persistent installer | No accepted boot/recovery or persistent installer |

The [official Neo3 kernel](https://github.com/bytedance/neo3-kernel/tree/9cacc1c1356dd32a048f4cb13bc84af402bd3326)
includes vendor graphics and platform code but omits the board DTS directory;
the [source profile](../profiles/kernel/pico-neo3.json) refuses that incomplete
source DT build. Authenticated stock trees provide board inputs, but do not
establish compatibility with the older public source. Frame's actual systemd
257.7 is a userspace candidate for 4.19;
the current Fedora/systemd 259 environment needs a newer kernel or a different
userspace baseline. See the [Frame firmware comparison](steam-frame.md#october-8-firmware-comparison).

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
