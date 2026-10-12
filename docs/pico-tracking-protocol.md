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
