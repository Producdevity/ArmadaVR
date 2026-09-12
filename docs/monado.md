# Native Monado compositor

The headset compositor and OpenXR runtime are native ARM64. The source bundle
pins Monado 25.1.0 at `01c1f6b23ab73c1459c8f4cfd2eb16d5142214c2`, the revision of
the existing Fedora runtime package. This is separate from the desktop SteamVR
runtime currently tested through FEX. It does not supply Valve's ARM64 runtime.

## Build and packaging

With the existing ARM64 development image available:

```sh
docker build --platform linux/arm64 -f Containerfile.monado \
  -t localhost/armada-vr:monado-tools .
bash tools/build-monado.sh NEW_MONADO_DIRECTORY
```

The source archive is verified by size and SHA-256. Compilation uses the resolved
immutable tools image without network access, two CPUs, a 4 GiB memory limit and
1 GiB of temporary build space. Existing output directories are refused. The
builder records the profile, patches, tests, compiler commands, package inventory,
source tree identity, test results and output hashes.

The installed bundle is under `stage/opt/armada-vr/monado`. It contains native
service/client libraries, tools and the OpenXR manifest. `tools/monado-artifact.py`
provides `validate(directory)` for packaging and test callers; validation rejects
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
`localhost/armada-vr:monado-runtime-test` image adds that dependency; `ldd -r` on
the service and immediate dynamic loading of both client libraries then passed.
The canonical runtime image and headset roots have not yet received this bundle.

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

These are native software and virtual rendering checks. They do not prove
SteamVR dashboard navigation, real controller tracking/haptics, KGSL/SDE panel
operation, optics calibration, exact-device custom boot or recovery. The complete
kernel/module/root integration and those acceptance gates remain outstanding.
Virtual results do not establish flash readiness.
