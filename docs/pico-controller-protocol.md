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
record boundary; the protobuf schema and live SSC connection still need validation.

The outer event timestamp at offset `0x10` uses SSC ticks multiplied by
`52.083333333333336`, plus an offset measured from Android elapsed real time and
the CPU counter. This is distinct from the timestamps inside the controller IMU
payload. The callback receives a queue-owned pointer, while `pollEvents` copies
an entire event before freeing its queue entry. A receiver must establish buffer
ownership and shutdown ordering; retaining the callback pointer is unsafe.

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

This is source and object-level acceptance. The complete kernel has not yet been
rebuilt with this driver; physical GPIO routing, interrupt delivery, suspend and
stock-service interoperability remain unverified. The preserved previously
accepted kernel image is unchanged. The driver supplies notifications only; SSC
controller data and optical tracking remain separate requirements.

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
**14,625** decoding and rejection checks on macOS ARM64 and Linux ARM64 with
AddressSanitizer and UndefinedBehaviorSanitizer. It tests unaligned buffers,
every short length, overlong records, unsupported configuration, records without
key data, and every single-bit corruption in each complete record. Rejection
preserves the previous sample. `just check` compiles and runs this fixture.

The private firmware, analysis, comparison runner and results are retained in
`output/pico-block-ota-20261011-v1/`. Physical button/axis captures, SSC transport
startup and lifetime, station driver reconstruction, pairing, haptics and optical
controller tracking remain unverified. This decoder establishes offline parsing
compatibility, not a working physical controller driver or flash readiness.
