# Quest 3 tracking protocol and native driver inputs

Checked 2026-09-10 against Meta's pinned kernel
`dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f`, the project's Monado source revision
`01c1f6b23ab73c1459c8f4cfd2eb16d5142214c2`, and current primary documentation.
No physical-device command or firmware installation was performed.

The complete native tracking driver remains required. The public source inspected
provides an IMU packet identifier and transport interfaces. The October 11
firmware analysis below adds a tested raw sample decoder, but active factory calibration and physical native Quest 3 tracking remain unverified.
The native runtime and calibrated Monado adapter now have the offline verification
described below. The existing
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
versions. Live capture is stock Android driver evidence, not native Reframed VR
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

The counter is published after payload writes, but the writer does not invalidate
the old counter before reusing a slot. Reading the same nonzero counter before
and after a copy therefore does not certify a stable record. Two equal copies
also fail if the writer pauses partway through an overwrite. A deterministic
test of that permitted interleaving produces new capture time and acceleration
with old gyro and temperature; ordinary decoding and age/order checks accept
the mixture. The fences implement notifications, not exclusion against writes.
The producer explicitly omits DMA-buffer CPU access locking around its writes.

The reproduction and exact pinned producer source are preserved in
`output/quest-ring-consumer-20261011-v1/`. This is an offline race demonstration,
not an observed failure on the headset. Existing captures stop and destroy the
channel before copying and are unaffected. A future live receiver must establish
an overwrite-safe acquisition contract, or use the driver's copied FIFO route;
the calibrated processor continues to require a stable record from its caller.

The copied FIFO allocates a separate queue per open file and returns complete
records through `miscfifo_fop_read_many`. It does not overwrite unread queue
entries; a full queue drops new records. Streaming packets have sequence zero,
so this interface cannot count missing IMU samples. Also, a successful
direct-channel delivery returns `NOTIFY_STOP` before the lower-priority FIFO
subscriber runs. A FIFO consumer cannot assume it receives packets already
claimed by stock direct-channel clients.

`quest_imu_process_fifo` feeds a complete copied packet through the same time
alignment, temperature compensation and rectification as direct records. It
requires sequence zero, increasing MCU/capture timestamps and bounded sample
age. The caller supplies the monotonic time immediately after reading; that
time is reported as delivery time, not sensor capture time. Output counter zero
means unavailable, not zero loss. Excessive capture gaps reset interpolation
history. Switching transports requires reinitialization. Rejections preserve
state and output, while history warmup consumes the record without output.

All 12,288 synthetic FIFO/direct comparisons across four payload formats, both
driver headers, both frames and three time corrections agree exactly. Existing
direct-record tests also pass under macOS and ARM64 Linux sanitizers. This is
offline processing verification; the sensor-free executable also passes on the
stock Quest's Android ABI. A separate passive stock-Quest probe on
October 12 received `EACCES` opening `/dev/syncboss_stream0`; no read or ioctl
followed, and its temporary executable was removed. Native FIFO acquisition,
MCU restart handling and physical tracking remain unverified. Evidence is in
`output/quest-fifo-pipeline-20261012-v1` and
`output/quest-fifo-acquisition-20261012-v1`.

### Native copied-FIFO owner

`quest-imu-stream.c` owns a nonblocking `/dev/syncboss_stream0` descriptor,
installs the exact ARM64 packet-80 filter with per-client queue clearing, and
feeds complete copied batches through the calibrated processor. Each poll is
bounded to eight 4 KiB reads and refreshes CLOCK_MONOTONIC around acquisition
and delivery. It validates a whole batch before publishing any of its samples.

Read errors, malformed or truncated batches, clock regressions, excessive
capture gaps, stale input and sink refusal invalidate the generation and close
its descriptor once. Empty reads still run the timeout check. Failure cannot
implicitly restart the stream; a new owner is required. The consumer must also
stop it on suspend, MCU restart or loss of exclusive sensor ownership before
publishing further poses.

The owner does not enable or configure the MCU, claim direct-channel ownership,
select calibration or open a device automatically. The offset tables must remain
alive and immutable. Closing this copied FIFO releases the client's queue; it
is not evidence of MCU shutdown or camera DMA quiescence. A successful existing
direct-channel subscriber can still suppress FIFO delivery, which this receiver
handles as silence rather than fabricating samples.

The 1,101 host and ARM64 Linux sanitizer cases compare calibrated output across
all four formats, both FIFO headers and optional time alignment. They also cover
batch rejection before delivery, startup rollback, empty-input expiry, bounded
interruption retries, clock/read/sink failures and single-close invalidation.
Transport syscalls are modeled; no physical FIFO acquisition is established.

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
The direct-record processor uses this component when its caller supplies an
explicit timing configuration; it does not choose offsets automatically.

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
failure preserves an existing stream and alignment history; successful
reinitialization deliberately starts both again. Sample rejection preserves
both states and the output, even when calibration arithmetic fails after raw
ordering checks or time interpolation.

Accepted vectors remain in the selected IMU frame. The prepared device rotation
is available to the consumer. Optional timing configuration applies alignment
before temperature compensation and rectification. A null timing configuration
retains unaligned diagnostic behavior. No extrinsic, camera pairing or pose
estimate is applied. This component does not acquire a ring, select a
unit calibration, open hardware or install a Monado tracking driver.

The processor passes 8,192 synthetic stream cases across all four wire formats
and both frames, plus rejected initialization, duplicate/stale records, counter
wrap, arithmetic overflow and retry checks under macOS and ARM64 sanitizers.
It also reproduces the component-by-component replay for all 12,032 retained
live records using factory/online models and both frames: 48,128 exact vector
comparisons and 96,256 state-preserving rejection checks. Recorded delivery
times simulate consumption; these checks do not measure a live consumer.
Evidence is in `output/quest-imu-pipeline-20261011-v1/`.

The integrated timing path returns a distinct effective timestamp and preserves
the current raw capture timestamp, MCU ticks, temperature, delivery and counter.
Both raw and effective timestamps must be within the caller's age limit and
not in the future. `QUEST_IMU_NEEDS_HISTORY` consumes a valid raw record and
reports missing records, while withholding the output sample. Counter loss
discards interpolation history even when the time gap is short. Capture gaps
above the configured limit also restart history. Rejected records do not
advance either state, so a corrected retry remains possible.

The combined processor passes 9,952 aligned cases and 288 history resets across
all four formats, both frames, positive/negative/equal corrections, changing
temperature, counter wrap, loss and long gaps. Overflow, stale corrected time,
invalid configuration and calibration-failure retries are also checked under
macOS and ARM64 Linux sanitizers. Against the preserved component library, the
12,032 raw captures produce 48,122 bit-identical calibrated outputs across both
models and frames, six withheld startup outputs and 96,256 state-preserving
rejections. Evidence is in `output/quest-aligned-pipeline-20261011-v1/`.
The live model, MCU scales, camera clock relationship, ring acquisition and
physical pose tracking remain unverified.

The same combined test also passes as a standalone Android ARM64 executable
on the connected Quest: 9,952 aligned cases, 288 history resets and 8,192
uncorrected calibration cases. Its hash is checked before execution; it accesses
no sensors, leaves the boot session unchanged and removes its temporary files.
The receipt is `output/quest-aligned-pipeline-20261011-v1/android-math-test.json`.
This verifies the target ABI and math, not physical sensor alignment.

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
2. Select the active factory/online model and runtime timing overrides, confirm
   the physical axis conventions, and map each camera into the IMU frame without
   applying corrections twice. Matrix order, time resampling and Fisheye62 math
   now have the isolated stock comparisons and integrated replay described above.
3. A camera stream with real image format, exposure timing, frame identifiers
   and a verified mapping into the same clock as the IMU. Kernel camera nodes
   and firmware filenames do not establish those semantics.
4. Replay validation of time conversion, dropped/out-of-order samples,
   camera/IMU synchronization, pose drift and relocalization. Physical motion
   and optical accuracy then remain necessary acceptance tests.

Packet routing and offline fixtures can advance independently. A complete 6DoF
result cannot be claimed until these inputs are obtained and validated; the
work remains part of the headset port.

## Camera shutdown errors

The Linux kernel patch set preserves errors from the Quest camera sensor,
platform-power and CSI-PHY shutdown paths. Previously, sensor regulator/pinctrl
and CCI-release failures could return success, platform shutdown discarded
regulator errors, and the PHY stop handler shadowed its returned status. The
PHY handler also now rejects incomplete user copies and invalid stop counts or
handles before changing ownership counters.

Cleanup keeps the vendor ordering, optional GPIO-table validation, nonfatal BoB
policy, hardware-no-op modes, aggregator branch and startup unwind. Where several
cleanup operations fail, the caller receives the first reported error. This does
not prove that rails are off, CSI has stopped, DMA is quiescent, or retrying the
same stop request is safe. Regulator-core failures that its own API hides remain
outside these patches.

The regression tools compare exact excerpts from the pinned Quest kernel with
the patched code: 4,610 sensor cases, 2,372 platform-power cases and 600 PHY cases.
They model hardware operations and check cleanup ordering and reported failures
under host and ARM64 ASan/UBSan. Full-source patch application and kernel builds
are separate checks; none establish physical camera operation or flash readiness.

## Native VIT tracking integration

`quest-slam-runtime.c` connects the copied Syncboss FIFO owner to the bounded VIT
session and Monado pose-history provider shared with the Pico pipeline. It uses
the session's mono8 interface and does not open SSC or decode Pico snapshots.
Preparation validates the supplied IMU configuration without opening a device;
explicit start opens the FIFO once. The first aligned IMU sample starts the
estimator. Camera/IMU gaps, rejected samples, backend errors, clock regression
and clock-map expiry invalidate the pose provider and end the owner. Restart
requires recreation, explicit sensor ownership and a new generation.

The caller must supply the active calibration and MCU scales, IMU alignment,
camera order, dimensions, exposure-center timestamps, bounded clock uncertainty,
and matching camera-to-IMU and head transforms. The interface does not configure
the MCU or cameras, establish DMA quiescence, or enable a Monado auto-prober.
Temperature tables and the VIT library/configuration must outlive the owner;
stop capture callbacks and release HMD references before destruction.

The native Monado bundle installs this owner as `libquest_tracking.so`. Its
exported functions provide lifecycle, input and locked pose queries; the
shared session internals remain private to the library. It links no Pico tracking
or SSC library and registers no automatic hardware probe. Package verification
checks ARM64 architecture, exports, dependencies and invalid-input refusal on load.

Host and ARM64 sanitizer tests exercise 212 lifecycle and fault cases with the
real shared session and Monado pose history, modeled FIFO input and a modeled VIT
backend. Separately, 12,032 retained stopped-channel records replayed through the
new FIFO owner produce 48,122 bit-identical calibrated samples across two saved
calibration candidates and two rectification frames. Six history warmups are
expected. Those records are reframed into FIFO messages with a simulated read
clock; this does not validate live FIFO acquisition, MCU scales or active model
selection.

The maintained `tools/test-pico-slam-replay.py` also exercises the installed
Quest library with the native Basalt estimator. Its test-only FIFO interposer
supplies calibrated reference IMU vectors in copied packet format and controls
only the Quest owner's monotonic clock. Basalt workers use the real host clock;
reference camera images are copied through the production input owner before the
caller overwrites its buffer. The reference run produces 185 estimator poses,
184 tracked pose queries, 370 images and 3,467 IMU samples. Camera silence with
continuing IMU input and clock regression invalidate poses before an explicit
stop and refuse implicit restart. Fresh processes repeat the normal run.

This uses the Valve Index reference dataset and its calibration, not Quest camera
frames or measured Quest accuracy. Native ARM64 replay passed the existing rigid
trajectory limits without fitting scale; the accompanying Pico replay and fault
checks also passed. Evidence is retained in
`output/quest-offline-parity-20261015-v1/installed-replay-v1/acceptance.json`.

## Basalt lens compatibility

The pinned Basalt `Fisheye624Camera` represents Fisheye62 by setting both focal
lengths to the shared focal length, keeping the six radial and two tangential
coefficients, and setting its four thin-prism coefficients to zero. This permits
native image coordinates without a pinhole remapping stage. Image dimensions,
crop, orientation, calibrated field of view and physical camera identity must
still match the calibration.

The original implementation returns nonfinite projections on the optical axis
and nonfinite float derivatives near it. Its inverse also returned success after
failed or incomplete convergence. Patch `0005-stabilize-fisheye624.patch` uses a
stable chain-rule derivative with the analytic on-axis limit and bounded inverse
iteration with convergence checks. Host and ARM64 sanitizer regressions cover
10,334 projection, all-parameter derivative, inverse and invalid-input checks;
the original code fails 982 assertions. These numerical tests do not establish
camera capture or headset tracking accuracy.

`tools/quest-slam-calibration.py` exports an explicit unit-calibration candidate
for Basalt's native `fisheye624` model. The caller selects two to four global-shutter
camera IDs in VIT order, the original or accelerometer-aligned IMU frame, and a
separate measured rate/noise specification. It rejects rolling-shutter cameras,
ambiguous IDs, reflected rectification and unsupported models. It preserves the
full image geometry; cropped, resized, rotated or reflected streams need a
separately verified mapping.

Camera extrinsics are `inverse(DeviceFromEffectiveImu) * DeviceFromCamera`.
Accelerometer alignment uses the same proper QR rotation as the native IMU
processor. Basalt's residual IMU calibration and camera time offset are zero
because the input owner already calibrates IMU vectors and maps exposure-center
timestamps. Applying the unit corrections again inside Basalt would be wrong.
The output records its source hash, camera order, effective IMU transform and
calibrated field-of-view boundary in a private manifest. It neither enables a
sensor nor establishes that these are the active camera IDs, rate, noise or
calibration on the device. Those confirmations remain required before live use.

## Native camera inventory

`tools/pico-camera-inventory.py --target quest3 --media /dev/mediaN` reuses the
bounded media-controller collector with the Quest kernel's 21 entity roles.
It opens only the explicitly selected media node and issues device-info and
topology queries. It does not open camera subdevices, probe a sensor, allocate
buffers or start capture. The target selects a known ABI; it does not authenticate
the headset or infer the active camera pair from node names.

The native ARM64 check compares the collector with the pinned Quest media and
camera headers. Common control structures have matching sizes, but the Quest
request-manager opcodes differ: create-session is `0x10f`, link is `0x111` and
schedule is `0x113`, compared with Pico's `0x10b`, `0x10d` and `0x10f`. Pico's
control layer cannot be used unchanged on Quest. Camera configuration, native
capture, exposure timing and DMA quiescence still require their own validation.

`src/quest-camera-control.c` queries explicitly identified sensor, CSI PHY and ISP
descriptors without acquiring devices or issuing configuration/start commands.
Quest's ISP response contains eight hardware classes (216 bytes), rather than
Pico's five (144 bytes). The wrapper retains the reported hardware counts and
nonsecure MMU handles and rejects out-of-range response counts. Queries preserve
caller output on errors and do not retry interrupted calls. Opening or closing
the CRM node remains the caller's responsibility and has global side effects.

`tools/test-quest-camera-control.py` builds the transport against the exact Quest
vendor headers in ARM64 Linux. Its ioctl substitute interprets the real vendor
structures, writes all eight ISP entries, and covers zero/full/invalid counts,
partial copy-back followed by errors, positive ioctl returns and invalid inputs
under ASan/UBSan. This proves payload compatibility and bounded handling, not
hardware capabilities or capture. Capability queries do not identify which
physical camera corresponds to an online calibration ID.


The Quest control layer also supplies explicit CRM session, legacy link,
activation, two-link synchronization, scheduling and flush calls. It uses Quest
opcodes and retains positive returned handles after failed copy-back; no retry,
node opening or automatic cleanup occurs. A failed call can still have changed
kernel state, even when no handle reaches userspace. The owner must establish
quiescence before reuse. Flush alone does not establish DMA quiescence.

The native request test uses the pinned vendor structures to inspect every
payload and reserved field. It exercises 282 ioctls across 311 cases, including
all supported link counts, both master links, scheduling flags, maximum request
IDs, partial handle returns and interrupted operations. This is transport ABI
coverage; sensor acquisition, ISP resources, buffer allocation, hardware clock
mapping and actual capture remain separate integration requirements.


`quest_monado_hmd_create` exposes the Quest tracking runtime through Monado's
headset callbacks. It borrows the runtime and copies explicit stereo display,
FOV, eye-pose and RGB distortion grids. It never opens or starts sensors and
rejects Pico's vendor-specific polynomial optics. Pico and Quest share the
calibrated optics implementation; their tracking ownership remains separate.
The Quest owner must stop capture and destroy the HMD before freeing tracking.
There is no automatic Quest prober or inferred display calibration.

Host and native ARM64 ASan/UBSan checks exercise copied optics, invalid input,
tracking loss and repeated lifetime transitions. The installed-library replay
also checks head poses, stereo views, bounded prediction and current-input
activity against the actual Quest provider while Basalt estimates the reference
trajectory. Reference data and synthetic display optics validate this software
connection; they do not establish physical Quest tracking or optical accuracy.


Quest also has `CAM_SET_STREAM_MODE` and `CAM_STREAM_MODE_CMD`, absent from
Pico's older common-opcode range. `quest-camera-buffer.c` prepares explicit
single-plane buffer sets and separate zero-timeout GET and one-buffer RETURN
commands. It checks bounded payloads, unique owned handles, packet offsets,
response counts and timestamp ranges. It preserves caller output on failure;
a failed copy-back may still have transferred ownership in the kernel and
requires stopping the owner. It does not prepare ISP packets, configure sensors,
map DMA-BUFs, perform CPU-cache synchronization or start capture.

In the pinned Quest source, ordinary successful requests skip SOF notification
in `__cam_isp_ctx_send_sof_timestamp`. The stream path saves
`stream_image->capture_timestamp` at buffer completion, but GET fills
`sof_timestamp` from `ctx_isp->monotonic_timestamp` at dequeue. A queued buffer
can therefore contain timestamps from different frames. The wrapper exposes
the latter as `observed_monotonic_ns`; it must not be used to infer a same-frame
clock mapping. Captured Qtimer SOF still needs measured alignment and exposure
center timing before VIO input.

The native vendor-header test checks 266 buffer ioctls, all 1-30 buffer counts,
zero-wait and separate return payloads, malformed responses, partial failure
output, and invalid handles/offsets under ASan/UBSan. Kernel timestamp ownership,
ISP resources and real capture remain unverified by these transport tests.


The Quest device transport now supplies explicit sensor probe/acquire, PHY
acquire, split ISP device acquire, packet configure, start/stop and release.
Probe uses Quest opcode `0x10e`; PHY acquisition represents the Quest CPHY flag
separately from same-type combo mode and excludes mixed/remote modes. These calls
can power hardware and are not inventory operations. They preserve positive
handles even after errors and never retry. The separate ISP hardware-resource wrapper is described below. Verified sensor
mode packets and an exclusive capture owner are still required.

`quest-camera-memory.c` uses Quest's CRM allocation/import/release opcodes
`0x116`–`0x118`. It requests cached nonsecure memory: this kernel uses DMA heaps
and its allocation implementation rejects uncached requests unless platform
policy forces caching. Packet buffers have kernel and user access; command
buffers may additionally map into explicit query-derived MMUs. Pixel import
requires a fresh caller-owned cached DMA-BUF and checks the returned mapped size
against its verified allocation. Failed allocation/import retains any returned
handle or descriptor. Failed release must not be retried because kernel state
may already have changed. Mapping, cache synchronization and DMA quiescence
remain the enclosing owner's responsibility.

Native ARM64 ASan/UBSan tests against the pinned vendor headers pass 318 device
and 6,558 memory ioctls, including all 1–16 MMU counts, descriptor boundaries,
partial copy-back, malformed handles and mapped extents, interrupted calls and
positive ioctl returns. The preceding query, CRM and buffer tests also pass.
These tests perform no device I/O and do not establish physical acquisition or
memory coherency.


`quest-camera-isp.c` encodes the Quest parser's 216-byte v2 acquire resource for
one uncropped RAW8/RAW10 input and one RDI0–3 output. The pinned IFE parser still
supports this version; no SFE, virtual, dual-ISP or cropped path is implied.
It represents PHY 6 with its noncontiguous resource identifier. The separate
24-byte PHY configuration differs from Pico's 32-byte payload: CPHY/combo mode
is selected during acquisition, and the driver derives lane-enable bits from
lane assignment. The encoder checks distinct lanes, mode-specific lane counts,
unused lane nibbles and supported MIPI calibration flags. The settle field
preserves the driver's division by 200,000,000 without assuming a time unit.

The hardware-acquire wrapper uses the split v2 ioctl and retains the full
five-resource/three-path response. Failure leaves that response unchanged but
may already have acquired hardware. Hardware release is explicit; neither call
establishes DMA quiescence. Actual sensor mode, lane wiring, bandwidth/clock
votes and exposure timing must be supplied from verified device configuration.

Native vendor-header sanitizer tests pass 1,049,227 geometry/PHY cases, including
all 65,536 lane maps in each supported mode/count combination, plus twelve
hardware acquire/release calls with injected errors. Payloads compare byte for
byte with the pinned vendor structures, including reserved fields and unaligned
output. The exact IFE parser version and bounds were inspected separately.
This proves encoding and transport compatibility, not capture or hardware timing.

### Camera buffer timestamp ownership

The Quest kernel patch `0007-quest-camera-buffer-timestamp.patch` stores the
monotonic timestamp in the same private stream-image record as its capture
QTimer. Dequeue returns that saved pair, including after several completed
frames queue before userspace reads them. The userspace ABI is unchanged.

The regression harness extracts and executes the pinned vendor completion, GET
and RETURN functions with modeled lists, completion waits and clocks. Across
1,000 two-buffer/recycle sequences, 35,000 checks detect 3,000 mismatched pairs
in the original code and none with the patch. Host and native ARM64 runs pass
with ASan/UBSan. The physical-target kernel rebuild also passes verification
of all 272 modules and 15,289 versioned symbol imports.

This corrects dequeue-induced frame mixing. It does not establish exposure
center, capture-to-IMU clock calibration, sensor synchronization or physical
DMA quiescence. Unpatched stock firmware still needs the conservative
`observed_monotonic_ns` interpretation described above.

### Installed camera transport

The Quest tracking DSO includes the camera control, buffer, memory and ISP
encoders described above, with explicit public exports. Combined Pico/Quest
and Quest-only ARM64 builds pass the same 25 Monado tests and produce identical
Quest libraries. The Quest-only package has no SSC dependency.

The vendor-header camera tests also pass against that installed library, covering
queries, request management, acquisition, memory, geometry and buffer transport.
The installed runtime still passes the Basalt reference replay and invalidates
tracking on camera silence and clock regression. These are offline ABI and
reference-data checks; the library does not yet own a complete physical Quest
camera pipeline.

### Camera capture ownership

`quest-camera-capture.c` joins the nonblocking ISP buffer transport, embedded-row
decoder, shutter matcher and mono8 conversion for one explicitly configured
stream. It holds one image while waiting for a shutter record, requires unique
buffer IDs and nonoverlapping mappings, and brackets pixel reads with the owner's
DMA-BUF CPU synchronization. It delivers a copied image before returning the
buffer to the ISP. Frame numbers and timestamps must advance; silence, ambiguous
shutters, failed cache operations and uncertain GET/RETURN completion invalidate
the tracking generation. Failed operations are never retried implicitly.

Clock-window and exposure-center callbacks remain mandatory: neither the metadata
row nor kernel SOF establishes the shutter clock relationship. The owner borrows
an exclusively configured ISP and its allocations; it does not acquire hardware
or start sensors. After startup, disposal requires independent confirmation that
capture DMA and any uncertain CPU cache access have ended. The sanitizer tests
exercise both shutter/image arrival orders and failed operations; they do not
prove physical capture or recovery.

### Explicit capture geometry

The calibration converter accepts `--geometry` for a caller-supplied uniform
downscale followed by a centered crop. The document has `schema_version: 1` and
a `cameras` array containing exactly the selected IDs. Each entry specifies
`id`, `scaled_size`, `crop_size`, and `pixel_center_origin` (0.5 or 1.0). Sizes
refer to visible pixels after removing the embedded metadata row. Nonuniform
scaling, upscaling, missing origin rules and out-of-bounds crops are rejected.

The retained Quest camera-model implementation scales the focal length and uses
`(principal + origin) * scale - origin` before subtracting the centered crop
offset. Its two rescale modes use different origins; the active mode must be
established independently. Factory camera dimensions alone cannot select it.
A 640×513 transport buffer with a leading metadata row therefore describes a
640×512 visible image, not a 640×513 calibration. Distortion coefficients and
camera/IMU extrinsics are unchanged by this conversion. The output records the
geometry input and hash but keeps active geometry and physical tracking unverified.

Offline checks compare both rules and centered cropping with the original ARM64
rescale/crop functions. The Basalt parser test also projects the original rays
through the requested pixel transform and checks the converted camera model.

### Copied shutter FIFO

`quest-camera-stream.c` opens a separate copied Syncboss FIFO with the packet-254
filter and per-client queue clearing. It routes supported tracking shutters to
one through four explicitly selected cameras. Each complete batch is decoded
and checked before any callback: malformed tails, duplicate/regressing raw
timestamps and excessive gaps invalidate the generation. Per-camera silence
deadlines prevent traffic from one sensor from keeping a stalled stereo partner
alive. Reads and callback delivery are bounded and checked against monotonic time.

`quest_camera_shutter_decode_fifo` retains the raw payload and signed NSYNC
value while mapping both timestamp and SOF to CLOCK_MONOTONIC. This requires a
valid local NSYNC status, checks signed addition/multiplication, and ignores the
version-3 remote offset for this local HMD. The receiver rejects future or stale
mapped times and per-camera regressions after an offset change. Raw and mapped
times remain separate; this does not identify exposure center.

The receiver closes its FIFO once on failure, including MCU driver messages,
read errors, clock regression and consumer refusal. It does not start cameras
or configure the MCU. Observation time is not exposure time: the capture owner
still requires a verified shutter-to-image clock window and exposure-center
conversion. Restart/suspend detection and camera DMA quiescence remain external.
Sanitizer tests use modeled transport and the pinned filter ABI; they do not
establish live camera delivery.

The installed reference replay also routes 370 images through this FIFO receiver
and the capture owner before Basalt. It decodes a synthetic embedded metadata
row, alternates shutter/image arrival order, checks copied mono8 pixels, returns
each buffer once and poisons it immediately after return. Both camera-silence
and clock-regression scenarios still invalidate the HMD pose provider.
The FIFO, ISP dequeue, CPU synchronization, row contents and exposure-center
relationship are explicit test models; image/IMU inputs and calibration remain
the Valve Index reference dataset. This closes the offline component connection
without claiming native Quest camera startup or hardware timing validation.

### Camera start and stop ownership

`quest-camera-pipeline` owns one start/stop attempt for one through four explicitly
selected local camera triples. Descriptors, handles, link membership and prepared
configuration are borrowed. Retained CamX lifecycle tables establish the order:
activate CRM links, then start IFE, PHY and sensor; stop IFE, sensor and PHY before
deactivating links. MCU commands and hardware synchronization remain separate.

Every attempted start is recorded before the ioctl, so cleanup includes a device
whose failed call may already have changed hardware. Stop visits all attempted
resources even after an error and never retries uncertain operations. Successful
stop ioctls do not establish quiescence: normal destruction also requires the
caller to confirm that DMA, workers and control calls have ended. Failed stop
retains the owner until independent quiescence and closure of every borrowed
descriptor. This component does not open nodes, configure sensors or prove
physical capture. Tests cover 600 cycles and every start/stop failure site for
one through four cameras.
