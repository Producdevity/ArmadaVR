# Native Monado compositor

The headset compositor and OpenXR runtime are native ARM64. The source bundle
pins Monado 25.1.0 at `01c1f6b23ab73c1459c8f4cfd2eb16d5142214c2`, the revision of
the existing Fedora runtime package. This is separate from the desktop SteamVR
runtime currently tested through FEX. It does not supply Valve's ARM64 runtime.

## Build and packaging

With the existing ARM64 development image available:

```sh
docker build --platform linux/arm64 -f Containerfile.monado \
  -t localhost/reframed-vr:monado-tools .
bash tools/build-monado.sh NEW_MONADO_DIRECTORY
```

To include the opt-in Pico SSC controller driver, pass a verified SSC bundle as
the second argument. `MONADO_BUILD_IMAGE_ID` can select an existing immutable
compiler image instead of the default tag:

```sh
MONADO_BUILD_IMAGE_ID=sha256:COMPILER_IMAGE_ID \
  bash tools/build-monado.sh NEW_MONADO_DIRECTORY VERIFIED_SSC_DIRECTORY
```

The optional build copies the maintained driver sources into the pinned Monado
tree, verifies the SSC public header and links the exact packaged library. The
artifact records both the driver source hashes and the full SSC dependency
inventory. A root build must supply that same SSC bundle; a missing, changed or
different installed dependency is refused before copying Monado.

The runtime still requires both `PICO_SSC_CONFIG` and `PICO_SSC_COUNTER=1` to
open a controller session. See [controller configuration](pico-controller-protocol.md#controller-configuration).
Include `/opt/reframed-vr/ssc/lib64` alongside the Monado library directory in
`LD_LIBRARY_PATH` for this optional build. No configuration, default runtime or
automatic subscription is installed. The prober supplies digital/touch controls;
physical analog calibration, tracking and haptics remain separate work.

The source archive is verified by size and SHA-256. Compilation uses the resolved
immutable tools image without network access, two CPUs, a 4 GiB memory limit and
1 GiB of temporary build space. Existing output directories are refused. The
builder records the profile, patches, tests, compiler commands, package inventory,
source tree identity, test results and output hashes.

The installed bundle is under `stage/opt/reframed-vr/monado`. It contains native
service/client libraries, tools and the OpenXR manifest. `tools/monado-artifact.py`
provides `validate(directory)` for build and test callers; validation rejects
changed sources, failed tests, damaged artifacts and foreign-architecture ELFs.
The service must contain the actual direct-display implementation, and the
preprocessor record must show that the target was compiled.

Building does not select the default OpenXR runtime or enable a service. Upstream
also stages systemd unit templates inside the opt prefix; they are not installed
in the system unit directories. A test selects the bundle explicitly through
`PATH`, `LD_LIBRARY_PATH` and its OpenXR manifest. Keep test output outside the
validated bundle so its artifact inventory remains unchanged.

Runtime dependencies must be checked in the final userspace. The current base
runtime needed Fedora's `opencv-videoio` package for this build. The separate
`localhost/reframed-vr:monado-runtime-test` image adds that dependency; `ldd -r` on
the service and immediate dynamic loading of both client libraries then passed.
The canonical runtime image and retained complete headset roots have not yet
received this bundle.

The [offline root builder](headset-root.md) accepts `--monado VERIFIED_MONADO_BUILD`.
Its `package(directory)` helper extends build validation with an exact inventory
of file hashes, permissions, directories and relative symlink targets. Installation
accepts an offline staging root, refuses existing bundle paths and symlink
ancestors, copies the bundle and build record, then verifies them. The root builder
also compares this inventory with files extracted from the completed ext4 image.
Both the early userspace-image probe and assembled-root checks must resolve every
service dependency and immediately load `libopenxr_monado.so` and `libmonado.so`.
This packages the bundle without selecting a runtime or enabling a service.

## Direct display selection

`XRT_COMPOSITOR_FORCE_VK_DISPLAY` selects a zero-based display index.
`XRT_COMPOSITOR_DESIRED_MODE` selects a zero-based mode index; `-1` retains the
upstream automatic policy. The patch rejects invalid explicit indices rather
than falling back to a different, potentially faster mode. It rejects null mode
handles, zero dimensions and zero refresh rates, and calculates the frame period
from the selected mode's millihertz refresh rate using integer arithmetic.

Surface creation now chooses a plane that supports the selected display and
mode, identity transform, zero-origin extents and an advertised alpha mode. It
preserves the selected plane's stack index and avoids a plane assigned to another
display. Empty inventories, allocation failures and Vulkan errors fail before
surface creation. Display and mode ordering are not stable device identities:
headset session integration must validate the actual panel and mode before using
these settings. No physical session or Quest display index is selected here.

These checks follow the Vulkan [display surface requirements](https://docs.vulkan.org/refpages/latest/refpages/source/VkDisplaySurfaceCreateInfoKHR.html)
and [plane capabilities](https://docs.vulkan.org/refpages/latest/refpages/source/VkDisplayPlaneCapabilitiesKHR.html).
They do not establish GPU completion, PRIME/modifier compatibility or scanout.

## Timing and verification

The target retains `COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING`. This chooses the
simple pacer instead of the GOOGLE display-timing pacer. Independently,
`comp_target_swapchain.c` starts a `VK_EXT_display_control` event thread when
supported. That thread waits for a first-pixel-out fence, timestamps completion
and sends the result through `u_pc_update_vblank_from_display_control`. The simple
pacer updates its presentation phase from that timestamp. The added test exercises
that actual pacer at 72, 90 and 120 Hz with 64 shifted vblank phases per rate.
Physical timing and the timestamp's accuracy remain unverified.

Evidence from September 12, 2026:

- `output/monado/build-v3/`: complete native build, two ASan/UBSan display-selection
  suites and all 25 upstream test executables pass. The pacing executable reports
  2,030 assertions across three cases, including the new vblank-phase case.
- `output/monado/display-baseline-v1/`: the original code reproduces a zero-plane
  null access and an out-of-range display-index heap read. The patched tests also
  cover Vulkan/allocation failures and 1,024 cleanup cycles per selection path.
- `output/monado-runtime-v2/`: the new native service/client pass both-controller
  press/release and disconnect/reconnect checks, then 599 stereo projection frames
  through Xvfb/lavapipe. Captures follow the independent left/right action states
  and return to baseline on release. Haptic checks cover the API only.
- `output/monado-audit-v1/`: eleven artifact rejection checks and all 123 host
  tests pass. The failed build with a missing patch utility and failed runtime
  probe with a missing videoio library remain preserved alongside the fixes.

- `output/monado-root-package-v6/`: the maintained root assembly script installs
  all 85 regular files and two library symlinks alongside the new kernel modules,
  firmware and Turnip. Native loading, filesystem checking and exact extraction
  checks pass using a dependency fixture and a temporary ext4 filesystem in RAM.
  This is not a retained complete root or a boot test.
- `output/monado-root-audit-v1/`: 130 host tests pass, including installation
  rejection cases. The original runtime fails the early missing-library probe;
  the corrected runtime passes it, then the output-space guard prevents export
  on the nearly full host disk. Both outcomes and earlier failed fixtures remain.

These are native software and virtual rendering checks. They do not prove
SteamVR dashboard navigation, real controller tracking/haptics, KGSL/SDE panel
operation, optics calibration, exact-device custom boot or recovery. The complete
root build and boot integration with the updated kernel/modules and those
acceptance gates remain outstanding.
Virtual results do not establish flash readiness.

The optional Pico runtime in `output/pico-monado-runtime-20261012-v1/build-v1/`
builds all 658 targets with the current SSC bundle and passes all 25 upstream
CTest executables. Native `ldd -r` resolves its dependencies. The actual
`monado-cli test` creates and assigns both Pico controller roles only with the
explicit configuration and counter opt-in; absent opt-in or invalid configuration
creates neither controller. Removing SSC from the library search path makes the
loader refuse the executable. The test container first confirms that its kernel
has no QRTR transport, and uses Monado's simulated HMD. These checks establish
runtime registration and cleanup, not sensor delivery or physical headset input.

A separate RAM-only root test extracts the preserved Pico image's loader, cache,
usrmerge links and dependency files. All 485 ELFs in the dependency walk are
accounted for, including private RUNPATH directories. Both service and CLI pass
full symbol relocation, and both client libraries pass immediate loading. The
7 GiB source image rehashes unchanged as
`d659b3cef2bf1bc65b6bfbc88aa41fcf751ea3dfb92184fade952107123689ff`.
This tests the image's actual libraries with the new bundle; it does not install
that bundle into the retained image or constitute a boot test. The receipt is
`output/pico-monado-runtime-20261012-v1/root-loader.json`.


## Offline graphics bundle compatibility

`tools/test-pico-userspace.py` verifies a matched Monado, SSC and Turnip set in
an isolated ARM64 Linux container. It requires the exact SSC dependency recorded
by Monado, validates all three artifact inventories before and after testing,
checks executable relocations and immediately loads both Monado client libraries,
Turnip and SSC. It also exercises actual controller registration with and without
the required opt-in and checks refusal when SSC is absent.

```sh
python3 -B tools/test-pico-userspace.py \
  --monado VERIFIED_MONADO_DIRECTORY --turnip VERIFIED_TURNIP_DIRECTORY \
  --ssc VERIFIED_SSC_DIRECTORY --output NEW_TEST_DIRECTORY
```

Run without network or passed-through devices. The verifier refuses kernels with
QRTR transport and environments exposing GPU or USB device nodes. Its controller
configuration is synthetic; there is no sensor stream or rendered frame. The
report records commands, environment, bundle identities, log hashes and failures.
It does not select a default runtime, enable a service or modify a root image.

The October 13 Reframed builds in `output/pico-compositor-20261013-v2/` and
`output/pico-turnip-20261013-v1/` pass this combined check against
`output/pico-head-tracking-20261013-v1/ssc-bundle/`. The compositor passes all 25
upstream tests plus the two display-selection sanitizer suites. These packages
use the current `/opt/reframed-vr` paths; the preserved pre-rename complete root
images still need a coordinated rebuild. Real panel scanout and GPU execution
under the custom Pico kernel remain unverified.


The semantic-space ownership patch shares a client object when multiple roles
refer to the same server ID. The original client allocated separate wrappers
for those roles, causing repeated destroy requests during teardown. The
regression exercises the actual client creation/destruction functions and
Monado reference counting across 117,649 alias/absent-role combinations,
including a borrowed space retained after overseer destruction. The original
source produces 508,134 failed invariants; the patch produces none under
ASan/UBSan and leaves no client allocations live.

Rendering tests accept `XR_RUNTIME_JSON` to select a packaged runtime explicitly;
without it they retain the distribution's default manifest path. This selection
must match the `monado-service` selected through `PATH` and its library search
path. Overwriting the supplied manifest previously made it possible to test the
system client library against a different packaged service.


The rebuilt client/service pair submits 599 stereo projection frames through
Xvfb and lavapipe. Four captured states follow left/right press and release,
and both virtual controller disconnect/reconnect transitions pass. The original
build logs a duplicate-space IPC failure at client teardown; the patched build
does not. The test retains unsupported virtual haptic-output diagnostics and
makes no claim about physical haptics, Pico GPU rendering or headset scanout.
Evidence is under `output/pico-head-tracking-20261013-v1/render-v2/`.


A VIEW-space projection layer must use eye-relative poses during composition.
The pinned compositor used world poses in its single-layer distortion path and
in the compute layer renderer. With a rotating HMD, a head-locked image moved
out of view. The patch routes VIEW-space projections through the layer renderer
and selects the eye pose for compute timewarp. World-space projections retain
the single-layer fast path. VIEW-space projections incur the extra composition
pass; physical performance remains unmeasured.

The path-selection regression exercises 72 combinations of projection type,
layer count, reference space, color adjustment and chroma key using the actual
compositor function under ASan/UBSan. Separate framebuffer tests exercise the
installed graphics and compute renderers with the Pico HMD and controllers.
