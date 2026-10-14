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

## Input pressure and image formats

For live capture, set `nonblocking-input=1` and `deterministic=0` in the Basalt
configuration. The VIT input contract requires nonblocking delivery, but the
pinned upstream implementation waits when an input queue fills. With no camera
frames, its IMU producer stopped at sample 302 in the isolated reproduction.
The patch reports queue exhaustion and refuses further input until the owner
stops and recreates the tracker. It does not silently discard sensor samples
and continue reporting tracking. Offline accelerated replay retains the legacy
blocking mode to pace its producer against the estimator.

Six queue-exhaustion and 24 malformed-image cases stop cleanly under native
sanitizer instrumentation of the caller. The image copy also honors row stride
and reads full 16-bit grayscale samples; upstream's advertised L16 path instead
read bytes. Padded, unaligned L8 and L16 replays now produce identical trajectories
for the checked recording. Basalt itself remains a Release build.

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

## Capture session ownership

`pico-slam-session` owns the input adapter, VIT estimator and HMD pose provider.
It recreates the estimator for each newer stream generation and invalidates
poses on camera/IMU gaps, expired clock mappings, rejected input, exhausted
backend queues or invalid output. Obsolete callbacks cannot reset a newer
stream. Its owner serializes capture calls and polls regularly, including when
capture is silent; HMD readers use the provider's independent lock. Stop capture
and release the HMD before destroying the session. The backend library and
explicit configuration must remain available, with nonblocking input enabled.

Host and ARM64 sanitizer tests cover 1,013 generations and balanced estimator,
frame and pose lifetimes. A separate native recorded-data run through this owner
produces all 185 estimator poses and 184 usable head poses, then invalidates the
provider on shutdown. Its producer is paced against the estimator, using recorded
time. This establishes session integration, not real-time Pico throughput.

The owned stereo transport now feeds this session in the maintained replay.
It creates real request packets and routes simulated kernel events through the
production request, stream and capture code. All 372 buffers are released after
copying; the estimator receives 370 images and 3,467 IMU samples and returns 185
poses. Reference RMS error is 6.417 mm with rigid alignment and no scale fit.
Physical sensor identities, calibration and capture timing are still required.

## SSC IMU handoff

`pico-slam-imu` transfers bias-corrected SSC samples from the sensor worker to
one serialized capture/SLAM owner. It applies explicit sensor-to-IMU rotation
and unit scales, pairs equal ticks and mapped timestamps, and carries the larger
clock uncertainty into the estimator. It does not infer factory calibration or
subtract bias again. Queues hold at most 32 unpaired samples per sensor and 128
pairs; exhaustion, bad current-generation data and excessive gaps invalidate the
generation. Old callbacks cannot invalidate a newer generation.

The consumer polls even during silence and stops the SLAM session on failure.
Producer and consumer clock reads are tracked independently: a consumer may
read its clock before the producer acquires the mutex. Neither thread calls the
estimator while holding the queue lock. Stop and join producers before freeing
this queue.

Host and ARM64 sanitizer tests cover 200 restarts, 59,800 paired samples,
10,000 concurrent producer/consumer pairs and 22 failure cases. ThreadSanitizer
also passes. The reference replay sends synthetic corrected SSC samples through
this handoff alongside the owned camera transport, delivering all 3,467 IMU
samples and 370 images to Basalt and producing 185 poses. This tests transport
integration; the recording still uses Valve Index calibration.

`pico-slam-runtime` connects the selected SSC raw/bias subscriptions to this
queue and the owned VIT session. The sensor worker never calls the estimator;
it invalidates the pose provider immediately on failure. The capture owner
polls bounded IMU batches and estimator output, starts camera delivery only
after the first accepted IMU pair, and copies completed camera frames through
the session. Startup waits for SSC without consuming the camera gap budget.
All failures require recreating the owner; destruction joins SSC callbacks
before freeing the queue or backend. The HMD must be released first.

The runtime owner passes host/ARM64 sanitizer and thread checks over 200 normal
lifetimes, eight backend failures and seven stream/clock failures. The real SSC
library also passes 100 cancelled startups without QRTR and seven scenarios
under the retained Pico kernel with virtual QRTR endpoints: normal, one bias
update, clock failure, missing service, wrong rigid body, unreliable bias and
malformed gyro. Normal scenarios deliver the expected corrected SI vectors on
the consuming thread; failure cases deliver no IMU to the test backend. These
VM tests use a synthetic estimator, independently of the real Basalt replay.

Its configuration requires explicit SUIDs, supported rates, clock calibration,
axis/unit conversion, camera timing and estimator calibration. Construction
starts those SSC subscriptions; callers can instead prepare the runtime and
activate SSC after camera setup. Deferred startup prevents samples from filling
the bounded queue during configuration. Start is attempted once, and stopping
before activation prevents a later start. Neither entry point opens, configures
or starts cameras.
The auto prober remains disabled for this unverified physical configuration.

## Continuous capture

`pico-slam-live` owns the configured stereo rig, request transport and SSC/SLAM
runtime. The caller provides already probed sensors and exclusive nonblocking
CRM/sync descriptors. Preparation validates tracking, configures the rig, creates
the transport, subscribes events and only then activates SSC. It does not open
nodes or infer physical camera selection.

Polling waits for the first paired IMU, queues the initial stereo requests and
starts the cameras. Completed frames are copied into the estimator; a request is
refilled only after both cameras complete. The queue holds one to eight pairs,
with an explicit frame period and timeout. Calibrated exposure-center offsets
apply to newly queued requests. The caller must poll during silence too, so
clock expiry, lost IMU and missing fences stop the generation.

Stop invalidates poses and attempts all affected stream stops. The caller must
then establish DMA quiescence independently before retiring requests,
unsubscribing events and releasing command buffers and hardware handles. An
uncertain cleanup remains owned and is not retried. Destroy any HMD borrowing
`pico_slam_live_tracking()` before destroying the live owner.

The host and ARM64 lifecycle tests use the actual tracking runtime with camera
lifecycle doubles. They cover 200 lifetimes, all supported queue depths, partial
stereo completion, 20 failures and retained cleanup ownership. ThreadSanitizer
also passes. These tests do not establish physical capture or calibration.

A separate ARM64 sanitizer replay connects the actual rig, request/packet
transport, runtime and Basalt estimator through simulated ioctls, DMA and SSC
reports. It supplies all 3,467 IMU pairs and 370 usable images from the pinned
reference recording, producing 185 poses and 184 tracked head poses. Startup
skips the one image pair preceding the first IMU sample; shutdown retires the
remaining queued pair. The trajectory has 6.41 mm rigid RMS error and 16.60 mm
maximum error without fitted scale, matching the earlier bounded-queue replay.
This recording uses Valve Index calibration and is not Pico accuracy evidence.

## Installed camera runtime

The Pico build installs `libpico_tracking.so` alongside Monado. Both the service
and CLI link to it; the library retains the camera setup, continuous capture,
SSC handoff, pose provider and HMD factory entry points. A static driver archive
was insufficient because the linker discarded capture functions that the
automatic prober does not call. Bundle verification now checks the exported
functions, executable dependencies and a device-free library load.

Add `--monado-bundle VERIFIED_MONADO_BUNDLE --ssc-bundle VERIFIED_SSC_BUNDLE`
to the replay command to test that installed library. This variant links only
the harness and simulated hardware transport locally; camera/runtime/HMD code
comes from the installed library. Its function-resolution check confirms that
origin. The library and Basalt are Release builds; the harness and hardware
fixture use ASan/UBSan.

The installed-library replay also creates the actual HMD with synthetic optics
and the live owner's borrowed pose provider. It checks tracked head poses,
stereo views, current-input activity, accepted and refused predictions, and
pose invalidation after capture stops. The fixture refuses a duplicate SSC
subscription. Input is paced separately for this test so the estimator can
produce poses within the production prediction window; this is not a headset
throughput measurement.

This is an internal interface tied to the exact Monado headers and driver
revision. It does not enable camera capture in the automatic prober or provide
physical calibration. A physical owner still needs exclusive device access,
verified settings, regular polling and independently established DMA shutdown.
Do not call the quiescence acknowledgment solely because a stop ioctl succeeded.

## Remaining integration

- Capture native camera buffers with known ownership, format and exposure times.
- Establish active camera identity/order, intrinsics, distortion and IMU-to-camera
  transforms; the retained older XML is not established as the active profile.
- Bind camera and IMU clocks with measured uncertainty, and reset the estimator
  whenever a stream generation changes.
- Supply the continuous capture owner with verified device configuration,
  calibration, clock mapping and regular polling.
- Validate camera coverage, real motion, loss/recovery and display-time prediction
  on the device after the boot/recovery gates pass.

See [camera records and input ownership](pico-tracking-protocol.md#camera-snapshots-and-synchronization)
and [the installation requirements](device-installation.md).
