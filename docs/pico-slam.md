# Pico visual-inertial tracking

The offline tracking path uses native ARM64 Basalt through VIT 2.0.1 and the
pinned Monado frame/IMU interfaces. Basalt and its required dependencies are
pinned in `profiles/basalt.json`. This backend is not enabled automatically on
a headset: camera capture, active calibration, sensor axes and clock alignment
must be established first.

## Build

Build an ARM64 compiler image from the project's Monado tools image, then pass
its immutable image ID to the builder:

```sh
docker build -f Containerfile.basalt -t localhost/reframed-vr:basalt-tools .
python3 -B tools/build-basalt.py prepare --output output/basalt-source-new
python3 -B tools/build-basalt.py build output/basalt-source-new/source \
    --image sha256:LOCAL_IMAGE_ID --output output/basalt-build-new
```

Source preparation fetches exact commits and verifies the complete source tree,
including the pinned Eigen archive. Hash-pinned patches are applied to a separate
copy; the original sources are preserved. Compilation runs without network access,
with a read-only patched source tree, one build job, two CPUs and a 4 GiB memory limit.
The build requires all 187 upstream tests to pass. The staged install contains
the native library, VIT header, corrected pkg-config file, dependency license
files and source provenance. It does not install or select a host runtime.

## Recorded-data test

`profiles/slam-replay.json` identifies the public Collabora Monado dataset
`MIO09_short_1_updown`, its exact archive hash and matching upstream Basalt
calibration. Download that archive separately. The test needs NumPy on the host
and the Basalt compiler image; it does not need a connected headset.

```sh
python3 -B tools/test-pico-slam-replay.py \
    --monado-archive PINNED_MONADO_ARCHIVE \
    --dataset-archive MIO09_short_1_updown.zip \
    --basalt-source output/basalt-source-new/source \
    --basalt-build output/basalt-build-new \
    --image sha256:LOCAL_IMAGE_ID --output output/pico-replay-new
```

The harness puts real recorded grayscale images into synthetic Pico snapshot
headers, reverses camera callback order and overwrites the source buffers after
each call. `pico-slam-input` delivers owned Monado frames and calibrated IMU
samples to the actual Basalt VIT library. Both fresh backend runs must finish,
produce the expected poses and meet the reference trajectory thresholds. The
adapter and harness run with address/undefined-behavior sanitizers; the Release
Basalt library itself is not sanitizer-instrumented.

Before replay, 70 lifecycle cases exercise empty, IMU-only, incomplete-camera,
stereo-without-IMU, queued-camera and normal startup, including repeated stop
and destruction before startup. The pinned patches join optical flow before
its derived buffers are destroyed, handle the end-of-stream pose sentinel and
correct the upstream grayscale format query. The unpatched backend crashed
when stopped after receiving stereo images without IMU input.

The checked recording contains 186 stereo frames and 3,467 IMU samples. Its
first image pair precedes the first IMU and is correctly withheld because it
cannot be bracketed. Both tested runs delivered 370 images and all IMU samples,
produced 185 finite poses, and stopped cleanly. Position RMS error was 6.4 mm
and maximum error 16.6 mm after rigid alignment to the recording's reference
trajectory. Alignment changes rotation and translation only; it does not fit
scale. Monado's independent `slambatch` replay also completed.

These results validate a short recorded-data pipeline with Valve Index
calibration. They do not measure Pico accuracy, latency, camera availability,
relocalization, thermal behavior or sustained on-device performance. Reference
calibration is never substituted for Pico calibration.

## Offline root package

Pass `--basalt VERIFIED_BUNDLE` to `tools/build-headset-root.py` to include the
native library in an offline root. The builder verifies its pinned source,
patches, tests, architecture and staged contents, tests dynamic loading, and
reads the installed files back from the completed image. `test-pico-root.py`
also checks the installed package and VIT version in the Pico-kernel VM.
This does not start a tracker or supply headset calibration.

## HMD pose output

`pico-slam-tracking` accepts timestamped estimator IMU poses and explicit
`T_openxr_slam` and `T_imu_head` transforms. It keeps Monado relation history,
estimates head linear/angular velocity, and caps prediction at 50 ms including
clock uncertainty. Initialization requires two estimates. Stale, nonfinite,
regressing or discontinuous poses are rejected; a newer generation clears
history after the owner resets the estimator.

The Pico HMD factory can borrow this provider through `visual_tracking`.
It then uses full head poses without starting a second IMU session or switching
to an unrelated orientation-only world frame. The caller owns the provider and
must stop capture and destroy the HMD before freeing it. The automatic prober
does not create this path: active camera calibration and a capture/VIO owner
are still needed.

Native sanitizer tests cover transforms, interpolation, prediction limits,
1,000 stream generations, 10,000 poses and 20,000 concurrent reads. Recorded
Basalt replay additionally yields 184 usable head poses after initialization
in each run and clears the provider on stop. These tests use synthetic transforms
and replay time, not measured Pico axes or on-device latency.

## Remaining integration

- Capture native camera buffers with known ownership, format and exposure times.
- Establish active camera identity/order, intrinsics, distortion and IMU-to-camera
  transforms; the retained older XML is not established as the active profile.
- Bind camera and IMU clocks with measured uncertainty, and reset the estimator
  whenever a stream generation changes.
- Own camera capture and estimator lifetimes together, connect pose draining to
  the HMD provider, and invalidate it on capture loss or backend failure.
- Validate camera coverage, real motion, loss/recovery and display-time prediction
  on the device after the boot/recovery gates pass.

See [camera records and input ownership](pico-tracking-protocol.md#camera-snapshots-and-synchronization)
and [the installation requirements](device-installation.md).
