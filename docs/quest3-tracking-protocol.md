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

### Live stock-system capture

On build `52433670036000520`, the bounded
[`android-sensor-probe.c`](../src/android-sensor-probe.c) now receives live IMU
data through the stock Android API as the authorized ADB shell. Two five-second
queue captures delivered 4,007 and 4,008 events for handle 2/type 65537. Their
vendor timestamp increments are about 1,248 microseconds, despite the Android
field's documented nanosecond unit. The probe therefore preserves it as
`timestamp_raw`, alongside host CLOCK_MONOTONIC and CLOCK_BOOTTIME receipt times.
Handle 3/type 65538 is IAD, not an IMU clock-sync sensor.

The IMU advertises hardware-buffer direct reporting but no shared-memory direct
reporting. Three five-second hardware-buffer captures contain **12,032**
published packet-80 records. They have consecutive nonzero counters, driver
header version 2/length 12, valid NSYNC offsets, and integer-format-2 payloads.
The maintained [`syncboss_imu_decode_direct`](../src/syncboss-imu.c) accepts these
stable 104-byte records, preserves their raw ticks and delivery time separately,
and converts sample time using the signed NSYNC offset. Across those captures,
sample intervals are approximately 1.248 ms and sample-to-driver-delivery delay
is below 0.8 ms. This establishes live framing and clock conversion on this unit;
it does not establish calibrated axes or tracking accuracy.

The vendor returns zero from direct-report registration even while activating
the stream. The probe accepts a nonnegative result provisionally, then requires
published, consecutive records before reporting success. It stops and destroys
the channel before reading the buffer, avoiding a concurrent-ring snapshot.
Unpublished slots already contain size/type fields; only the nonzero counter
marks publication. See Android's
[direct-channel layout](https://developer.android.com/reference/android/hardware/SensorDirectChannel).
The live driver uses packet type 80 and raw header/payload bytes, not the standard
converted sensor-event payload.

Trying both routes concurrently yielded raw records but no converted queue
events. They are therefore captured separately; a same-sample scale comparison
has not passed. A later test with two independent processes and the queue started
first produced 7,211 queue events and zero direct records. Direct registration
and shutdown both returned zero; the probe correctly rejected the empty capture.
The contention therefore also occurs across clients, not only within one process.
Both captures and their receipts are in
`output/quest-imu-parameters-20261011-v1/independent-clients/`.

The live analysis supplies explicit candidate scale parameters,
including 128 LSB/degree C, and does not claim those values were read from the
MCU. Reading the active parameters, validating axes with labeled motion, camera
capture and tracking integration remain required.

Build the probe with an Android NDK API-29-or-newer AArch64 compiler and
`-std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror -landroid`.
With no arguments it lists non-private sensor metadata. Explicit handle,
duration (1–30 seconds) and period select a bounded queue capture.
`--direct NEW_FILE` selects a hardware-buffer snapshot instead. Use a fresh
temporary device path and save stdout/stderr and the raw file as private
evidence. It creates no app, changes no permissions and closes its own channels
on normal termination or SIGINT/SIGTERM. It does not guarantee cleanup after a
kernel/process crash. Evidence, failed experiments and source/binary hashes are
preserved under `output/quest-live-sensors-20261011-v1/`.

Host and ARM64 Linux sanitizer tests cover truncated and unaligned records,
unpublished counters, incorrect types/headers, invalid clock status, signed
offset underflow/overflow, both supported driver headers and all four payload
versions. Live capture is stock Android driver evidence, not native ArmadaVR
boot or physical tracking acceptance.

`syncboss_imu_stream_accept` provides a separate gate before tracking consumption.
It rejects future or inverted timestamps, non-finite samples, duplicate/reordered
counters, MCU-clock regressions and samples older than a caller-supplied limit.
Accepted counter gaps report missing records, including the nonzero-counter wrap
from `UINT32_MAX` to 1. Rejection leaves the previous stream state intact; a new
channel or MCU restart requires an explicit reset. It does not infer clock
freshness from unchanged offsets or silently join different channel lifetimes.

Replay of all 12,032 live records passes with a 5 ms age limit and each recorded
delivery time as simulated consumption time. Injected duplicates and stale
records are rejected, and 87 deliberately removed records are counted exactly.
This is replay validation, not a measured real-time tracking consumer. Synthetic
sanitizer tests additionally exercise counter wrap, invalid inputs and timestamp
limits. The preserved replay is `output/quest-live-sensors-20261011-v1/stream-replay.json`.

[`syncboss_direct_channel_mcu_defs.h`](https://github.com/facebookincubator/oculus-linux-kernel/blob/dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f/drivers/staging/oculus/mcu/syncboss/syncboss_direct_channel_mcu_defs.h)
defines `SPI_PACKET_TYPE_IMU_DATA` as **80** and restricts that direct-channel
implementation's supported packet range to this identifier. This is an upstream
routing definition, now also observed in the live captures above.

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
These names and values are recorded calibration metadata. Forward projection
has since been checked against the stock implementation below; inverse
projection, coordinate conventions, time offsets and correspondence to captured
images still need verification. In particular, the factory and online IMU matrices
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

### Fisheye62 projection and inverse

[`fisheye62.c`](../src/fisheye62.c) implements a shared focal length, six radial
coefficients and two tangential coefficients. Its parameter order is
`f, cx, cy, k0, k1, k2, k3, k4, k5, p0, p1`. The model agrees with Meta's
[published Fisheye62 equations](https://facebookresearch.github.io/projectaria_tools/docs/tech_insights/camera_intrinsic_models#the-fisheye62-model).
It is not interchangeable with a four-coefficient OpenCV fisheye model.

The authenticated build's `libtrackingengines.so`, SHA-256
`a462214606167735bb70f7112c72fed3bb9f2632b107d7d9f82005e4e4c65f29`,
contains the corresponding projection at ELF address `0xdb0420`. Its camera
model reports eleven total parameters, three projection parameters and eight
distortion parameters. Isolated comparisons against this original routine
cover 28,672 rays using the six recovered camera models and a synthetic model.
The maximum pixel difference is `1.1823431123048067e-11`. Only the math helper
runs, with its `atan` import supplied by the host; constructors, services and
device access are excluded. The comparison accounts for the routine's hidden
ARM64 return buffer in `x8`.

The maintained tests cover 37,441 projection cases per host, including each
distortion coefficient independently, the optical axis and spherical projection,
plus invalid inputs and output preservation. They pass under address/undefined
behavior sanitizers on macOS and ARM64 Linux. The API accepts finite front-facing
camera-frame points and rejects non-finite results. Image bounds, calibrated
field of view and camera-to-IMU transforms remain separate integration
requirements. No physical camera image or tracking pose is validated
by this mathematical comparison. Private analysis is retained in
`output/quest-tracking-524-20261011-v1/calibration-analysis/`.

`fisheye62_unproject` returns a camera-frame ray with Z=1 inside an explicit
angular limit supplied by the caller. It bounds tangential inversion, certifies
a positive radial derivative over the entire requested interval using Bernstein
bounds, and checks the final reprojection. Non-invertible or uncertifiable
models, out-of-range pixels and failed convergence preserve the output. The
angular limit is not inferred from image dimensions or calibration coefficients.

All six recovered models and a synthetic model pass 28,672 comparisons with
the stock inverse at ELF `0xdb01a0`, through 1.4 radians from the optical axis.
The largest ray-component difference is `1.1874945471390674e-12`. The isolated
comparison substitutes only the `tan` import and does not run constructors or
services. Another 28,381 maintained cases plus rejection checks pass sanitizers
on macOS and ARM64 Linux. Evidence is in
`output/quest-inverse-projection-20261011-v1/`. This establishes model math;
applying it to cropped or rescaled Camera2 output still requires a verified
mapping, and the 1.4-radian test limit is not a measured physical field of view.

### Temperature offsets and IMU rectification

The same tracking library contains `viper::PiecewiseLinearTemperatureImuModel`.
Its sample method at ELF `0x1f0f670` interpolates the three raw offset components
linearly between neighboring temperature rows, clamps to the nearest endpoint
outside the table, and then computes `rectification * (raw - offset)`. A
one-row table gives a constant offset. Accelerometer and gyroscope use separate
tables and matrices. The constructor copies float matrices; the sample method
uses their column-major internal representation.

[`imu-calibration.c`](../src/imu-calibration.c) provides checked temperature lookup
and vector rectification with an explicit row-major matrix API. It rejects
non-finite input, unordered tables and arithmetic overflow without changing the
output. Unlike stock's first-row fallback for non-finite temperature, the new
helper rejects that sample. A caller must explicitly convert the calibration
into the agreed units and frame before using these functions.

Comparisons cover 20,036 original stock samples, or 40,072 acceleration/gyro
vectors, including the unit's full temperature tables, exact row temperatures,
both out-of-range endpoints and constant tables. The largest component
difference is `1.9073486328125e-6`, within float rounding. Both implementations
receive the same explicitly prepared matrices; this does not prove how the
stock JSON parser maps `DeviceFromImu` and `RectificationMatrix` into those
runtime matrices. That mapping, timing corrections and calibration selection
remain open. Host and ARM64 Linux sanitizer tests also pass 801 temperature
cases plus input validation, matrix-axis and output-preservation checks.

The constructor also calls a matrix split at ELF `0xae3b90`. Isolated execution
establishes `matrix = Q * R`: `Q` is a proper rotation and `R` is upper triangular
with positive diagonal entries. `imu_split_rectification` implements that split
with row-major inputs/outputs, rejecting reflections, near-singular pivots and
non-finite results without changing outputs. The stock helper uses column-major
storage. The maintained implementation agrees with 4,100 original calls,
including full 3D rotations and all four factory/online accelerometer/gyro
matrices, within `8.881784197001252e-16` per component. Another 12,288 scale and
rotation cases plus rejection checks pass host and ARM64 sanitizers.

The constructor's math prefix at ELF `0x1bd5550` establishes the composition:
for accelerometer matrix `Ma = Qa * Ra`, gyro matrix `Mg`, and device rotation
`D`, its aligned branch produces `Ra`, `transpose(Qa) * Mg`, and `D * Qa`.
Translation remains unchanged. Both sensors then share the accelerometer frame;
the adjusted device rotation preserves their device-space interpretation.
The other branch keeps both complete matrices and the original device rotation.
Offsets are subtracted in the raw sensor frame before rectification.

`imu_prepare_rectification` implements these two explicit choices. It requires
an already proper orthonormal device rotation, valid invertible calibration
matrices, and float-representable results. Invalid input leaves the output
unchanged. It does not select factory versus online calibration or assume which
branch the live service uses.

The API matches **8,196** isolated stock-prefix executions: 8,192 synthetic
cases and both modes for the unit's factory and online matrices. Synthetic
accelerometer/gyro float outputs match exactly; device rotation differs by at
most `4e-15`. Another 8,192 frame-invariance cases and invalid-input checks pass
macOS and ARM64 Linux sanitizers. The comparison intercepts the constructor
after its math and before configuration access or model allocation; no ELF
initializers, services or hardware are invoked. Sources and receipts are in
`output/quest-frame-composition-20261011-v1/`. The decomposition investigation
remains in `output/quest-imu-parameters-20261011-v1/`.

The initializer at ELF `0x1b65200` passes a literal `1` as the eleventh argument
to `0x1b899a0`. That wrapper inverts the bit before calling `0x1bd5550`, selecting
the accelerometer-aligned branch. The other observed constructor caller,
`0x1bd0ba0`, also supplies mode zero. Stack offsets and instructions confirm
this static call-path choice; it is not a trace of the active headset runtime.
The separate selector at `0x28cb950` can prefer a second calibration model. Its
initializer sets a false default and registers a runtime configuration entry,
so that default does not establish the live factory/online choice. Instruction
excerpts and the call-path receipt are in
`output/quest-frame-composition-20261011-v1/`.

Active model selection, time corrections and camera-to-IMU mapping still need
verification before a calibrated physical tracking claim.

The calibration JSON parser at ELF `0xca9c00` stores `DtAccelRef` as a double at
input offset `0x1b8` and `DtGyroRef` at `0x1b0`; the serializer at `0xa8c160`
confirms that mapping. Constructor `0x1bd5550` uses these values unless a
nonzero runtime override replaces each with the negated override. The
temperature-model constructor stores the resulting pair separately at object
offsets `0x8` and `0x10`. This identifies the fields and override behavior, but
does not establish the camera clock mapping. The downstream IMU resampling
behavior is now verified separately below.

The wrapper at ELF `0x1b899a0` converts each correction from seconds to signed
nanoseconds, truncating toward zero. Consumer `0x1b8a440` invokes resampler
`0x1e6b560` before the model's temperature compensation and rectification.
The resampler adds each correction to capture time. It emits at the smaller
corrected time, retaining that sensor's current vector and interpolating the
other vector from five samples. Equal corrections preserve both vectors. The
current sample supplies temperature. This identifies IMU-to-IMU alignment,
without establishing a shared camera clock or the active calibration model.

[`imu-time-align.c`](../src/imu-time-align.c) implements this alignment with
explicit nanosecond offsets. It withholds output until real history brackets
the requested time, discards history across a configured gap, and rejects
invalid or out-of-order samples without changing state or output. It does not
copy the stock routine's zero-filled startup interpolation. Callers must reset
on channel restart and supply enough samples within its five-record capacity.
It remains a separate component: the current `quest_imu_process_direct` API
does not automatically enable it or choose offsets.

All 245,760 retained-output comparisons against the original ARM64 resampler
are bit-identical, across positive, negative and equal offsets, irregular
intervals and random vectors. The isolated comparison executes only that
routine, without ELF initializers or hardware. Another 6,208 linear-vector
checks and stream-boundary cases pass macOS and ARM64 sanitizers. The existing
8,192-case calibrated processor test also passes on the Quest Android ABI;
that executable accesses no sensors. Sources and receipts are retained in
`output/quest-imu-time-alignment-20261011-v1/`.

The retained factory and online JSON files already preserve the respective
timing fields. Replaying all 12,032 captured direct records through stream
validation, the new alignment stage and then calibration produces 24,061
aligned model samples. Three factory startup records are withheld for history.
Both supported rectification frames preserve device-space vectors within
`2.27e-6`. This uses each saved model separately, its stored corrections without
runtime overrides, and the same unqueried candidate MCU scales; it does not
select the model currently used by the headset.

Combined offline replay now runs all 12,032 preserved live records through the
raw decoder, stream-order gate, temperature offsets and frame preparation for
both factory and online models. Both frame choices preserve the resulting
device-space vectors within float rounding. The replay uses recorded delivery
times as simulated consumption times and the same explicit candidate MCU
parameters described above. It applies no time correction or translational
lever-arm compensation and does not select the live model. This verifies the
components together, without establishing real-time consumption, labeled-axis
accuracy or pose tracking. Input hashes and results are preserved in
`output/quest-calibrated-replay-20261011-v1/`.

[`quest-imu.c`](../src/quest-imu.c) connects these components into a calibrated
direct-record processor. Initialization requires one explicit calibration,
frame, MCU format/conversion parameters and age limit. It copies the prepared
matrices and retains immutable offset tables owned by the caller. Initialization
failure preserves an existing stream; successful reinitialization deliberately
starts a new one. Sample rejection also preserves the stream and output, even
when calibration arithmetic fails after the raw record passes ordering checks.

Accepted vectors remain in the selected IMU frame. The prepared device rotation
is available to the consumer, but no extrinsic, time correction, camera pairing
or pose estimate is applied. This component does not acquire a ring, select a
unit calibration, open hardware or install a Monado tracking driver.

The processor passes 8,192 synthetic stream cases across all four wire formats
and both frames, plus rejected initialization, duplicate/stale records, counter
wrap, arithmetic overflow and retry checks under macOS and ARM64 sanitizers.
It also reproduces the component-by-component replay for all 12,032 retained
live records using factory/online models and both frames: 48,128 exact vector
comparisons and 96,256 state-preserving rejection checks. Recorded delivery
times simulate consumption; these checks do not measure a live consumer.
Evidence is in `output/quest-imu-pipeline-20261011-v1/`.

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
The current-build native Camera2 metadata probe enumerates IDs 1, 50 and 51.
IDs 50/51 expose RGB intrinsics and camera poses whose translations match the
recovered calibration entries 4/5. This is a metadata correspondence, not a
verified camera-to-IMU transform or Linux camera driver. Android reports an
unknown timestamp source and does not expose the Fisheye62 coefficients through
the standard distortion tag.

Opening camera 50 as the ADB shell succeeds. Bounded YUV capture attempts at
320x240 and native 1280x960 produce no image callbacks, both asleep and after a
temporary wake. Their sessions close normally. A subsequent active
`media.camera` dump stalls, the probe times out, and the virtual-camera provider
then exits. The normal stock reboot restores all three camera IDs and an idle
bridge on the same firmware with verified boot green. No shell-probe frame was
obtained. Meta's [Camera API prerequisites](https://developers.meta.com/vr/documentation/spatial-sdk/spatial-sdk-pca-overview/)
require passthrough to be enabled.
Do not repeat the active camera dump. Metadata, probe sources, failures and
recovery receipts remain in `output/quest-live-sensors-20261011-v1/`.

A subsequent bounded foreground Android diagnostic, with the passthrough
feature, both camera permissions, a preview surface and a YUV image reader,
received nine actual 1280x960 frames from camera 50. Its setup follows Meta's
[native Android sample](https://github.com/meta-quest/Meta-Passthrough-Camera-API-Samples/tree/f079db625e3bb60a811d4f36f6af200cb83e330b).
It records image timestamps, exposure metadata and sampled brightness, without
saving images. Capture ends when Android pauses the activity; the bridge then
returns to its stopped state without a reboot. The short capture does not
establish sustained frame rate, camera-to-IMU synchronization or image quality.

Two launches after incremental APK replacement did not reach application
startup. A normal APK replacement reached startup, but Android paused it before
creating the preview surface. A system Guardian dialog was active around these
attempts; its causal role remains an inference. App sources, APKs, lifecycle
logs and original measurements remain under the private `camera-app/` directory.

With the wearer at the home screen after clearing normal boundary prompts,
the same installed diagnostic subsequently captured 297 distinct 1280x960
frames from each of cameras 50 and 51 in separate five-second runs. Both runs
have 297 matching capture results, strictly increasing image timestamps and no
capture errors. Median image-delivery differences from `CLOCK_MONOTONIC` are
28.1 and 30.1 ms; Android still declares the timestamp source unknown, so those
differences do not prove clock synchronization or end-to-end latency. No image
pixels are retained, only sampled brightness and timing metadata. The runs
closed normally without a reboot and the diagnostic was stopped. Evidence is
in `output/quest-camera-worn-20261011-v1/`. These are stock Android RGB streams;
raw tracking-camera access and Linux camera-driver acceptance remain open.

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
   offsets and inverse Fisheye62 projection. Select the appropriate factory/online model
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
