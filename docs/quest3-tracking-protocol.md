# Quest 3 tracking protocol and native driver inputs

Checked 2026-09-10 against Meta's pinned kernel
`dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f`, the project's Monado source revision
`01c1f6b23ab73c1459c8f4cfd2eb16d5142214c2`, and current primary documentation.
No physical-device command or firmware installation was performed.

The complete native tracking driver remains required. The public source inspected
provides a useful IMU packet identifier and transport interfaces, but does not
provide a verified Quest 3 sample decoder, factory-calibration schema or native
Quest 3 Monado tracking implementation. The existing
[`syncboss-replay.py`](../tools/syncboss-replay.py) correctly leaves sensor payloads
opaque; decoding the FIFO wrapper does not produce calibrated motion.

## Additional interface found in Meta's kernel

[`syncboss_direct_channel_mcu_defs.h`](https://github.com/facebookincubator/oculus-linux-kernel/blob/dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f/drivers/staging/oculus/mcu/syncboss/syncboss_direct_channel_mcu_defs.h)
defines `SPI_PACKET_TYPE_IMU_DATA` as **80** and restricts that direct-channel
implementation's supported packet range to this identifier. This is an upstream
routing definition, not evidence of packet 80 observed on the connected unit.

The [direct-channel implementation](https://github.com/facebookincubator/oculus-linux-kernel/blob/dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f/drivers/staging/oculus/mcu/syncboss/syncboss_direct_channel.c#L384)
reveals several constraints useful for a future receiver:

- It rejects a sample when the driver's NSYNC offset is invalid.
- It copies the driver header followed by `packet->data` into the 64-byte payload.
  Unlike the FIFO format, this copy omits the type/sequence/length prefix.
- Its outer timestamp is assigned with `ktime_get_ns()` while distributing the
  record. It must not be assumed to be the sensor's capture timestamp.
- A write barrier precedes publishing the incremented nonzero counter. Optional
  DMA fences wake readers; the storage is a wrapping ring.

The [public UAPI](https://github.com/facebookincubator/oculus-linux-kernel/blob/dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f/drivers/staging/oculus/include/uapi/linux/syncboss.h#L113)
defines the packed 104-byte record and DMA-buffer registration parameters. Its
union includes `float fdata[16]`, but the implementation writes raw header and
packet bytes. Interpreting those floats as Android accelerometer or gyroscope
values would be incorrect. The UAPI also names the internal consumer source
`libsyncboss/os_interface/syncboss_hal_impl_android_driver.c`; that pathname is a
useful research lead, not a published consumer implementation.

The generic [MCU message header](https://github.com/facebookincubator/oculus-linux-kernel/blob/dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f/drivers/staging/oculus/include/linux/syncboss/messages.h)
contains only packet framing. No acceleration/gyro field layout, units, sample
timestamp field, axis convention or calibration coefficients were found in
these inspected interfaces.

## Exact-unit information and calibration boundary

The read-only stock file
[`sensorservice.cfg`](../output/device-inventory/quest3-stock-52083180031500520/thermal-sensors/sensorservice.cfg),
SHA-256 `5b39783bf303a9366f18f487b5fb8fbc97f0e37f2755a03398783927891e6a73`,
sets `nice_constellation = "ruby"`, an 800 Hz default IMU rate and 25 Hz default
magnetometer rate. It supplies no sample layout, calibration path or transforms.
These defaults are configuration values, not a measured delivery rate.

There is an official limited metadata route under stock Horizon OS:
[MRUK's PassthroughCameraAccess](https://developers.meta.com/horizon/reference/mruk/v85/class_meta_x_r_passthrough_camera_access/)
exposes focal length, principal point, sensor resolution and `LensOffset`, defined
as the camera's pose relative to the headset. Meta's
[Camera API overview](https://developers.meta.com/horizon/documentation/spatial-sdk/spatial-sdk-pca-overview/)
limits this interface to the forward-facing RGB cameras through Android Camera2.
It does not document the raw tracking-camera calibration files or the IMU frame.
Inference: this could supply a later RGB metadata cross-check on the unchanged
stock OS, but cannot replace a camera-to-IMU calibration or a Linux camera driver.
No app was installed and no camera was opened for this investigation.

### Exact-build stock interface evidence

A later read-only ADB pass copied five readable ARM64 libraries from
`/system_ext/lib64` on the same `52083180031500520` unit. Each local SHA-256
matches the checksum read on the headset, and each ELF header identifies
AArch64. The binaries, exported symbols, dependency lists and string tables
are local-only evidence under
`output/device-inventory/quest3-stock-52083180031500520/sensor-libraries/`;
`manifest.json` records provenance. No copied binary was executed.

| Library | Observed interface evidence |
|---|---|
| `libsensorcontrol.so` | Depends on the Oculus sensor HIDL interface and `libvrsensors-hidlwrapper.so`, plus the data-transform library |
| `libsensordatatransforms.so` | Exports `transformImuData(OVR::Sensors::v1::ImuData const&, unsigned int)`, `transformTimeSyncData`, and IMU dispatch/configuration functions |
| `libsystemsensoraccess.oculus.so` | Imports `OVR::Sensors::HidlWrapper::v1::createImuSensor()` and contains the `syncbossWorkerPriority` configuration name |
| `libcalibrationmanager.oculus.so` | Imports `ICalibrationService::fromBinder`, exposes manager/producer/consumer entry points, and checks a versioned shared-memory calibration header |
| `libsensorutils.so` | Small helper library; its dependency list does not establish a raw MCU decoder |

The calibration manager contains the names `HMDCalibration`,
`imu_calibration.json`, `camera_calibration.json`, and left/right display offset
JSON paths. These are names embedded in executable code, not retrieved unit
calibration files or a verified schema. The IMU transform symbol accepts an
already-formed HAL `ImuData`; its existence does not identify the packet-80 wire
layout or prove calibration units.

The saved `imu-transform-disassembly.txt` covers the exported IMU transform at
ELF address `0x295d0` (240 bytes), neighboring magnetometer transform and time-sync
transform at `0x297a0` (180 bytes). The IMU function copies the two 12-byte regions
at input offsets 44 and 56, widens a float at offset 40, and copies existing timing
fields. It also appends a `clock_gettime(CLOCK_MONOTONIC)` timestamp in nanoseconds.
These observations identify a downstream structure conversion. They neither
label the two vectors' physical units nor decode Syncboss framing; treating the
new host timestamp as the original sensor sample time would be unsupported.

The same shell cannot read `/system_ext/bin/sensorproxy` or
`/vendor/lib64/libsensorcalibration.so`: both return permission denied. This
records a concrete access boundary, not evidence that these components are
absent. The copied HIDL/Binder clients cannot be treated as standalone glibc
Linux drivers. The useful next offline inputs remain the matching service-side
implementation and validated packet/calibration data.

## Reusable Monado integration points

The pinned [driver build list](https://gitlab.freedesktop.org/monado/monado/-/blob/01c1f6b23ab73c1459c8f4cfd2eb16d5142214c2/src/xrt/drivers/CMakeLists.txt)
contains Android, Rift/Rift S, simulated and remote drivers, but no Quest/Eureka
driver. The current [Monado hardware list](https://monado.freedesktop.org/#supported-hardware)
also does not list a native Quest 3 driver. This bounds the inspected upstream
support; it does not exclude unpublished or uninspected work. Android streaming
clients and synthetic remote poses do not supply this missing hardware driver.

The existing native interfaces are suitable once real sensor inputs are decoded:

| Interface | Required input |
|---|---|
| [`xrt_imu_sample`, `xrt_imu_sink`](https://gitlab.freedesktop.org/monado/monado/-/blob/01c1f6b23ab73c1459c8f4cfd2eb16d5142214c2/src/xrt/include/xrt/xrt_tracking.h#L133) | Timestamp in nanoseconds, acceleration in m/s², angular velocity in rad/s |
| [`xrt_slam_sinks`](https://gitlab.freedesktop.org/monado/monado/-/blob/01c1f6b23ab73c1459c8f4cfd2eb16d5142214c2/src/xrt/include/xrt/xrt_tracking.h#L208) | Per-camera frame sinks and a synchronized IMU sink |
| [`t_slam_calibration`, `t_slam_create`](https://gitlab.freedesktop.org/monado/monado/-/blob/01c1f6b23ab73c1459c8f4cfd2eb16d5142214c2/src/xrt/auxiliary/tracking/t_tracking.h#L624) | Intrinsics/distortion, IMU calibration, sample rates and column-major `T_imu_cam` for each camera |

`t_inertial_calibration` includes alignment/scaling matrices, offsets, bias and
noise parameters. Wiring zeros or identity matrices into this interface would
not validate the headset's factory calibration.

## Concrete remaining inputs

1. A version-matched packet-80 schema or analysis of the matching
   `libsyncboss` consumer, validated with labeled stationary and axis-motion
   captures. Establish field widths, signedness, scaling, batch structure and
   MCU timestamp behavior before producing `xrt_imu_sample` values.
2. The unit's calibration blobs with their encoding, provenance and validity
   checks, including IMU scale/bias/alignment and each tracking camera's
   intrinsics, distortion and IMU-relative extrinsics.
3. A camera stream with real image format, exposure timing, frame identifiers
   and a verified mapping into the same clock as the IMU. Kernel camera nodes
   and firmware filenames do not establish those semantics.
4. Replay validation of time conversion, dropped/out-of-order samples,
   camera/IMU synchronization, pose drift and relocalization. Physical motion
   and optical accuracy then remain necessary acceptance tests.

Packet routing and offline fixtures can advance independently. A complete 6DoF
result cannot be claimed until these inputs are obtained and validated; the
work remains part of the headset port.
