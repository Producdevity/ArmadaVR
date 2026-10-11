# Quest 3 tracking protocol and native driver inputs

Checked 2026-09-10 against Meta's pinned kernel
`dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f`, the project's Monado source revision
`01c1f6b23ab73c1459c8f4cfd2eb16d5142214c2`, and current primary documentation.
No physical-device command or firmware installation was performed.

The complete native tracking driver remains required. The public source inspected
provides an IMU packet identifier and transport interfaces. The October 11
firmware analysis below adds a tested raw sample decoder, but factory calibration
and native Quest 3 Monado integration remain unverified. The existing
[`syncboss-replay.py`](../tools/syncboss-replay.py) correctly leaves sensor payloads
opaque; decoding the FIFO wrapper does not produce calibrated motion.

## Current firmware IMU conversion

On October 11, offline analysis of authenticated build `52433670036000520`
identified four raw IMU formats in `/odm/lib64/libsyncboss.so`, SHA-256
`a3164d43adb9de5eddb9fa38fd221701a6c4bbf3faa12a216a462b4831555c57`.
The containing ODM image was verified against the retained signed firmware.
Ghidra decompilation was checked against AArch64 instructions and constant bytes.
No stock service or device initialization was run.

[`syncboss-imu.c`](../src/syncboss-imu.c) implements the conversion behind the
bounded [`syncboss_imu_decode`](../src/syncboss-imu.h) interface:

| Format | Required bytes | Motion fields | Timestamp and device |
|---|---:|---|---|
| 1 | 36 | Six float32 values and float32 temperature | Full first uint64 timestamp; device 0 |
| 2 | 22 | Six signed int16 values and signed int16 temperature | Full first uint64 timestamp; device 0 |
| 3 | 22 | Same integer fields as format 2 | Upper 48 timestamp bits; low four device bits |
| 4 | 36 | Same float fields as format 1 | Upper 48 timestamp bits; low four device bits |

These sizes are the consumed fields, not measured transport record lengths.
The decoder accepts trailing padding up to the FIFO payload limit of 252 bytes.
All wire fields are little endian and unaligned input is supported. It returns
acceleration in m/s², angular velocity in rad/s and temperature in °C. Timestamp
ticks remain unconverted, with no axis remapping, factory bias/alignment or NSYNC
offset applied. It rejects incomplete payloads, unknown versions, invalid scales
and non-finite outputs, leaving the caller's sample unchanged on failure.

The Eureka device descriptor selects format 2. Its built-in fallback conversion
uses 16.384 LSB/(°/s), 4096 LSB/g, 126.8 LSB/°C and 25 °C offset. Firmware also
requests conversion parameters from the MCU; the fallback is not evidence of
the connected unit's active values. Callers must supply the version and
parameters explicitly. The integer converter truncates its temperature scale
and offset to signed integers before calculating temperature. Its optional
temperature compensation uses a factor of `2^-17`, separately rounded float32
operations, and different coefficients for the three axes. This behavior is
preserved; it is distinct from factory calibration.

Validation used only the four isolated conversion entry points from the pinned
ELF in a network-disabled ARM64 Linux process, without ELF constructors, service
startup or hardware access. All **24,000** synthetic cases match the original
44 output bytes exactly, covering every version and both integer compensation
modes. Six fixed vectors are retained in [`tests/syncboss-imu.c`](../tests/syncboss-imu.c).
Its **1,382** conversion/rejection checks pass with AddressSanitizer and
UndefinedBehaviorSanitizer on both macOS ARM64 and Linux ARM64. `just check`
also compiles and runs the native fixture. Compile without fast-math; the test
builds use `-ffp-contract=off`.

Private firmware, Ghidra projects, comparison runner and results remain under
`output/quest-tracking-524-20261011-v1/`. These tests establish offline conversion
compatibility only. Live packet selection, clock stability, active MCU scales,
sensor axes, calibration and physical tracking acceptance remain open.

### FIFO framing and clock conversion

`syncboss_imu_decode_fifo` combines the raw decoder with the published driver
framing. It accepts exactly one packet-80 record with header version 2 or 3,
the matching header length, a valid NSYNC offset, and HMD device ID 0. Other
packet types, driver messages, unknown states, truncation and trailing records
are rejected. The sample format and conversion parameters remain explicit.
The version-3 remote offset is not applied to this local HMD clock.

The kernel computes NSYNC as `ktime_to_us(ktime_get()) - mcu_timestamp_us`.
The exact-build stock `syncboss_get_spi_imu_api_payload` at ELF address `0x51908`
requires a valid NSYNC status, then adds that signed offset to the decoded MCU
timestamp. The maintained FIFO decoder preserves the raw timestamp and exposes
`(mcu_timestamp_us + nsync_offset_us) * 1000` as signed CLOCK_MONOTONIC
nanoseconds. It checks negative results and overflow before conversion, including
the minimum signed offset. Failure leaves the output unchanged. Direct-channel
ring ownership and its host delivery timestamp are separate from this FIFO API.

All **16,000** synthetic comparisons against the original stock wrapper agree
on sample bytes and converted clock values across both headers and all four
sample formats. The stock routine is invoked with its header-plus-payload
layout; FIFO type/sequence/length bytes are separately validated by the new API.
The **2,312** framing and clock checks pass with address/undefined-behavior
sanitizers on macOS and ARM64 Linux. These cover unaligned inputs, every truncated
length, malformed headers, both offset signs and integer boundaries.

This supplies a bounded parser for a future native receiver. Live delivery,
offset freshness, clock resets, dropped samples and physical axis/calibration
validation remain required before feeding a tracking estimator.

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

### Calibration recovered through stock diagnostics

On October 11, a read-only `dumpsys sensorservice` capture from build
`52433670036000520` exposed the unit's calibration in the JSON name of the
`Syncboss IMU Android Sensor` descriptor. Its nested `calibration.factory` and
`calibration.online` strings both identify the connected Eureka unit and
`ICM45688` IMU. The descriptor advertises 800 Hz; that is not a measured sample
delivery rate. No sensor was enabled, app installed, permission changed or
service restarted to obtain this data.

The factory blob contains constant IMU offsets, rectification matrices and
`DeviceFromImu`. The online blob includes temperature-dependent offset tables,
updated IMU matrices and six cameras:

| Camera IDs | Sensor | Image size | Shutter |
|---|---|---|---|
| 0, 1 | OG01A | 1280 × 1024 | Global |
| 2, 3 | OV7251 | 640 × 480 | Global |
| 4, 5 | IMX471 | 2328 × 1748 | Rolling, about 10.75 ms readout |

Each camera includes `DeviceFromCamera`, `PinholeSymmetric` projection,
eight-coefficient `Fisheye62` distortion and an unprojected field-of-view boundary.
These names and values are recorded calibration metadata; distortion equations,
coordinate conventions, time offsets and correspondence to captured images
still need verification. In particular, the factory and online IMU matrices
distribute rotation differently. Applying both sets would risk applying the
same correction twice.

[`extract-quest-calibration.py`](../tools/extract-quest-calibration.py) imports an
offline capture with an explicit expected serial and a new private destination:

```sh
python3 tools/extract-quest-calibration.py CAPTURE --serial EXPECTED_SERIAL --output NEW_DIRECTORY
```

It preserves the embedded JSON strings byte-for-byte with hashes, checks both
unit identities and supported schemas, and validates matrix dimensions,
non-singularity, rigid transforms, unique camera IDs and strictly ordered
temperature tables. Unknown models, duplicate keys, non-finite values,
incomplete captures and existing output directories are rejected. Unit files
are created with mode 0600 inside a 0700 directory. The parser does not apply
calibration or authenticate the original capture; its read-only ADB provenance
is recorded separately.

The original capture and both exact blobs remain private under
`output/quest-tracking-524-20261011-v1/runtime-diagnostics/`. Public tests use
synthetic calibration only. This closes the missing calibration-data input;
physical validity, interpretation and native tracking integration remain open.

### Earlier configuration and library evidence

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

1. Validate the decoded raw formats with labeled stationary and axis-motion
   captures. Confirm the active variant and conversion parameters, packet
   selection, batch structure, sensor axes and MCU timestamp behavior before
   producing `xrt_imu_sample` values.
2. Verify the recovered calibration's units, matrix/application order, time
   offsets and Fisheye62 equations. Select the appropriate factory/online model
   and map each camera into the IMU frame without applying corrections twice.
3. A camera stream with real image format, exposure timing, frame identifiers
   and a verified mapping into the same clock as the IMU. Kernel camera nodes
   and firmware filenames do not establish those semantics.
4. Replay validation of time conversion, dropped/out-of-order samples,
   camera/IMU synchronization, pose drift and relocalization. Physical motion
   and optical accuracy then remain necessary acceptance tests.

Packet routing and offline fixtures can advance independently. A complete 6DoF
result cannot be claimed until these inputs are obtained and validated; the
work remains part of the headset port.
