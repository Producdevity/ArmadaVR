# Pico Neo3 controller receive protocol

The b1977 stock system image from the authenticated 5.8.4.0 OTA contains
`libstationopticsservice.so`, SHA-256
`91b45470678ca7d614a7f12ccb21261f3cda273c4ba6b08c1edb153cd5b6286e`.
Ghidra and AArch64 disassembly identify a receive path distinct from the
40-byte MCU command/response protocol on `/dev/spidev0.0`.

`StationService::SlpiDataAnalyze` consumes a **100-byte** record. Byte 99 is
CRC-8 over bytes 0–98, initialized to zero, with reflected polynomial `0x8c`
and no final XOR. Its complete lookup table matches that polynomial. Input
records begin with `0xa5`; `0xbb` heartbeat and `0xaa` test records are separate.
For stock MCU type 1, key data is processed only when byte 1 equals 1. Type 2
requires bit 2 of byte 1. These are observed configuration values, not inferred
hardware model names. The SLPI startup requests a Qualcomm SSC stream named
`ble_mcu`; it is not equivalent to opening the SPI command device.

The service loads `sensors-common-imu.pvr.so`, SHA-256
`0fc16cf6482c852c64be9035c8286c7f467a4518989843aeecce607270070159`,
from the same authenticated system image. Its `SLPIInit("ble_mcu", 480)` route
uses `libssc.so` and `libsns_api.so`. For SSC message ID `0x400`, the library
parses `sns_ble_mcu_data` and copies 25 uint32 words into the 100-byte station
record at offset `0x1018` of a `0x1980`-byte event. This establishes the inner
record boundary. The payload and outer event schemas have been checked offline
as described below; the live SSC connection still needs validation.

The outer event timestamp at offset `0x10` uses SSC ticks multiplied by
`52.083333333333336`, plus an offset measured from Android elapsed real time and
the CPU counter. This is distinct from the timestamps inside the controller IMU
payload. The callback receives a queue-owned pointer, while `pollEvents` copies
an entire event before freeing its queue entry. A receiver must establish buffer
ownership and shutdown ordering; retaining the callback pointer is unsafe.

The same-build device's `libssc.so`, SHA-256
`9adc9c3c6aee833e3566bf3574844c038098385abd7c94c81aeedfd07edc0fa8`,
contains the serialized `sns_ble_mcu.proto` descriptor. Its 309-byte length
also matches the descriptor registration call recovered with Ghidra. Field 1
is repeated `fixed32` controller data; field 2 is a required sample-accuracy
enum with values 0 through 3. It is not a byte-string field. The first 25 words
are the station record consumed by the stock service.

`pico_controller_decode_ssc` accepts this payload and calls the existing CRC and
key decoder. It requires exactly 25 words and an accuracy value in range,
supports packed, unpacked and mixed word encoding, skips unknown scalar fields,
and refuses groups, malformed lengths and payloads over 4,096 bytes. Both output
values remain unchanged on failure; no payload pointer escapes the call.
The caller must still validate message ID, stream identity, the outer timestamp
and connection lifetime. This receiver does not open a QMI connection or start
the SLPI.

The bounded receive-only implementation is
[`pico_controller_decode`](../src/pico-controller.h) in
[`pico-controller.c`](../src/pico-controller.c). It checks length, header, CRC
and key-data presence before changing the caller's sample.

| Offset | Meaning |
|---:|---|
| 3 | Trigger analog byte |
| 4, 5 | Stick X and Y analog bytes |
| 6 | Grip analog byte |
| 7, bits 0–5 | Home, app/menu, A/X, B/Y, grip, stick click |
| 7, bit 6 | Hand: set means left (1), clear means right (2) |
| 7, bit 7 | Trigger click |
| 8, bits 0–4 | A/X, B/Y, stick, thumb-rest and trigger touch |
| 9 | Battery, interpreted according to the explicit radio type |
| 99 | CRC-8 |

Radio type 1 maps battery values 0–5, 6–25, 26–50, 51–75 and 76–255 to levels
1–5. Radio type 2 keeps the low three bits. The stock service gates touch fields
with its touch-support flag; the decoder takes that flag explicitly. Analog
values remain unsigned bytes. A default value of 128 appears for disabled stick
axes in the service, but does not establish the physical center, dead zone,
range, axis sign or calibration of an enabled controller.

The stock service separately applies per-button configuration masks and a test
hook, checks controller online state, updates shared memory, and emits Binder
notifications. The raw decoder performs none of those lifecycle operations. Its
caller must obtain the active MCU/radio configuration and supply complete records
from a verified transport. It does not pair controllers, send haptics, access a
device, load firmware or produce a tracked pose.

## Controller configuration

The stock MCU selector reads `/sys/hwinfo/version_id`. In station mode 0,
revision bits `0x38` select MCU type 2 when any are set, otherwise type 1.
Mode 1 uses bit `0x10`; a low byte of `0xff` is unknown. Stock constructs
`MCUNRF52832` for type 1 and `MCUIN618` for type 2. This sysfs file is not
readable by the Android shell on the inspected Pro Eye.

The radio type comes from a separate MCU device-info reply: byte 16 equal to
0 selects type 1, and 1 selects type 2. Other values leave the previous value
unchanged. The constructor's zero is therefore not a usable default. These
findings come from `mcuGetType`, `MCUServer::initialize` and
`StationService::MCUCallBackDataProcess` in the hash-pinned stock library.

[`pico-controller-config.py`](../tools/pico-controller-config.py) reads the stock
configuration through verified Binder getters. It checks the device, exact
b1977 fingerprint, station library hash and configuration-service APK hash
before making any Binder call. Station transactions 76 and 74 return radio
type and touch enablement. ConfigurationService transaction 3 reads
`sys_controller_chip` and `sys_controller_prototype`. The latter must agree
with the live radio getter. Two snapshots and the Android boot ID must agree.
Exceptions, unexpected response layouts and unsupported values fail the read.

```sh
python3 tools/pico-controller-config.py --serial DEVICE_SERIAL \
  --output output/pico-controller-config.json
```

The collector issues no setters, resets, sensor subscriptions or firmware
operations. The output contains the device serial and belongs in private
evidence. It describes stock service state, not independently measured silicon
or analog calibration, and must not become a default for another unit.

The connected Neo3 Pro Eye running 5.8.4.0 reports **MCU type 1, radio type 2,
touch enabled**. The stored touch property is empty while the live getter
returns 1; an empty property must not be interpreted as disabled. The MCU type
is the value published by stock startup into its configuration service; direct
revision readback remains unavailable. The configuration APK is SHA-256
`701ac92f7d5598f0f7dbd7fa5bdd276cbecc19c967e5472c3090396fb39f595f`.
Its getter dispatch and query implementation were inspected offline. Private
analysis and physical read receipts are retained in
`output/pico-controller-config-20261012-v1/`.

## Native Linux SSC transport

The native [libssc](https://codeberg.org/DylanVanAssche/libssc) source is pinned
in [`profiles/ssc.json`](../profiles/ssc.json), including its GPL-3.0-or-later
license. The complete library, CLI and test executables build on ARM64 Linux
with GLib 2.88.3, libqmi 1.36.0 and protobuf-c 1.5.2. Its QMI service 400 and
small/large report IDs `0x21`/`0x22` agree with the same-build Pico library's
recovered IDL tables. This establishes protocol overlap; it does not prove DSP
startup, SUID discovery or a physical `ble_mcu` subscription.

Upstream's `report` signal discards the outer DSP timestamp and frees its mutable
array after callbacks return. The pinned
[`report-timestamps` patch](../patches/libssc/0001-report-timestamps.patch) adds
`report-timestamped` with message ID, SUID high/low, raw 64-bit timestamp and
immutable `GBytes`. Consumers can retain the bytes with `g_bytes_ref`. The
original signal remains available with its existing arguments. The new payload
is copied independently, so a legacy callback cannot mutate retained data.
Timestamps remain raw DSP ticks; clock alignment is still the receiver's job.

The [main-context patch](../patches/libssc/0004-own-sensor-main-context.patch)
keeps synchronous client cleanup and the registry-discovery deadline on the
calling thread's GLib context. Upstream cleanup iterates the global context
instead, which can run unrelated callbacks and leave its own completion waiting
on another context. Its registry timer also uses the global context. The patch
retains the selected context, waits for its completion without busy polling,
and owns the timer as a source that can be destroyed on its actual context.

The original wrong-context dispatch is reproduced under native ARM64 sanitizers.
The patched source passes 128 private-context completions, registry timeout and
timer disposal checks. The packaged library also completes three real QRTR
discovery/open/report/close cycles on a private context in the Pico-kernel VM;
the default-context and protocol negative controls pass too. Evidence is under
`output/pico-ssc-context-20261012-v1/`. This verifies context ownership, not all
cancellation and DSP-loss paths or a production sensor worker.

The [pending-operation patch](../patches/libssc/0005-own-pending-sensor-operations.patch)
fixes a reproduced crash when an enable request fails and a late measurement
uses the freed task. Discovery, attributes and enable now share explicit
operation ownership: completion removes the report handler, deadline and
cancellation source exactly once, while outstanding transport callbacks retain
their own reference. A thirty-second deadline covers the complete discovery
sequence and each enable. Cancellation is dispatched on the owning context;
a concurrent enable is refused, and close cancels a pending enable.

Fourteen native sanitizer cases cover transport errors, cancellation and timeout
at each phase, a report arriving before its acknowledgement, and a second enable
while one is pending. The previous revision's crash and missing cancellation and
enable deadline are retained as negative controls. The packaged library passes
five diskless Pico-kernel suites, including actual QRTR cancellation on a private
context, both-hand device inputs and three reconnects on each context. All 397
host tests pass. Evidence is under `output/pico-ssc-operation-20261012-v1/`.
The DSP endpoint remains mocked; physical startup and transport loss still need
device validation.

The [close-error patch](../patches/libssc/0006-log-close-errors-before-transfer.patch)
fixes a further use-after-free found by removing the mock SSC server during a
live subscription. The close callback logged an error after transferring it to
a task; the caller can consume and free that error synchronously. Logging now
precedes the ownership transfer. The original failure reproduces under ARM64
AddressSanitizer, and the patched source passes 100 alternating successful and
failed closes. The server-loss case also exits cleanly on the exact Pico kernel
in QEMU. Failed runs, the crash backtrace and passing results are retained under
`output/pico-ssc-session-20261012-v1/`.

[`ssc-clock`](../src/ssc-clock.h) maps the outer 19.2 MHz timestamp into an
explicitly selected host-clock domain. Initialization takes a CPU QTimer read,
its measured frequency and two host-clock readings bracketing that read. It
rejects an excessively wide bracket and carries its uncertainty into each
sample. Integer arithmetic preserves small deltas above double's precision
and checks the entire timestamp interval for stale or future data. Rejected
input preserves state and output. The caller must recalibrate and reset stream
ordering after resume, reconnect or a DSP reset; no hardware clock origin is
inferred. This does not align the separate timestamps inside controller IMU
payloads.

The mapper passes macOS and ARM64 Linux sanitizer fixtures, including the full
64-bit range, mismatched CPU/DSP frequencies and calibration boundaries. An
independent exact-rational comparison covers 30,000 timing intervals. The
native SSC controller client also uses it on both-hand reports in the Pico
kernel VM. Evidence is under `output/pico-ssc-clock-20261012-v1/`; its clock
calibration and DSP reports are synthetic, so physical timing remains unverified.

[`ssc-clock-linux`](../src/ssc-clock-linux.h) provides explicit Linux ARM64
counter acquisition for a session's calibration callback. It brackets
`CNTVCT_EL0` with `CLOCK_MONOTONIC`, checks `CNTFRQ_EL0` on both sides, and keeps
the narrowest acceptable bracket from a bounded number of attempts. Counter
resolution is added to the uncertainty. Invalid clock results, reversed time,
frequency changes or an excessive bracket fail without changing the output.
The target kernel must permit userspace counter access; this is not an automatic
probe or proof that the CPU and DSP share an origin.

Injected clock failures and 64 real captures pass native ARM64 sanitizers. The
same capture fixture passes on the exact Pico kernel in diskless QEMU, along
with the eight SSC session scenarios. The native lab reports a 24 MHz counter
and QEMU reports 62.5 MHz, exercising conversion to the SSC 19.2 MHz domain.
Evidence is under `output/pico-ssc-counter-20261012-v1/`. These are host and
virtual counter measurements; physical Pico sensor timing remains unverified.

[`pico-controller-stream`](../src/pico-controller-stream.h) combines the payload
decoder and clock mapper with subscription identity. Each new subscription has
a generation number; callbacks must retain their original number. Reports from
an old generation, another SUID, or another message type are refused. Timestamp
ordering is independent for each hand, so a newer left-hand report does not
discard a valid right-hand report. Duplicate, reversed, stale, future and
host-clock-regressing reports leave both stream state and output unchanged.
Disconnect invalidates the stream immediately; consumers must release active
controls and obtain a fresh calibration before resubscribing.
macOS and ARM64 sanitizer tests cover 3,058 refusals and 1,000 subscription
generations. The native QRTR client completes three discovery/open/delivery/close
cycles with the packaged library on the Pico kernel; all ten suites pass.
Evidence is under `output/pico-controller-stream-20261012-v1/`. DSP reports and
clock calibration remain synthetic.

[`pico-monado-device`](../src/pico-monado-device.h) creates left and right
`xrt_device` objects using the pinned runtime's device helpers. A separately
owned producer publishes accepted stream events under a mutex; `update_inputs`
copies complete snapshots into the runtime-owned arrays. Each hand times out
independently, clearing values and deactivating inputs. Transport disconnect or
producer destruction also clears the next snapshot. Devices and the producer
can be released in either order; a new connection requires a newer generation.
The source clock must be calibrated to Monado's `CLOCK_MONOTONIC` domain.
No pose tracking or output capabilities are advertised. Pose queries return an
invalid relation, not a simulated controller position.

The device callbacks, upstream allocation-independent device helpers and unique
IDs pass macOS and ARM64 AddressSanitizer/UndefinedBehaviorSanitizer tests.
ThreadSanitizer also passes on macOS, including 20,000 events across 1,000
generations with concurrent per-hand snapshots and device destruction. The
runner is `tools/test-pico-monado-device.py`; evidence is retained under
`output/pico-monado-device-20261012-v1/`. These tests use synthetic events.
The exact Pico kernel also passes SSC discovery, decoded both-hand reports,
device callbacks, disconnect clearing and three reconnect cycles in diskless
QEMU. The fixture uses synthetic DSP reports and an explicit monotonic clock
anchor with a ten-second test age limit; it does not establish hardware latency.
The factory is connected to the session owner below. Runtime builder hookup
remains; it is not automatically probed or installed into the headset image.

[`pico-ssc-session`](../src/pico-ssc-session.h) owns discovery, subscription and
shutdown on a dedicated GLib context and thread. It discovers the `ble_mcu` SUID,
calls an explicit monotonic-clock calibration provider, and publishes validated
reports to the two device objects. MCU/radio configuration and analog
calibration remain explicit inputs. Invalid calibration fails startup.
Missing valid reports cancel the subscription and release controls; malformed
traffic cannot keep the session alive. There is no automatic reconnect using
an old clock calibration.

Stopping cancels pending operations, attempts sensor disable, disconnects report
callbacks and drains the context until the sensor and client are destroyed.
It then joins the worker before releasing the producer. Devices can be released
independently and may outlive the session. Status and counters are copied under
a mutex; a failed startup or lost stream remains visible to the runtime owner.
This source does not register a runtime builder or start the DSP.

[`pico-monado-prober`](../src/pico-monado-prober.h) adapts the session to Monado's
auto-prober interface. It stays inactive unless `PICO_SSC_CONFIG` names a
successful b1977 receipt from the collector above and `PICO_SSC_COUNTER=1`
explicitly enables ARM counter calibration. This is a developer opt-in; the
receipt is not proof of the identity of the machine running Monado or of a shared
CPU/DSP clock origin. It must only be used with the corresponding headset.
No configuration file or environment setting is installed automatically.

The prober accepts a bounded regular JSON file, rejects ambiguous or unsupported
configuration, opens at most one session, and creates only the two controllers.
It owns the session until prober destruction; devices may outlive it. The
initial limits are 100 ms sample age and 500 ms stream/hand timeout, pending
physical latency validation. Analog calibration remains absent, so only digital
and enabled touch inputs can become active. It creates no HMD or tracked pose.
The current packaged runtime still needs this prober's build registration.

`tools/test-pico-monado-device.py` also compiles the actual prober with upstream
Monado headers and cJSON. Its 123 checks include 100 create/destroy cycles,
explicit opt-in, session-start failure, bad/duplicate configuration fields,
binary/trailing data, oversized files and nonregular paths. The session and
counter calls are stubbed in this boundary test; transport is tested separately.
macOS and Linux ARM64 sanitizer results are retained under
`output/pico-monado-runtime-20261012-v1/`.

`tools/test-pico-ssc-session.py` builds against the pinned Monado source and a
verified SSC bundle. On native ARM64, 100 alternating immediate-cancellation
and unavailable-QRTR cycles pass AddressSanitizer/UndefinedBehaviorSanitizer.
`tests/ssc-session-vm.py` tests the actual session over QRTR on the Pico kernel:
three both-hand start/stop cycles, immediate stop, absent service, invalid clock,
delayed enable acknowledgement, loss of valid reports, all-malformed reports,
and abrupt server termination. All eight scenarios pass and the VM shuts down
normally. The DSP producer and clock anchor are synthetic; the two-second
fixture time limits are not physical latency measurements. Evidence is under
`output/pico-ssc-session-20261012-v1/`.

[`pico-monado-input`](../src/pico-monado-input.h) maps accepted events to Monado's
existing Neo3 input names: X/Y on the left, A/B on the right, menu/system,
trigger, stick and grip, plus supported touch inputs. It clears values and
deactivates inputs on disconnect. Analog calibration supplies explicit released
and pressed endpoints, stick centers, ranges, dead zones and axis inversion.
Without calibration, analog inputs remain inactive. The stock service's disabled
axis value of 128 is not used as physical calibration. Grip and aim pose inputs
remain inactive; no tracking result is invented.

Run the adapter tests against the hash-verified Monado archive:

```sh
python3 tools/test-pico-monado-input.py /path/to/monado-source.tar.gz \
  --output /path/to/new-results
```

macOS and native ARM64 sanitizers pass 6,912 mapping checks using actual Monado
headers. Evidence is under `output/pico-monado-input-20261012-v1/`. The adapter
also receives both-hand events through the native SSC client on the Pico kernel,
including three reconnect cycles and disconnect clearing (`controller-vm-v1/`).
Its calibration and DSP producer are synthetic. The adapter
still needs runtime registration and a verified physical calibration source
before it can deliver device input to applications.

On a Linux host with the listed development dependencies, run:

```sh
python3 tools/test-ssc-reports.py /path/to/pinned/libssc --output /path/to/new-results
```

The runner verifies source hashes, applies the patches with zero fuzz, generates
protobuf-C bindings, and tests the actual original and patched dispatchers with
AddressSanitizer and UndefinedBehaviorSanitizer. It checks multiple reports,
SUIDs, zero and maximum timestamps, malformed outer messages, legacy mutation,
and payload decoding after the wire buffer and client have been destroyed.
The encoded events also pass an independent Google protobuf parser loaded with
the exact Pico `sns_client.proto` and `sns_ble_mcu.proto` descriptors.

The [`object cleanup` patch](../patches/libssc/0003-release-partial-and-repeated-disposal.patch)
also releases a partially initialized client's QRTR bus and makes repeated
sensor disposal safe. The same runner reproduces the original bus leak and
double-free, then checks the patched source with sanitizers and GObject weak
references. The rebuilt library passes all 15 upstream cases and the three
controller cases on the Pico kernel. Evidence is under
`output/pico-ssc-lifetime-20261012-v1/`; `controller-vm-v1/verification-v2.json`
rechecks the saved kernel records after the original host checker incorrectly
expected three suites instead of ten. This does not verify physical reconnects.

The dispatcher test does not open QRTR or start a DSP. Docker's current kernel
does not provide AF_QIPCRTR. A separate diskless VM runs the exact Pico kernel
with a userspace QRTR name service: all 15 upstream cases in seven suites pass,
including discovery and sensor samples against the upstream DSP mock. See
[Qualcomm services](qcom-services.md) for the missing name-service dependency.
Live stream lifetime, suspend/resume, physical clock calibration and controller
input delivery remain open integration work.

[`ssc-build.py`](../tools/ssc-build.py) builds the pinned source archive on native
ARM64 Linux, runs the source regression tests, and packages the installed library
and CLI under `/opt/armada-vr/ssc`. Four pinned Fedora runtime dependencies are
included with their licenses; the bundle uses the root's GLib and C library.
RPM verification checks those dependency files before copying. The original SSC
source archive, local patches and license accompany the runtime. The builder
requires an immutable container image ID for its provenance record and performs
no downloads:

```sh
python3 tools/ssc-build.py /path/to/libssc-source.tar.gz \
  --output /path/to/new-bundle --image-id sha256:CONTAINER_IMAGE_ID
```

`build-headset-root.py --ssc /path/to/new-bundle` installs the validated bundle
for a Pico root with QRTR services. It checks exact inventory, hashes, permissions
and links, verifies native relocations using the root's loader, runs the CLI's
version command, and reads the installed files back from ext4. This adds no
automatic sensor subscription or DSP startup. Consumers must select the bundle's
`lib64` directory explicitly.

The offline root at `output/headset-root-pico-ssc-20261012-v1/` contains this
bundle. Its parent image was preserved and rehashed unchanged. Native loader,
missing-dependency refusal and ext4 readback evidence are retained under
`output/pico-ssc-root-20261012-v1/`. Physical sensor operation and custom boot
acceptance remain unverified.
The exact packaged library and CLI also pass the ten-suite Pico kernel VM run
in `controller-vm-v2/`, including 15 upstream tests and both-hand controller
delivery. The earlier fixture omitted a parent directory; its failure is retained
in `controller-vm-v1/`.

Pico controller samples also expose an upstream subscription-startup mismatch:
`ble_mcu` reports message `1024`, while libssc normally completes sensor opening
on measurement `1025`, proximity measurement `769`, or a configuration event.
[`ble-mcu-startup`](../patches/libssc/0002-ble-mcu-startup.patch) recognizes `1024`
only for the `ble_mcu` data type. It does not treat arbitrary vendor message IDs
as a successful subscription.

The public-API client in [`tests/ssc-controller.c`](../tests/ssc-controller.c)
and [`SSC mock extension`](../tests/ssc-controller-mock.py) exercise discovery,
subscription, timestamped delivery, both-hand decoding and close over real QRTR
in the isolated Pico kernel VM. The mock advertises a 480 Hz stream and sends
the existing stock-validated synthetic records, malformed payloads, another
SUID and an unrelated message ID. Both programs require the explicit VM marker;
they are not physical-device test launchers.

With the timestamp patch alone, the client receives 19 valid samples and rejects
seven malformed payloads, but opening never completes. With both patches,
opening and closing succeed after four valid samples from both hands and one
malformed-payload rejection. A different data type emitting `1024` still cannot
complete opening. The same library passes the 15 generic upstream cases and
the dispatcher sanitizer checks. Evidence is retained under
`output/pico-ssc-transport-20261012-v1/research/controller-vm-v2/` and
`controller-vm-v1/generic-verification.json`. A real Pico stream may require
additional configuration; these results establish the virtual protocol path.

## Station interrupt driver

The authenticated b1977 kernel supplies the separate `/dev/stationdev` interface
missing from the published source. Recovered symbols, relocations and instruction
analysis identify a platform driver with compatible `bytedance,station`, GPIO
property `station-sync`, and a rising-edge, one-shot threaded IRQ. Its file
operations are open, poll and release; there is no read, write or ioctl data path.
An interrupt makes poll return `POLLIN | POLLRDNORM`; polling consumes that
notification. Multiple interrupts can coalesce into one readiness result.

[`0024-station-notifications.patch`](../patches/kernel/pico-neo3/0024-station-notifications.patch)
implements this interface and adds `CONFIG_PICO_STATION`, requested by the Pico
Linux profile. The GPIO direction remains owned by the device tree, as in stock.
Each open file tracks its own event sequence, so opening or polling another file
cannot erase its notification. Removal unregisters and drains the IRQ, wakes
waiters with `POLLERR | POLLHUP`, and retains state until existing files close.
Rebinding creates independent state; old descriptors keep reporting disconnect.
Probe errors release resources and retain deferred-probe error codes.

The patched source compiles as an ARM64 kernel object with `-Werror` against the
headers and recorded flags from the accepted physical-configuration Pico build.
All 26 imported symbols exist in that kernel, and the kernel's configuration
resolver retains `CONFIG_PICO_STATION=y` with its OF and GPIOLIB dependencies.
The fault harness compiles the actual driver functions with kernel API stubs:
**74 cases** pass with AddressSanitizer and UndefinedBehaviorSanitizer on macOS
and Linux ARM64, including 64 concurrent IRQ/poll/removal races. It checks probe
failures, independent readers, counter wrapping, detach and rebind. `just check`
runs this harness; `tools/test-pico-station.py SOURCE_MISC --output NEW_DIRECTORY`
also verifies the pinned build files and applies the patch with zero fuzz.

The complete physical-configuration kernel now builds with this driver enabled.
All 38 module signatures, 2,715 imported symbol versions and dependencies pass
verification. A diskless virtual boot checks the full loaded Image and passes
the Android ABI regression before shutting down normally. This virtual board
does not exercise the station GPIO or its interrupt handler.

Physical GPIO routing, interrupt delivery, suspend and stock-service
interoperability remain unverified. The driver supplies notifications only;
SSC controller data and optical tracking remain separate requirements. The new
kernel and the previously accepted image are both retained; see
[kernel verification](pico-firmware.md) for the artifact identity.

## Verification

An isolated ARM64 Linux process executed the stock CRC and key parsing functions
against synthetic buffers, with both controllers online, service key masks
enabled, the test hook disabled, and callbacks/logging avoided. It did not run
ELF constructors, Android service initialization or hardware operations. The new
decoder matched all eight output fields on **16,384** records covering both
hands, all button bits, both MCU/radio modes, touch enabled/disabled and all
256 battery values. CRC bytes for those comparisons came from the original
function rather than the new implementation.

Sixteen fixed records and expected outputs are retained in
[`tests/pico-controller.c`](../tests/pico-controller.c). The fixture passes
**51,946** decoding and rejection checks on macOS ARM64 and Linux ARM64 with
AddressSanitizer and UndefinedBehaviorSanitizer. It tests unaligned buffers,
every short length, overlong records, unsupported configuration, records without
key data, every single-bit corruption in each complete record, and SSC framing,
word counts, accuracy and unknown fields. Rejection preserves both outputs.
`just check` compiles and runs this fixture.

An independent Google protobuf parser loaded with the device's exact descriptors
agrees on 4,096 payloads, including mixed encodings and reordered fields. A
separate 200,000-case mutation run passes both sanitizers on macOS and ARM64
Linux. The libraries, descriptors, bounded Ghidra analysis and comparison scripts
are retained in `output/pico-ssc-transport-20261012-v1/`. No live sensor stream
was started for these checks.

The private firmware, analysis, comparison runner and results are retained in
`output/pico-block-ota-20261011-v1/`. Physical button/axis captures, SSC transport
startup and lifetime, physical station-driver operation, pairing, haptics and optical
controller tracking remain unverified. This decoder establishes offline parsing
compatibility, not a working physical controller driver or flash readiness.
