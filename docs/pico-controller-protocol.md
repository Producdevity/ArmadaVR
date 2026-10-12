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

On a Linux host with the listed development dependencies, run:

```sh
python3 tools/test-ssc-reports.py /path/to/pinned/libssc --output /path/to/new-results
```

The runner verifies source hashes, applies the patch with zero fuzz, generates
protobuf-C bindings, and tests the actual original and patched dispatchers with
AddressSanitizer and UndefinedBehaviorSanitizer. It checks multiple reports,
SUIDs, zero and maximum timestamps, malformed outer messages, legacy mutation,
and payload decoding after the wire buffer and client have been destroyed.
The encoded events also pass an independent Google protobuf parser loaded with
the exact Pico `sns_client.proto` and `sns_ble_mcu.proto` descriptors.

The dispatcher test does not open QRTR or start a DSP. Docker's current kernel
does not provide AF_QIPCRTR, so upstream's QRTR mock tests cannot run there.
The library is staged privately, not installed into the headset image; live
stream lifetime, suspend/resume, timestamp alignment and controller input
delivery remain open integration work.

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
