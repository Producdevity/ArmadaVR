# KGSL Turnip build and offline packaging

The headset graphics backend is native AArch64 Mesa Turnip using Qualcomm's
KGSL kernel interface. FEX is used separately for x86 applications; the native
ARM64 SteamVR runtime is now available for integration. The Android hardware
tests below establish bounded GPU execution on both headsets under their stock
kernels. Physical Linux display acceptance remains open.

```sh
bash tools/build-mesa.sh NEW_MESA_DIRECTORY turnip
python3 -B tools/build-headset-root.py KERNEL_BUILD STARTUP_INITRAMFS \
  --firmware QUEST_FIRMWARE --image RUNTIME_IMAGE \
  --turnip NEW_MESA_DIRECTORY --output NEW_ROOT_DIRECTORY
```

The pinned Mesa 26.1.8 archive is checked by size and SHA-256. The build uses an
immutable ARM64 container image, a source cache keyed by profile, patches,
builder and image, and no network during compilation. Existing outputs are
refused. Build and test logs, source identities and artifact hashes are retained.

The Turnip patches fix fence ownership and wait-any behavior, implement
an opt-in KGSL-to-DRM synchronization bridge, enable that bridge in the
KGSL-only build, connect an explicitly selected DRM display to KGSL WSI, and
use the modern KGSL GPU-object allocation interfaces. Submission still uses KGSL. The DRM device supplies binary and
timeline synchronization objects; it does not replace the KGSL GPU driver.

The bridge requires `-Dfreedreno-kgsl-drm-sync=true` at build time and
`TU_KGSL_DRM_SYNC` pointing to a suitable DRM device at runtime. The device must
support the required synchronization features. Its actual path and capabilities
must be checked on the target; a numbered render node is not a device identity.

## Correction to earlier build evidence

The September 10 `output/mesa/turnip-drm-v5/` driver compiled with
`MESA_SYSTEM_HAS_KMS_DRM=0` and without `HAVE_LIBDRM`. Mesa's KGSL-only platform
logic disabled libdrm before the target dependency was added. Consequently the
conditional bridge was absent from the final shared library, despite successful
compilation and separate helper tests.

Patch 0004 adds an explicit Meson option that preserves DRM support for this
configuration and makes libdrm required. The builder now requires the actual
KGSL compiler command to enable DRM, the finished ELF to link libdrm, and the
bridge's runtime option to be present in that ELF. Compile and dynamic-link
records are included in the artifact hashes.

`output/mesa/turnip-drm-v7/` passes these checks, all three ASan/UBSan helper
suites and five Freedreno tests. `output/turnip-bridge-audit-v1/` retains the
old compiler command and library identity, verifies rejection of old, damaged,
foreign-architecture and inconsistent bundles, and records loading the corrected
library in the pinned userspace. Vulkan reaches the library and reports no GPU
when no KGSL device is exposed.

The earlier QEMU test of real DRM/software fences remains valid for its exact
helper header, which is unchanged. It covers cross-process opaque descriptors,
future timeline points, merged dependencies and cleanup. The submission-wrapper
suite models KGSL calls. Neither test demonstrates actual GPU completion.

## Root contents

`--turnip` validates the build, source/profile/patch/test identities, artifact
checksums, ELF architecture and installed ICD path. The root receives the
library and ICD under `/opt/reframed-vr/turnip/`, plus the build record under
`/usr/share/reframed-vr/`. Loading with `RTLD_NOW` inside the assembled userspace
must succeed. After formatting, the builder extracts these files from the ext4
image and compares their hashes with the validated bundle.

Packaging does not select Turnip globally or start a physical VR session.
The existing QEMU software-rendering lab remains available. Headset session
selection, display scanout, GPU execution, tracking/calibration, thermal
operation, exact-device boot acceptance and recovery still require completion.

## Opt-in direct display

Patch 0005 allows a KGSL instance enabling `VK_KHR_display` to open the DRM
primary node named by `TU_KGSL_DISPLAY`. The helper requires a character device,
primary-node identity, PRIME import, universal planes, atomic KMS, DRM master,
and nonempty connector/CRTC/plane inventories. It does not force another client
to surrender DRM master. Failure closes the opened descriptor and preserves the
original error; later KGSL initialization failures also release that descriptor.

The display opens before the synchronization bridge, which could otherwise take
DRM master first. `local_fd` remains the KGSL GPU descriptor; `master_fd` is the
selected display. Existing common display WSI uses the latter for scanout.
Presentation-device checks match the selected primary's device identity,
including DRM leases. Physical-device DRM properties describe the selected
primary and do not invent a DRM render node for KGSL.

This selection applies only to instances enabling `VK_KHR_display`. Ordinary
application instances do not acquire the primary through `TU_KGSL_DISPLAY`.
`TU_KGSL_DRM_SYNC` remains independent; applications should use a verified,
suitable render node for synchronization so they do not occupy display master.
The vendor SDE DRM driver declares render-node support, but actual node identity,
permissions and synchronization capabilities still require target validation.

The [DMA-buffer sync-file backport](dma-buf-sync.md) supplies the kernel API
Mesa uses to attach completion fences before presentation. KGSL now rejects
allocation/import requests for its unsupported implicit-sync fallback instead
of silently accepting that flag. This does not prove GPU completion, modifier
compatibility or scanout. Complete v10 kernel/module/device-tree exports now
include the backport. The October 9 restored roots contain the matching newer
modules and `turnip-display-v2`. The October 10 physical-kernel root also includes
the GPUOBJ driver update described below; its QEMU counterpart remains pending.

`output/mesa/turnip-display-v2/` contains the compiled ARM64 driver, four passing
ASan/UBSan suites and five passing Freedreno tests. The first display build is
preserved at `turnip-display-v1/`; it failed because the presentation callback
referenced a function private to another source file. The corrected callback
uses the kernel backend identity directly.

```sh
python3 -B tools/test-kgsl-display.py QEMU_KERNEL_BUILD \
  --turnip NEW_MESA_DIRECTORY --output NEW_DISPLAY_TEST_DIRECTORY
```

This runner refuses physical kernel builds and existing output directories,
verifies kernel/Turnip identities, and boots without disks or networking.
`output/kgsl-display-v1/` uses the QEMU DMA-buffer test kernel and the exact
compiled display helper with real virtio-gpu DRM ioctls. Primary/render-node
checks, master contention, lease identity, repeated release/reacquisition and
clean power-off pass. The separate sanitizer suite models libdrm failures with
real descriptor ownership. These tests perform no KGSL GPU work or panel scanout.

`output/kgsl-display-audit-v1/` records artifact rejection tests, loading the new
DSO in the pinned ARM64 runtime, and an absent-KGSL negative probe. The previous
v7 bundle is preserved but is not accepted as a build of the current patches.
The October 9 [restored roots](headset-root.md) include the display driver.
The later GPUOBJ allocation change is included in the October 10 physical-kernel
root and passes its loader/extraction checks; virtual root acceptance remains open.

## Native compositor integration still required

The pinned runtime already includes a native ARM64 Monado service with the
Vulkan direct-display target. Its package is
`25.1.0^20260820git01c1f6b-1.fc44.aarch64`; this is distinct from the translated
desktop SteamVR compatibility runtime.

The exact [Monado source](https://gitlab.freedesktop.org/monado/monado/-/tree/01c1f6b23ab73c1459c8f4cfd2eb16d5142214c2/src/xrt/compositor/main)
was retrieved and checked against Git blob identities. The files and hashes are
preserved under `output/hardware-research/monado-display-01c1f6b/`. Its
`XRT_COMPOSITOR_FORCE_VK_DISPLAY` setting selects a zero-based display index and
requests `VK_KHR_display`. The [native Monado bundle](monado.md) corrects the
display-index bounds check, empty-plane access, mode validation and compatible
plane selection. The existing runtime image still contains the original package;
the October 9 restored headset roots include the new bundle and pass dependency
and extraction checks. Automatic mode selection still prioritizes pixel count
and then refresh rate.

The earlier description of this target as forcing simulated display timing was
incomplete. `COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING` selects the simple pacing
implementation, but the independent `VK_EXT_display_control` event thread feeds
actual vblank timestamps into it when supported. The new upstream pacing test
verifies that supplied vblank phase changes reach its predictions at 72, 90 and
120 Hz. No KGSL/SDE hardware timing has been measured; the existing flag is
preserved pending that validation.

The vendor Sharp panel description specifies two DSI controllers and a
2064-by-2208 per-panel timing. The vendor mode enumeration multiplies horizontal
active pixels by controller count; this implies a 4128-by-2208 combined mode for
that configuration. It is source-derived topology, not a measurement of the
attached headset. BOE/JDI/Sharp variants, selected panel, orientation, mode,
optics/calibration and compositor timing must be matched to the actual device.
Do not select the highest advertised refresh rate automatically for bring-up.

The native Monado bundle accepts `XRT_COMPOSITOR_FORCE_DISPLAY_MODE` as
`WIDTHxHEIGHT@REFRESH_MILLIHZ`. For example, `1920x3664@72000` requests exactly
that tuple; this is syntax, not an accepted mode for a particular headset.
Use a tuple from the selected DRM/Vulkan display's verified mode inventory.
The selector refuses malformed, absent or duplicate matches, and refuses a
conflict with `XRT_COMPOSITOR_DESIRED_MODE` if a numeric index is also supplied.
It never falls back to the automatic selector after an explicit tuple fails.
The constraint is checked before creating a display surface. Without it,
the existing numeric-index and automatic selection behavior remains.

This removes dependence on mode-list ordering; it does not identify a connector,
validate panel timing or establish physical scanout. Display identity and the
HMD's calibrated layout still need to agree with the selected mode.

No physical compositor service is enabled by this change. DMA allocation,
GPU rendering, PRIME import/modifiers, correct binocular output and timing,
tracking/calibration, thermal behavior, exact-device boot acceptance and recovery
remain required. Virtual results do not establish flash readiness.

## Physical Android GPU acceptance

On October 10, the source-built Android Turnip candidate passes actual GPU
buffer transfers and offscreen shader rendering on both connected development
headsets. Each test runs as the ordinary ADB shell with enforcing SELinux,
using a temporary process-local driver and exact firmware guards.

| Device | Stock firmware | GPU | Tested result |
|---|---|---|---|
| Quest 3 | `52083180032000520` | Adreno 740v3 | 32 buffer/fence checks; 16 shader-rendered frames |
| Neo3 Pro Eye | `smartcm.1696861506` / 5.8.4.0 | Adreno 650 | 32 buffer/fence checks; 16 shader-rendered frames |

The initial Quest candidate enumerates its GPU but fails `vkCreateDevice` with
`VK_ERROR_OUT_OF_DEVICE_MEMORY`. Patch 0006 adapts the
[community GPUOBJ allocator change](https://github.com/Jbbrack03/Quest-3-Turnip-Driver/blob/83cd874f3449d091fca51d25389f88f520a7f2f3/0001-android-carry-Quest-3-KGSL-compatibility-patches.patch)
to the existing Armada KGSL changes. Allocation, address queries, release and
memory-type probing use the modern ioctl family. An additional error path frees
an allocation whose address query fails before publishing the BO. A bounded
userspace-injected query error on each physical headset verifies a successful
release of that exact object, followed by normal process exit.

The initial Pico candidate cannot load because Android 10 lacks the exported
`atrace_get_enabled_tags` function used by newer platform headers. The separate
[Android tracing patch](../patches/mesa/android/README.md) restores the old ABI
for API 29 builds. Both headsets use their actual system libcutils; no stub
library is installed or used at runtime.

Each transfer checks every word of a 1 MiB GPU-written buffer after a bounded
fence wait. The rendering test executes vertex and fragment shaders into a
64-by-64 image, varies a push constant between red and yellow, and keeps the
right half blue. Every pixel is checked across 16 frames. Exported captures
match the stock-driver baseline. These small offscreen tests do not measure
headset frame pacing, thermal behavior, compositor presentation or scanout.

Both stock Vulkan drivers lack the queried opaque-FD semaphore sharing and
direct-display capabilities. Opening the SDE DRM render node from the shell
returns `EACCES` on both devices; its synchronization capabilities therefore
remain unmeasured. Explicitly enabling `TU_KGSL_DRM_SYNC` with that inaccessible
node fails initialization, preserving the bridge's refusal behavior.

The tested Android candidate has SHA-256
`b1db70afb1cf54fde2996b6ff60a2a26d576bcc8e45e088f529060a6c2eea3da`.
Build commands, negative baselines, driver/dependency hashes, pixel captures
and cleanup receipts are retained in `output/headset-gpu-20261010-v1`.
Firmware, boot identity, locked/green state and enforcement are unchanged;
the temporary device files are removed.

A separate Linux/glibc test also passes on both stock kernels. It uses the
unchanged output of the maintained Linux builder, including the KGSL DRM and
display patches: SHA-256
`4799fc79cdc9fb9c8ede6657428a5a2b9868e5675e37575d9ecc5c5388b76ee4`.
An explicit private glibc loader and 23-file dependency/test package run as the
ordinary ADB shell user, without a chroot, Android HAL or system-library changes.
Both devices render all 16 shader frames correctly; exported pixels match the
Android candidate byte for byte. The driver and its dependencies are removed
afterward, with device identity and enforcement unchanged. Exact package hashes,
build commands and results are in `output/headset-linux-gpu-20261010-v1`.

These results establish real GPU execution for both userspace ABIs under stock
kernels. Custom boot, DRM synchronization, display ownership and physical VR
remain separate gates.


The October 13 Reframed build in `output/pico-turnip-20261013-v1/` recompiles
Turnip with the current install prefix and unchanged KGSL/display patches. All
five Freedreno tests and the fence, wait, submission and display helper suites
pass. The resulting ARM64 library passes immediate loading alongside the rebuilt
Monado and SSC bundles through [the combined userspace check](monado.md#offline-graphics-bundle-compatibility).
This verifies the native dependency set, not execution or scanout on the Pico GPU.

## DMA-buffer import failures

Patch 0007 releases the imported KGSL GPU object if its address query or the
retained descriptor duplication fails. The import is published only after both
resources exist; failures preserve the original diagnostic and leave the caller's
descriptor untouched. A cleanup ioctl failure cannot guarantee reclamation, but
still returns the original import error without publishing an unusable BO.

The builder runs the actual import and export functions under ASan/UBSan with
modeled KGSL ioctls and real descriptors. The 2,304 cases cover successful imports,
query/import/duplication failures, cleanup failure and repeated recovery. The
preceding source fails 1,536 ownership checks; the corrected source passes.
These are userspace ownership tests, not hardware GPU or display acceptance.

Patch 0008 fixes display-plane modifier lookup: an absent format no longer
inherits the first format's modifiers. Missing property queries are handled
without dereferencing null, and all property metadata is released, including on
kernels without `IN_FORMATS`. The existing no-modifier fallback remains.

The actual WSI function passes 6,336 synthetic format/query/allocation cases,
including format indices across the 64-bit modifier-mask boundaries, and a
missing-property sanitizer test. The previous function fails 4,776 cases and
triggers a null-pointer diagnostic. This verifies lookup and ownership; compatible
physical SDE/Adreno layouts and actual PRIME import still need hardware testing.

The diskless DRM fixture now compiles the same WSI function as the driver and
compares its results with libdrm's modifier iterator while accounting for every
property allocation. It passes 352 lookups on the retained virtio-gpu test kernel,
plus descriptor, master-contention and lease checks. Both virtual planes omit
`IN_FORMATS`, so this kernel run covers the no-modifier path; populated tables
remain covered by the sanitizer suite. The original function fails the same
real-DRM fixture. Both guests power off normally. This uses the retained Quest
QEMU transport kernel, not the Pico kernel or a physical display.

Current driver and image receipts are under
`output/pico-import-cleanup-20261014-v1/`: `turnip-final/`, `root-final/`,
`qemu-root-final/` and `boot-final/`. Native loading, full root readback and a
fresh read-only Pico root VM pass. The exact-mode Monado package is unchanged.
Physical Adreno/SDE interoperability, panel/mux operation and recovery remain
unverified; these images must not be treated as flash-ready.


## Native allocation lifetime

Patch 0009 separates cleanup of an unpublished allocation from destruction of a
registered BO. A failed sparse binding or virtual-address query now releases the
GPU object. A rejected capture/replay address releases the returned CPU mapping,
and destroying a replayable sparse VMA releases its retained mapping. Mapping
failures no longer remove an unregistered object from the dump list or emit a
BO destruction event before its allocation event.

The allocation suite executes the actual patched functions with the source's
KGSL UAPI declarations, modeled GPU ioctls and real file-backed `mmap`/`munmap`.
It covers ordinary, lazy, sparse and capture/replay allocations, fixed-address
success and rejection, reference counts, owned descriptor closure, repeated
allocation/query/binding/mapping failures, and failed release ioctls. All 6,144
cases pass under ASan/UBSan; the preceding source fails 3,456 cases. When the
kernel refuses a release, reclamation remains unproven; the original allocation
error is still returned and no BO is published.

The full builder requires this suite alongside the existing fence, wait,
submission, display and import suites. These results establish userspace
lifetime handling, not physical Adreno execution or display acceptance.

## Multi-plane PRIME ownership

Patch 0010 imports each display image's DMA-buffer once and reuses that handle
in every plane slot. Repeated PRIME imports into the same DRM file return the
existing GEM handle without another handle reference. The previous per-plane
cleanup could therefore close the same handle repeatedly. Both framebuffer
failure cleanup and normal destruction now close it once.

The actual WSI image functions pass 4,096 ASan/UBSan cases covering one to four
plane slots, modifier/no-modifier paths, allocation/import/framebuffer failures
and successful teardown. The previous functions fail 1,536 cases.

The diskless virtual-DRM fixture also executes these functions against actual
PRIME and framebuffer ioctls. It passes 192 cases: 64 framebuffer creations,
64 invalid-descriptor import refusals and 64 unsupported-modifier framebuffer
refusals, with no repeated handle closes or leaked descriptors. The original
functions fail the same fixture. Both guests power down normally.

The virtual framebuffers use XRGB8888; extra plane slots do not establish
physical multi-plane format support. Adreno/SDE modifier compatibility, UBWC,
GPU completion and physical scanout remain unverified.
