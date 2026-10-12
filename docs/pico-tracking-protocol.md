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
the Armada kernel.

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
