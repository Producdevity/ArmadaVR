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
including the pinned Eigen archive. Compilation runs without network access,
with a read-only source tree, one build job, two CPUs and a 4 GiB memory limit.
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

## Remaining integration

- Capture native camera buffers with known ownership, format and exposure times.
- Establish active camera identity/order, intrinsics, distortion and IMU-to-camera
  transforms; the retained older XML is not established as the active profile.
- Bind camera and IMU clocks with measured uncertainty, and reset the estimator
  whenever a stream generation changes.
- Feed fresh estimator poses through a bounded, calibrated HMD pose provider;
  reject stale or invalid output rather than advertising positional tracking.
- Validate camera coverage, real motion, loss/recovery and display-time prediction
  on the device after the boot/recovery gates pass.

See [camera records and input ownership](pico-tracking-protocol.md#camera-snapshots-and-synchronization)
and [the installation requirements](device-installation.md).
