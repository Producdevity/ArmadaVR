# Pico head IMU transport

The Neo3 b1977 tracking service reads Android hardware-buffer direct channels.
This is a different path from the `ble_mcu` controller stream. The inspected
`libpvrtrackingservice.so` has SHA-256
`4b7f74b80607a2863559ab6496f4a8abe6778ccaec834074a923a2064cfe0a9a`.
Its `android::threadReader` requests uncalibrated accelerometer type 35 and
gyroscope type 16, using a BLOB hardware buffer and the fastest supported
report level. It forwards the vectors and the fourth float into the tracking
algorithm. That fourth float has vendor-specific behavior; it must not be
assumed to be a standard bias or temperature value.

The stock reader pairs accelerometer and gyroscope records with identical
capture timestamps. Each direct-channel record is 104 bytes, with sequence at
byte 12, timestamp at byte 16 and six floats starting at byte 24. The timestamp
is expressed in nanoseconds. On the tested stock unit, it aligns with the ARM
hardware counter's origin, not Android boot time. This observation does not
establish the controller DSP stream's clock origin or a native Linux driver.

## Reproducible stock diagnostic

With the headset running the exact supported stock firmware and no existing
direct sensor connections, run:

```sh
python3 -B tools/pico-imu.py capture \
  --ndk /path/to/android/ndk/27.1.12297006 \
  --adb /path/to/adb --port 5037 --serial DEVICE_SERIAL \
  --output output/pico-imu-capture
```

The tool requires a new output directory. It checks the device identity,
firmware and tracking-library hash, builds an ARM64 Android diagnostic, verifies
the uploaded binary, records two seconds of samples and then removes its own
temporary binary. The diagnostic uses the shell's sensor permissions and owns
only its own direct channel. It does not change firmware, calibration, pairing
or boot configuration. A hard process deadline bounds stalled Binder calls.
The receipt verifies the boot identity and absence of remaining direct channels.

A missing default gyroscope is not proof that no gyroscope exists: on the tested
unit, enumeration exposes a unique non-wakeup type-16 sensor while the default
sensor lookup returns null. The diagnostic allows that unique fallback and
rejects ambiguous matches.

Counter reads are bracketed by both CLOCK_MONOTONIC and CLOCK_BOOTTIME before
and after sampling. Offline analysis rejects missing records, reordered sensor
timestamps, changed counter frequency, discontinuous clock mappings, samples
outside the measured interval and incomplete cleanup. It preserves all six raw
floats, maps capture time explicitly into CLOCK_MONOTONIC, and reports the
measurement uncertainty. It applies no calibration and produces no pose.

An existing log can be analyzed without contacting a headset:

```sh
python3 -B tools/pico-imu.py analyze output/pico-imu-capture/capture.log \
  --output output/pico-imu-analysis.json
```

Keep capture receipts and unit calibration private. A successful stock capture
proves access through Android's current HAL. Native SSC/FastRPC delivery,
coordinate transforms, camera synchronization, optical calibration and physical
tracking under the custom kernel still require separate verification.

## Saved calibration inputs

The preserved stock direct-channel capture identifies both head sensors as
`icm4x6xx`. The unit's saved SSC registry contains factory bias and correction
matrix records for that family, alongside separate `lsm6dst` records. Inspection
must select the family explicitly; the presence of a registry entry does not
identify the live native SUID.

The offline inspector preserves coefficients, per-field versions and source
hashes without applying them:

```sh
python3 -B tools/inspect-pico-calibration.py /path/to/saved/registry \
  --sensor icm4x6xx_0 --output output/pico-calibration.json
```

It rejects duplicate JSON keys, mismatched group identities, incorrect types,
non-finite values, singular correction matrices and invalid signed axis
permutations. Inputs are bounded regular files; output is a new private file.
The orientation strings and twelve placement fields remain uninterpreted.
An all-zero placement is reported as such, with no identity-matrix fallback.

On the preserved unit, all twelve placement fields are zero. The separate
`calibration/Bosh/IMUParams.txt` contains identity matrices and zero offsets;
the camera XML carries a `not_configured` device label. These files do not
establish active headset calibration. Before using any registry correction,
verify the native sensor association, units, transform direction, correction
order and whether the DSP already applies factory or temperature compensation.
Applying the same correction twice would corrupt otherwise valid samples.
Camera extrinsics and camera/IMU timing remain separate requirements.

Inspection of the six selected registry files and preservation checks for all
337 extracted files are recorded under `output/pico-calibration-20261012-v1/`.
Unit coefficients and backups remain outside version control.

## Native sensor inventory

The Android names do not establish native SSC sensor IDs. The metadata collector
queries the registry and then every SUID for one explicit data type, with default
selection and subscription updates disabled. It requests attributes from each
candidate without opening sensor streams or applying calibration. Supported
queries are `accel`, `gyro`, `accel_cal`, `gyro_cal`, `ble_mcu` and `ndi_fpga`.

Build in the existing offline ARM64 SSC compiler environment:

```sh
python3 -B tools/build-ssc-inventory.py --ssc-bundle VERIFIED_SSC_BUNDLE \
  --output NEW_INVENTORY_BUILD
```

The builder verifies the current SSC bundle, extracts checksum-pinned source,
applies the maintained patches and compiles a separate collector using the same
QMI/QRTR dependencies. It does not change the SSC runtime or boot image.

Package a verified build for the offline root with:

```sh
python3 -B tools/ssc-inventory-artifact.py NEW_INVENTORY_BUILD \
  --ssc-bundle VERIFIED_SSC_BUNDLE --output NEW_INVENTORY_PACKAGE
```

Pass that package to `build-headset-root.py --ssc-inventory`, together with
its exact `--ssc` dependency. Installation checks the collector, source and
build record, verifies the installed SSC dependency, and checks the root's
loader and image readback. It adds no service or automatic sensor query.
The installed command is `/opt/reframed-vr/ssc-inventory/bin/ssc-inventory`;
its launcher supplies the packaged dependency search path explicitly.

After a separately authorized and recoverable native Linux boot, with the Pico
QRTR services explicitly enabled, `ssc-inventory-release --collect accel` emits
one JSON document. The existing SSC dependency directory must be in the loader
search path. Each sensor retains both 64-bit SUID halves and numbered typed
attributes. Fixed64 attribute values use decimal strings to preserve precision;
rates and nested arrays remain intact. An empty list means that the requested
type was not advertised, not that the hardware is absent.

The collector requires a Kona device tree and the exact `reframed.pico=qrtr`
token, or the separately guarded virtual test environment. It refuses duplicate
or reserved SUIDs, malformed attributes and non-finite values. Each request has
a three-second deadline; the complete process has a sixty-second deadline.
It accepts a reply before its QMI acknowledgement but requires both before
continuing. An incomplete query exits with an error and emits no inventory.
It does not start a DSP, mount storage, select a sensor for tracking or configure
the clock, rate, axes or bias. Save inventories with the private unit evidence.

The mock DSP tests require all requests to be discovery or attribute queries;
any stream command fails the test. Native sanitizer and VM results validate
the collector and protocol handling only. Actual native SUIDs, attributes and
their association with the head IMU remain unverified until physical capture.

## Native record processing

`src/pico-imu.c` accepts stable copied direct-channel records with explicitly
supplied tokens and a counter-to-monotonic calibration. Its bounded queues pair
only identical capture timestamps. Real stock captures contain short batches
of one sensor followed by the other; a single pending sample is insufficient.
The implementation retains up to sixteen records per sensor and accounts for
unpaired records when timestamps differ, sequence gaps occur or a queue fills.
It rejects malformed payloads, stale/future samples and reordered records while
preserving state and caller outputs on rejection. Counter wrap skips zero.

The processor is transport-independent. It does not open a sensor, select
calibration, convert axes, subtract the vendor auxiliary values or estimate pose.
Callers must reacquire the clock and clear stream state after suspend, reconnect
or reset. The clock uncertainty must include hardware-counter quantization.

The October 12 stock capture contains 1,946 accelerometer records and 1,945
gyroscope records, with 1,945 exact timestamp pairs at approximately 1,024 Hz.
Replay through the C processor preserves every pair, vector and auxiliary value
and verifies the integer clock mapping. The same replay and rejection tests
pass AddressSanitizer and UndefinedBehaviorSanitizer on macOS and ARM64 Linux.
This verifies decoding of observed stock data, not native sensor delivery under
the Reframed kernel.

## Low-latency RPC messages

The inspected stock `libsns_low_lat_stream_stub.so` has SHA-256
`893aefcbcb74320fcce5660e2d4c2d88cf403839225638a6f1681636312f700f`.
`src/sns-low-latency.c` marshals its four channel operations through a synchronous
caller-supplied transport. It does not load the stock binary or open a DSP.

| Operation | FastRPC scalar | Input bytes | Output bytes |
| --- | --- | --- | --- |
| Initialize | `0x00010100` | 12 | 4 |
| Configure | `0x01010000` | 48 | 0 |
| Stop | `0x02010000` | 4 | 0 |
| Deinitialize | `0x03010000` | 4 | 0 |

Initialization carries the descriptor, size and the zero third argument used by
the stock caller. Configuration carries the channel at byte 0, signed timestamp
offset at byte 8, SUID at bytes 16–31, period in microseconds at byte 32, vendor
flags at byte 36 and sensor handle at byte 40. The source encoder zeroes padding
bytes 4–7 and 44–47; the stock stub leaves those bytes uninitialized. Flags and
SUID selection remain explicit; their appropriate values for native head
tracking have not been established.

The differential test executes the exact stock ARM64 wrapper instructions in
Unicorn 2.1.4, mocking handle acquisition and the transport boundary. It compares
768 calls against the source-built encoder, including signed offset extremes,
positive and negative remote errors, two initial stack patterns and unchanged
output on failed initialization. No Android dependency, DSP or device is opened.
The separate native tests exercise malformed arguments and 1,000 failure cycles
under address and undefined-behavior sanitizers.

After building `tests/sns-low-latency.c` with `src/sns-low-latency.c`, the optional
stock comparison is reproducible with:

```sh
python3 -B tests/sns-low-latency-stock.py \
  --stub /path/to/private/libsns_low_lat_stream_stub.so \
  --test-binary /path/to/sns-low-latency-test \
  --output output/sns-low-latency-stock
```

Unicorn 2.1.4 must be available in that Python environment. Keep the stock binary
private. On macOS the emulator needs permission to allocate executable memory.

The current [Qualcomm FastRPC userspace source](https://github.com/qualcomm/fastrpc/tree/a56e9d4de3614a8d8f4b0e15ff561350c4b686e3)
uses the mainline `/dev/fastrpc-sdsp` interface and DMA-heap allocation. Pico's
published vendor kernel instead exposes `/dev/adsprpc-smd` and the vendor ioctl
layout: for example, invocation is request 1 with a pointer to `remote_arg`,
where the current mainline interface uses request 3 with different argument
records. The current userspace library cannot be installed unchanged against
that driver. A matching vendor backend, shared-buffer allocation, SUID discovery,
DSP lifecycle and physical output validation remain necessary. RPC message
compatibility alone does not resolve those requirements.

The kernel patch `0033-fastrpc-init-channel.patch` rejects `INIT` and
`INIT_ATTRS` before domain selection. The original file starts with channel
`-1` and reads the channel's security policy before the later transport check.
The guard returns `EINVAL` before that array access. Valid domain policy and
transport errors retain their existing behavior.

`tools/test-pico-fastrpc.py` checks its initialization fixture against the
checksum-pinned vendor source, reproduces both array-boundary failures with
ASan, and checks the patched path with ASan/UBSan. Its transport boundary is
stubbed; these checks do not establish DSP communication or physical boot.

The rebuilt physical configuration also passes 512 real `INIT`/`INIT_ATTRS`
calls in diskless QEMU using `tests/pico-fastrpc-ioctl.c`: both device nodes,
32-bit and 64-bit callers, all four initialization flags and signed/unsigned
attributes. Kernel taint is unchanged. The previous kernel fails the same
test with `ECONNREFUSED` before selecting a domain. No DSP is attached by
this test, and no headset is accessed.

The stock low-latency wrapper allocates through `rpcmem_alloc(-1, 1, size)`.
Its allocator uses the ION v2 allocation ioctl and registers the resulting
DMA-BUF with FastRPC. It tries ADSP heap 22 when no SMMU is reported, then
system heap 25; heap selection and DSP mapping cannot be inferred from a
successful CPU mapping.

`src/ion-buffer.c` provides the ION v2 allocation, CPU synchronization and
release lifecycle for the Pico's 4 KiB pages. The caller supplies the ION
file descriptor and a verified heap ID. Allocation preserves output on
failure and requires a CLOEXEC descriptor. CPU access is bracketed explicitly;
release requires prior DSP detachment and never retries `close` after `EINTR`.
Fault tests cover allocation/mapping failures, ownership preservation and
all CPU access modes. The real kernel test uses a virtual system heap and
checks both ABIs, cached/uncached memory, zeroed allocation, alias mappings
and cleanup. Physical cache coherency and sensor-DSP mapping remain unverified.

`src/pico-fastrpc.c` implements domain selection, sensor attachment and the
vendor invoke variants. It packs pointer-sized buffer records plus separate
FD/attribute arrays, checks scalar argument counts and rejects truncating
addresses or DMA offsets. The vendor NOVA bit is `1`, unlike current mainline
userspace's `256`. CRC calls retain the vendor's 64-entry output array.
Descriptors remain caller-owned; a failed GETINFO requires reopening because
the kernel may retain the selected domain even when session allocation fails.
Neither attachment errors nor interrupted invocations are treated as success.

The adapter tests compare ioctl bytes and compile layout assertions against
the pinned kernel header for both ABIs. The diskless VM checks rejection before
domain selection and preserves the normal/secure node policy differences; it
also runs the packing tests on both ABIs. Successful DSP attachment, reverse-RPC
listener integration and sensor delivery still require the native runtime path.

The adapter also exposes vendor mode control and 64-bit map/unmap requests
for listener allocations. It preserves the returned DSP address on failure
and retains positive driver error results, including the vendor's `EBADR`
response to mapping before initialization. Mapping success, cache behavior
and DSP teardown are not established by the rejection tests.

The [native FastRPC bundle](pico-fastrpc.md) reuses Qualcomm's listener and
buffer registry with this adapter and the ION allocator. Its isolated runtime
test covers allocation/registration/release through the complete library;
the physical DSP and tracking stream remain unverified.

The b1977 tracking library's `SlpiDataReader::start` calls this low-latency
wrapper for `ble_mcu`, requesting period `100000` microseconds, flags `0` and
sensor handle `1`. Its reader uses 100-byte controller slots, checks CRC-8 and
orders them by the nonzero 64-bit field at byte 56, which stock calls station
time. These observations describe a controller path. They do not establish a
native head-IMU transport, the controller's actual reporting rate or that
field's clock origin. The separate head reader still uses Android's 104-byte
direct-channel records described above. Do not apply its layout or clock
calibration to the controller ring.

## Controller IMU records

The stock station parser also carries two raw controller IMU slots. The native
`pico_controller_decode_imu` decoder preserves their signed gyroscope,
accelerometer and temperature counts, sequence bytes, microsecond timestamps
and separate station time. `pico_controller_decode_imu_ssc` accepts the existing
SSC message 1024 payload and returns its accuracy separately. Both reject
malformed records without changing caller outputs.

Slot selection depends on explicit MCU/radio types and the connected-hand mask.
MCU type 1 identifies the hand from the packet and can carry two samples for
that hand. MCU type 2 assigns the first slot to right and the second to left.
Radio type and connected state also control which slots stock accepts; the
implementation does not assume both slots are always present. Zero sample
timestamps are absent, and valid packets can have no samples.

The saved stock ARM64 `imuDataAnalyze` instructions were executed in an isolated
container against 384 synthetic configurations, yielding 288 samples. Native
macOS and ARM64 decoder outputs agree with the captured stock fields and vector
conversion inputs. CRC corruption, truncation, unaligned records, signed limits,
empty slots and unchanged outputs on rejection pass sanitizer tests. The oracle
replaces the output sink and external logging; hardware reversal is explicitly
bypassed in its synthetic configuration. Evidence is retained privately under
`output/pico-controller-motion-20261014-v1/`.

A separate stock-execution comparison exercises the scale and axis branches
without bypassing them: 10,368 records and 18,144 samples match bit-for-bit,
including signed zero. It covers both MCU/radio types and hands, signed count
limits, scale/bypass bytes 0, 1 and 255, and revision bytes `0` and `1`.
These are synthetic branch inputs, not selected settings for the headset.

Stock first multiplies gyroscope counts by float32 `0.001065264455974102`
and accelerometer counts by float32 `0.004790039267390966`. A scale byte of
exactly 1 doubles each converted value. Its axis operation, applied separately
to each three-vector, is:

| Configuration | Left | Right |
| --- | --- | --- |
| Nonzero axis-reversal bypass byte | `(x, y, z)` | `(x, y, z)` |
| Bypass zero, MCU 1, revision byte ASCII `0` | `(x, y, z)` | `(-x, -y, z)` |
| Bypass zero, all other verified MCU/revision branches | `(-y, -x, -z)` | `(y, x, -z)` |

This reproduces stock's intermediate vectors, not their alignment with Monado's
tracking origin. The [optional status capture](pico-controller-protocol.md#controller-configuration)
records the raw selection bytes per hand. Stock instructions, complete synthetic
inputs, outputs and comparison results are retained privately under
`output/pico-controller-status-20261014-v1/`.

This decoder does not enable controller poses. Physical calibration, captured
hardware-dependent scale/reversal flags, per-hand timestamp mapping and observed
native reports are still required. The stock process multiplies controller
sample timestamps by 1,000 before delivering Android events; this establishes
the wire unit but does not establish an origin shared with the host or head IMU.
Station time must not be substituted for capture time.

## Native SSC IMU reports

The retained b1977 `libssc.so` embeds `sns_accel.proto` and `sns_gyro.proto`,
both referring to `sns_std_sensor_event`. That message uses repeated float
field 1 and required accuracy field 2 (0–3); its message ID is `1025`.
This establishes a standard wire format, not the identity or configuration of
the headset's physical sensors.

[`ssc-imu`](../src/ssc-imu.h) decodes exactly three finite floats and preserves
the reported accuracy. It accepts packed, unpacked and mixed encoding, skips
unknown scalar fields and rejects malformed lengths, groups and oversized
payloads. It retains neither the input buffer nor Android-specific auxiliary
fields. Accuracy zero remains visible and does not imply calibrated input.

The receiver requires two distinct, explicitly discovered SUIDs and a current
SSC clock calibration. It validates message type, subscription generation,
freshness and timestamp order independently for acceleration and gyroscope.
It retains the raw DSP timestamp alongside the mapped host timestamp and its
uncertainty. It does not pair asynchronous samples, infer units or axes, apply
calibration, or produce pose. Disconnect invalidates both streams; reconnect
requires a new generation and clock calibration.

The stock descriptor comparison is reproducible without loading the Android
library or opening a device:

```sh
python3 -B tests/ssc-imu-stock.py \
  --stock-library /path/to/private/b1977/libssc.so \
  --test-binary /path/to/compiled/ssc-imu-test \
  --output output/ssc-imu-comparison
```

Build the test from `tests/ssc-imu.c`, `src/ssc-imu.c` and `src/ssc-clock.c`.
The comparison requires Python's protobuf package and verifies the exact stock
library and descriptor hashes. Its corpus covers 8,227 cases; the native fixture
also exercises 100,000 mutations and independent sensor ordering across 1,000
connection generations. The VM-only `ssc-imu-transport.c` and `ssc-imu-mock.py`
test actual libssc discovery, subscription, timestamped reports and close on a
private GLib context. The DSP endpoint remains simulated. Physical sensor
selection, measured timing, calibration and native headset tracking still need
device validation.

The exact physical-configuration Pico kernel passes three simultaneous
accelerometer/gyroscope open/report/close cycles in diskless QEMU, with invalid
payload, SUID and message-type rejection. The stock-descriptor corpus passes
inside that guest too; kernel taint is unchanged and shutdown is normal.
The fixture needs its established 2 GiB memory map: reducing it to 1 GiB puts
Pico's fixed crash-log reservation at `0xb0000000` outside RAM and panics before
userspace. Both that failed fixture and the corrected run are retained under
`output/pico-ssc-imu-20261012-v1/`.

The [sensor-property patch](../patches/libssc/0007-correct-sensor-property-types.patch)
fixes libssc's metadata property declarations. Stream type, availability and
sample rate were registered as strings while their getters used unsigned
integer, boolean and float values. All three reads produced GLib critical
errors. The corrected properties are read-only and match their getters; they
do not configure a rate or mark a sensor as available.

The source regression reproduces all three original failures and verifies
1,024 typed reads under ARM64 ASan/UBSan. The IMU transport fixture reads the
metadata returned by actual QRTR discovery before opening either sensor.
Applications must still validate the selected SUIDs, stream type and supported
rate before starting physical streams.

## Native IMU session

[`ssc-imu-session`](../src/ssc-imu-session.h) owns accelerometer and gyroscope
discovery, subscriptions and shutdown on a private GLib worker context. Before
opening either stream, it requires the caller's exact SUIDs, requested rates
present in the advertised lists, continuous reporting and availability. It then
requests a fresh clock mapping from the caller. Rate selection does not measure
actual delivery.

Reports reach the caller only after both subscriptions open. Each sensor has
independent timestamp ordering and a deadline for valid reports. Startup failure,
cancellation or either stream going silent invalidates both streams, disables
attempted subscriptions and drains outstanding callbacks before destruction.
Failed cleanup remains visible in the status. There is no automatic restart;
the owner must establish a new session after suspend or DSP restart.

Callbacks run on the worker, and their data must remain valid until destruction
succeeds. A callback may request cancellation; destroying its own worker is
refused so the owner can join it later. Events are borrowed for the callback's
duration. This layer preserves raw sensor values and accuracy; it does not infer
physical identity, calibration, coordinate transforms or pose.

`tools/test-ssc-imu-session.py` builds release, ASan/UBSan and UBSan-only fixtures
against the verified SSC package in an offline ARM64 Linux environment. Native
sanitizer tests cover 100 alternating immediate cancellations and failures with
QRTR absent. `tests/ssc-imu-session-vm.py` exercises two simultaneous streams,
identity/rate mismatch, unavailable or malformed gyro data, partial startup,
silent gyro, service loss, clock failure and cancellation from a report callback.
These VM scenarios use real QRTR and libssc with a synthetic DSP endpoint.
All twelve cases pass on the physical-configuration kernel in release and
UBSan-only builds. This kernel's 39-bit virtual address space cannot initialize
the available ASan runtime; ASan coverage is limited to the native tests above.
Logs, including the failed ASan VM startup, are retained under
`output/pico-imu-session-20261012-v1/`.

## Runtime calibration events

The b1977 vendor sensor HAL (`sensors.ssc.so`, SHA-256
`bf3acf3704b06b4218c8b24cd72744646b63b94bd74f0cc7a7b8bfd2ddb8038e`)
subscribes to separate `accel_cal` and `gyro_cal` sensors using on-change
configuration 514. Their `sns_cal_event` reports use message 1022, with bias
field 1, optional scale field 2, optional compensation matrix field 3, required
accuracy field 4 and optional fixed32 calibration ID field 5.

In the inspected regular-event path, both calibrated acceleration and gyro
subtract a timestamp-selected three-axis bias from the corresponding SSC
sample. Uncalibrated events retain the SSC values and expose the bias separately.
The calibration callbacks retain bias, timestamp and accuracy; they do not apply
the saved factory correction matrices again. This identifies host-side behavior;
it does not establish which factory corrections the DSP already performs or the
separate FastRPC direct-channel implementation.

The HAL pairs calibration sensors by their `rigid_body` attribute, ID 19.
A matching datatype alone is insufficient. Physical SUIDs, pairing and the
native sensor clock still require validation on this unit.

[`ssc-calibration`](../src/ssc-calibration.h) decodes these reports and keeps
32 timestamped updates per sensor. It requires explicit paired calibration SUIDs,
the IMU generation and clock, and caller-selected age limits. Correction uses
the newest update at or before the sample. Missing, future, expired or unreliable
biases produce no corrected result. An unreliable update cannot silently fall
back to an older reliable one. Reconnects clear the history; late samples whose
calibration has been evicted are refused. The raw sample and calibration
metadata remain available alongside the bias-subtracted result.

Scale and matrix fields are preserved without application; no axes, units or
factory-calibration policy are inferred. This component is not automatically
started or installed into the runtime. Physical calibration pairing must be
established before enabling correction.

The optional `tests/ssc-calibration-stock.py` comparison extracts checksum-pinned
descriptors from a private b1977 `libssc.so`. It checks 8,475 cases against
Google protobuf, including packed/unpacked fields, optional metadata, invalid
dimensions and nonfinite values. The native fixture exercises 100,000 payload
mutations and 1,000 generations of timestamp selection, expiry, eviction and
invalidation under ASan/UBSan. These tests do not establish physical calibration
or a tracked pose. Private HAL exports and test receipts are retained under
`output/pico-native-sensors-20261013-v1/`.

The [calibration-startup patch](../patches/libssc/0008-complete-calibration-sensor-startup.patch)
lets matching message 1022 complete an `accel_cal` or `gyro_cal` subscription.
Previously these opens stayed pending even after the transport acknowledged
configuration and calibration reports arrived. A source regression reproduces
256 pending opens and verifies 384 corrected startup/cancellation cases,
including reports before acknowledgement, late transport failure, wrong SUIDs
and unrelated datatypes. Report payload validation remains the receiver's job.

The rebuilt library passes ten QRTR VM suites on the unchanged physical Pico
kernel, including existing controller failures, simultaneous raw IMU streams
and three two-sensor calibration open/report/close cycles. The latter exercise
the source decoder and explicit bias subtraction with UBSan, malformed-report
rejection and fresh generations. The 8,475-case calibration corpus also passes
inside the guest; kernel taint is unchanged and shutdown is normal. The DSP and
IMU sample inputs are synthetic. This bundle has not replaced the previously
verified offline root or established physical sensor delivery.

The [rigid-body patch](../patches/libssc/0009-expose-rigid-body-identity.patch)
exposes attribute 19 as the read-only integer property `rigid-body`: display
`0`, keyboard `1`, external `2`, or unknown `-1`. Missing, duplicate, malformed
or unsupported values remain unknown; absence is never treated as display.
The source regression checks actual protobuf attribute parsing and public
property reads across 128 cycles, including wrong-SUID reports and replacement
of previously valid metadata by invalid metadata. A physical pairing must check
this property and the expected SUIDs before enabling bias correction.

Setting `ssc_imu_session_config.bias.report` explicitly enables the paired
calibration path. Supply two calibration SUIDs, each pair's verified rigid body,
and a maximum bias age. The session requires four distinct SUIDs, checks metadata
for all four sensors before enabling any, and owns their discovery, reports,
cancellation and teardown on the same private worker. Default sessions continue
to expose raw IMU events only.

Calibration updates are retained during startup: the first on-change report
may also complete its subscription, and there may be no second update. Raw
callbacks wait for all four subscriptions to open. Corrected callbacks additionally
require a reliable, timestamp-appropriate bias. If either corrected stream stops
for the configured report timeout, the session invalidates both IMUs and disables
all attempted subscriptions. An expired bias cannot be kept alive by malformed
calibration traffic or continuing raw samples. Raw callbacks may cancel without
receiving a subsequent corrected callback. Corrected callbacks may also request
stop; destruction must still be joined from the owning thread.

The paired session passes 26 QRTR VM scenarios with UBSan, retaining the
original two-sensor cases and adding four-sensor startup, one-shot calibration,
wrong SUID/body, missing metadata, malformed/unreliable/expired bias, partial
startup, service loss and callback cancellation. Native ASan/UBSan with leak
detection covers 100 cancellation/no-QRTR cycles split between two- and
four-subscription configurations. These checks exercise the production session
owner with a synthetic DSP; actual calibration SUIDs, pairing and native physical
delivery remain unverified. The HMD integration below now uses this session,
with explicit device calibration and activation required.

The [sensor-selection patch](../patches/libssc/0010-select-requested-sensor-uid.patch)
adds construct-only uint64 properties `requested-uid-high` and `requested-uid-low`.
A nonzero 128-bit request discovers all sensors of that datatype and requires
exactly one matching SUID before requesting attributes. Missing or duplicate
matches fail discovery. Both halves zero retain libssc's default-sensor behavior.
The IMU session supplies its expected identities through these properties and
refuses older libraries without them. Selection does not infer physical identity
from datatype or list order.

The prior implementation requested only the default sensor, making a verified
non-default IMU or calibration sensor inaccessible. The source regression
reproduces that limitation in 128 cases and checks 1,024 corrected cases under
ASan/UBSan, including selection among 32 candidates, wrong datatype, empty lists,
duplicates and IDs with one zero half. QRTR session tests additionally place
other candidates before and after each requested sensor and verify that missing
or ambiguous selection sends no sensor-enable requests. All 32 session scenarios
pass with UBSan on the unchanged physical-configuration kernel; native ASan/UBSan
cancellation checks and the 429 host tests also pass. These remain virtual
transport and lifecycle results, not physical tracking acceptance.

A bounded diagnostic on the exact stock 5.8.4.0 firmware found that the Android
shell cannot create either AF_QIPCRTR (42) or legacy Qualcomm AF_MSM_IPC (27)
sockets: both return `EACCES`. It sent no messages, started no sensors, removed
its temporary binary and verified the unchanged boot identity. This rules out
direct SSC discovery from that shell context; it does not establish whether
either transport works under the custom kernel. The receipt is retained under
`output/pico-native-sensors-20261013-v1/socket-probe-v1/`.

The [rate-selection patch](../patches/libssc/0011-select-supported-sample-rate.patch)
adds the construct-only float property `requested-sample-rate`. A positive request
must exactly match an advertised rate. Missing, duplicate or malformed rate
attributes leave the selected rate unavailable; stale metadata cannot preserve
an earlier match. A zero request retains first-advertised-rate selection for
existing libssc callers. The IMU session requests its two configured rates and
refuses libraries lacking this capability. Calibration subscriptions remain
on-change and request no sample rate.

The source regression reproduces first-rate-only selection in 128 cases, then
checks 2,048 selection, metadata replacement and refusal cases under ASan/UBSan.
It decodes actual outgoing configuration messages to verify the selected rate
and requires no send when selection fails. This establishes request behavior;
it does not establish physical sampling frequency or timing accuracy.

All 37 QRTR session scenarios pass with UBSan, including requests for 1,000 Hz
from an advertised list of 5, 1,000 and 2,000 Hz in both raw and calibrated
sessions. The mock checks every outgoing continuous-rate request and confirms
that malformed or missing rate metadata prevents enabling any subscription.
Native ASan/UBSan cancellation tests and the 429 host checks also pass.

The eleven-patch SSC library is installed in the separately verified October 13
[Pico root](headset-root.md). Monado's existing controller integration passes
its OpenXR regression against that library. The head session described below
integrates calibrated IMU data with the HMD. Physical identities, clock origin,
coordinate transforms and camera tracking remain to be established.


## Calibrated head orientation

`pico-head-session` connects the four-subscription SSC session to Monado's
`m_imu_3dof` orientation filter through `pico-head-tracking`. The caller must
supply verified raw and calibration SUIDs, display rigid-body identities,
advertised rates, a CLOCK_MONOTONIC clock calibration, a proper sensor-to-head
rotation and positive unit scales. Acceleration is converted to m/s² and angular
velocity to rad/s. No device configuration or axis convention is inferred.

The provider pairs equal capture ticks and timestamps in bounded queues. It
rejects unreliable, nonfinite, future, stale, reordered or foreign-generation
samples. A gap beyond the configured limit invalidates the stream until a new
generation starts. Reconnection clears Monado's FIFO history as well as its
orientation state. Reads and sample updates are serialized; stopping the session
immediately clears exposed pose validity while its worker disables subscriptions.

The capture-time API returns orientation and its timestamp. The timestamp-query
API retains 4,096 poses and interpolates or predicts orientation using calibrated
angular velocity in tracking-origin coordinates. Forward prediction is limited
to 50 ms including timestamp uncertainty. Invalid, stale and older-than-history
queries clear the relation. Position, a calibrated yaw origin and camera tracking
are not provided.
Exact timestamp pairing is deliberately strict: physical sensor delivery must
establish whether the two streams are synchronized before this policy can be
used on the headset. The [Monado HMD](pico-hmd.md) uses the timestamp-query API
and requires explicit activation and a validated calibration file; there are
no physical sensor or optical defaults.

Run the native test with the pinned Monado archive and an optional verified SSC
bundle on a Linux host with Eigen, GLib and sanitizer-capable C/C++ compilers:

```sh
python3 -B tools/test-pico-head-tracking.py PINNED_MONADO_ARCHIVE \
  --output NEW_TEST_DIRECTORY --ssc-bundle VERIFIED_SSC_DIRECTORY
```

The test compiles the actual pinned Monado fusion sources. It verifies known
rotations on all three axes, explicit axis/unit conversion, batching, timestamp
and generation refusal, reconnection and concurrent reads. The session test
also covers 100 cancellation cycles without QRTR under ASan/UBSan and leak
detection. Seven isolated Pico-kernel QRTR scenarios exercise calibrated
orientation and refusal paths with a synthetic DSP, including a single initial
bias update and malformed gyro traffic. The fixture sends at 10 Hz and advances
its synthetic counter accordingly; a requested 1,000 Hz rate is not a measured
physical rate. These checks do not establish native headset tracking or boot.


Add `--replay-capture SAVED_CAPTURE_LOG` to exercise a preserved stock recording.
The existing parser validates its clock brackets, sequence and sample bounds.
Replay preserves the sample order and capture timestamps, with a monotonic
simulated arrival clock. Raw vectors use identity axes, unit scales of one and
synthetic accuracy for numerical testing only; vendor auxiliary fields and
calibration are not applied. The original capture is never changed.

The preserved Pico recording passes replay of 3,891 samples and 1,945 exact
sensor pairs under ASan/UBSan. It includes nine samples delivered behind a newer
sample from the other stream. Every available orientation is finite and
normalized at the expected paired capture time; expiry and disconnect clear
validity. This is recorded-input coverage of the new provider, not a measurement
of pose accuracy, real delivery latency or native SSC transport.

## Camera snapshots and synchronization

`pico-camera` decodes immutable snapshots of the b1977 tracking service's
89-byte packed camera header and one contiguous 8-bit plane. The wire format
is little endian and unaligned. Its fields are:

| Offset | Field |
| --- | --- |
| 0 | Camera ID, one byte |
| 1, 5 | Width and height, 32 bits each |
| 9, 13 | Pixel format and exposure line count |
| 17, 25, 33 | Timestamp, qtimer and corrected exposure time, 64 bits each |
| 41, 49 | Line duration and frame number, 64 bits each |
| 57, 61, 65 | Plane size, gain and frame classification |
| 69, 73 | Additional plane sizes |
| 77, 81, 85 | Uninterpreted stride fields and extra metadata |
| 89 | Pixel data |

Only stock format `0x123`, exact width × height storage, dimensions up to
4096 × 4096 and no additional planes are accepted. Other formats and padded
rows need an explicit conversion. The parser preserves its output on error.
A decoded view borrows the caller's immutable snapshot; it does not read a
live shared-memory region or establish ownership of a HAL buffer.

The stock writer at ELF offset `0x8bfcc` converts format `0x24` to `0x123`
and, when configured, converts RAW16 by taking each little-endian sample
right-shifted by two and truncated to eight bits. The stock reader at `0x8a8ac`
finds this header 97 bytes into its DataBuffer record. `releaseFrame` at
`0x8a9a8` is a no-op; it must not be used as proof that a retained pixel pointer
remains valid. The head-image writer can additionally run exposure processing
and alter its first timestamp. That processing is separate from this decoder.

`pico_camera_center` implements the observed qtimer correction: subtract
52,000 ns, the explicitly supplied readout correction and half the exposure
line count × line duration. It rejects invalid or overflowing arithmetic.
It does not convert clocks, choose timing constants or apply an IMU offset.
The saved camera XML is still not established as this unit's active calibration.

`pico_camera_head_set` validates two or four complete head images in an explicit
camera order. It rejects duplicate IDs, missing times, controller images and
skew above the configured limit, capped at the stock four-camera check of
100 µs. It neither infers camera identities nor supplies missing images.
The caller must establish a common clock and subscription generation.

540 synthetic records were serialized and acquired by the original stock ARM64
functions with isolated buffer callbacks. All decoded fields and 162,839
pixel values matched on macOS and Linux ARM64; sanitizer checks also cover
truncation, invalid dimensions, extra planes, timestamp overflow and skew
boundaries. These checks do not establish camera capture, head exposure
processing, calibration, SLAM accuracy or physical 6DoF tracking.

### Monado SLAM input

`pico-slam-input` copies accepted snapshots into reference-counted Monado
`xrt_frame` objects. The caller supplies camera IDs, calibrated image dimensions,
clock offset/error/expiry and calibrated IMU vectors. Camera callbacks can arrive
in any order; a complete set is submitted in configured order only after IMU
samples bracket its capture interval. At most one image per camera is pending.
Images retain their original qtimer timestamp and frame number independently
of their mapped presentation to the tracker.

After checking skew, the adapter assigns the earliest mapped capture time to
every image in the set. Basalt's VIT `push_frame` requires ordered camera indices
and exactly matching timestamps; independently forwarding close timestamps
would trigger its assertion. The stock Pico tracking service also contains an
optional earliest-time normalization before its head-SLAM callback. This is a
bounded synchronization policy, not evidence that physical camera timing matches.

Truncated frames, wrong dimensions/IDs, non-head images, stale or future data,
nonfinite IMU vectors, duplicate timestamps and older generations are rejected.
Clock expiry/reversal and excessive per-stream gaps invalidate the generation.
Stopping discards pending frames. References retained by a downstream sink stay
valid after input destruction. The caller must serialize calls, keep sinks alive
and reset or recreate the downstream estimator after a sensor restart.

The adapter passes 39,118 checks on macOS and Linux ARM64 with ASan/UBSan,
including 4,006 image deliveries, 2,007 IMU deliveries and 1,010 generations.
The tests use the real pinned Monado interfaces and retaining test sinks, not a
pose estimator. They do not enable physical cameras or advertise positional
tracking. Run them with:

```sh
python3 -B tools/test-pico-slam-input.py PINNED_MONADO_ARCHIVE --output NEW_TEST_DIRECTORY
```

The separate [native Basalt replay](pico-slam.md) tests this adapter with real
recorded images and IMU samples and an actual 6DoF estimator. Its reference
calibration is not a physical Pico calibration.
