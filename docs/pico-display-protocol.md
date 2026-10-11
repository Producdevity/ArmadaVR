# Pico Neo3 display bridge

The authenticated Pro Eye b1977 kernel contains the VXR7200 driver omitted from
ByteDance's published kernel source. The raw boot kernel has SHA-256
`e5386b05957dee245891a4d3ad4bebc4e10d30b74f78f449db6849d3abf8c984`.
Recovered symbols, AArch64 instructions and targeted Ghidra analysis identify
the register transport and board controls below. These are offline findings;
the selected live device tree and replacement display operation remain unverified.

## Register transport

[`vxr7200-transport.c`](../src/vxr7200-transport.c) implements the stock wire format
through an explicit bus callback:

| Operation | I2C messages | Result |
|---|---|---|
| Read 32-bit register | Write four little-endian address bytes, then read five bytes in the same transaction | First four reply bytes form the little-endian value |
| Write 32-bit register | One eight-byte write: little-endian address followed by little-endian value | Complete message required |
| Read chip ID | Write address `0x200d38`, then read exactly four bytes | Little-endian uint16 at reply offsets 2–3 |

The stock read at `0xffffff80086d3cf4` ignores the fifth reply byte. Its meaning
has not been established; the new transport preserves this behavior. Stock
continues after a failed or partial transfer and can return uninitialized stack
data. The new API instead returns the adapter's negative error, treats an
unexpected message count as `-EIO`, and leaves the output value unchanged.

The stock write at `0xffffff80086d3c34` attempts up to three transfers with 20 ms
delays and discards the final error. The new API performs one attempt and returns
its result. Retry policy belongs to the caller because register writes can trigger
commands. The callback contract requires bounded, synchronous transfers; adapter
address selection, locking and timeout remain the adapter's responsibility.

The chip-ID path calls `SynaVmmRegRead` with address `0x200d38` and length four.
That function uses a combined I2C address write and an exact-length read, so this
path receives four bytes rather than the register helper's five. The `syna_rc_*`
mailbox operations are separate. `vxr7200_read_chip_id` implements this exact
probe transaction with the same error/output-preservation contract. It returns
the observed ID, leaving supported-device selection to the caller. No
firmware-update or power command is issued implicitly by the transport.

## Video observations

[`vxr7200-video.c`](../src/vxr7200-video.c) reads the five timing registers used
by stock `SynaVxr_Get_IOTimings`, `SynaVxr_IsVideoIn` and `SynaVxr_FrameRate`.
The stock log strings identify the upper/lower halves of `0x220830` as vertical
and horizontal totals, and `0x220838` as vertical and horizontal active counts.
These register names do not establish the selected panel's orientation.

Stock's video-presence check accepts only these `(vertical, horizontal)` pairs:
`(3840, 2160)`, `(3664, 1920)` and `(3664, 3840)`. The new status reports that
allowlist result as `stock_dimensions`, alongside the actual counts. It does
not interpret a match as input lock, sustained video or permission to switch
the mux. Unknown and zero dimensions remain observable.

The stock frame-rate calculation is
`1,000,000 * ((reg_200008 >> 16) & 1023) * (reg_220804 & 255) / reg_220814`.
The new API preserves its numerator and denominator as integers, avoiding
kernel floating point and division by zero. A zero operand marks the rate
unavailable. Two complete register observations must match; a change returns
`-EAGAIN`. Failed or partial I2C transfers stop immediately and leave the output
unchanged. Matching observations are not an atomic hardware snapshot.

The caller must own power/reset and serialize the complete read against bridge
commands. The helper issues no writes, retries or power transitions. Host and
ARM64 sanitizer checks cover 4,096 observations, every transfer-failure position,
changing registers and numeric boundaries. Another 327,680 comparisons match
the original dimension predicate, and 8,192 frame-rate comparisons agree within
`1.4e-7` relative error from stock float rounding. The stock routines run only
in an isolated process with register reads and logging substituted; no hardware
or kernel startup is involved. Evidence is in
`output/pico-video-detection-20261011-v1/`.

## Board controls

Stock probe at `0xffffff80086d23b0` uses compatible `qcom,vxr7200` and these
properties. The pin numbers below come from the retained candidate stock tree,
not a confirmed live-tree selection:

| Property | Candidate GPIO | Purpose |
|---|---:|---|
| `qcom,vxr7200-1v8en` | 23 | 1.8 V bridge rail |
| `qcom,vxr7200-1v0en` | 15 | 1.0 V bridge rail |
| `qcom,vxr7200-3v3en` | 14 | 3.3 V bridge rail |
| `qcom,vxr7200-rst` | 91 | Bridge reset |
| `qcom,vxr7200-mipi_switch` | 64 | Qualcomm/external bridge route |
| `qcom,vxr7200-plugin` | 68 | External video detection |

The stock power helper raises 1.8 V, raises 3.3 V, waits 5 ms, then raises 1.0 V.
Power-off lowers 3.3 V, 1.0 V and 1.8 V, in that order. The mux path selects
Qualcomm with zero and the external VXR7200 path with one. It coordinates panel
blanking, panel power/reset, switching delays and backlight restoration. Those
operations cannot safely be reduced to an isolated GPIO write.

[`vxr7200-power.c`](../src/vxr7200-power.c) implements the rail sequence through
synchronous GPIO and delay callbacks. It starts with unknown power state and
requires a successful power-off before enabling. An enable failure attempts all
three shutdown steps; any shutdown error leaves state unknown and prevents a
subsequent enable until shutdown succeeds. Repeated requests for the known state
do not toggle the rails. The caller must own the GPIOs, hold bridge reset
asserted and serialize these operations with display switching and suspend.
This component does not select pins, change the mux or release reset.

The normal sequence matches the original helper in 4,096 isolated comparisons.
Host and ARM64 Linux sanitizer tests cover 124 failure scenarios, including a
failed operation that nevertheless changes its simulated GPIO and failures at
every shutdown step. These tests verify ordering and error handling; they do
not measure physical rail timing or establish safe panel integration.

## Standalone panel ownership

Kernel patch `0025-native-panel-mux.patch` connects bridge-control ownership to
the existing DSI panel lifecycle. An explicit panel property,
`pico,native-mipi-mux = <&bridge_node>`, references an available `qcom,vxr7200`
node containing the five control GPIO properties above. No candidate GPIO
numbers or selected panel path are compiled into the driver. The published
Pico source omits its vendor device trees, and the live tree selection remains
unverified, so no retained stock DTB is changed automatically.

With that property present, panel initialization acquires all five GPIOs without
requesting a new direction or value. It accepts only the observed stock native
state: reset asserted, bridge rails off, mux selecting Qualcomm, with every
line configured as an output. Missing pins, nonzero binding flags, duplicate
GPIOs, conflicting owners, inputs, high outputs and failed reads are rejected.
Deferred-probe errors are preserved. Partial acquisition releases every owned
GPIO, and panel initialization failures or removal release the mux ownership.

Every panel power-on rechecks the route under the existing panel mutex before
enabling panel supplies. This covers both ordinary and LP11-delayed preparation.
The stock panel's mode, reset, backlight and regulator code remains responsible
for panel operation. The mux helper does not use the stock bridge's global
panel pointer, sysfs writes or timer callbacks.

This first kernel integration preserves an established standalone route. It
deliberately returns an error for an external or unknown route; switching a live
panel requires a complete DRM modeset transaction, bridge reset/rail sequencing,
EDID/video detection and error recovery. Physical mux state, the selected panel
binding, suspend/resume and rendered output still require device verification.
The tests exercise 694 acquisition, state-change and cleanup cases under address
and undefined-behavior sanitizers on macOS and ARM64 Linux. Evidence is retained
under `output/pico-panel-mux-20261011-v1/`.

[`bind-pico-panel.py`](../tools/bind-pico-panel.py) assembles this binding into a
new offline DTB copy. It requires the source SHA-256 and explicit panel/bridge
paths, validates active ancestors, phandles, GPIO controllers, polarity and pin
uniqueness, and refuses an existing binding or output directory. After editing,
it verifies that only the binding and any required bridge phandle changed;
the memory description, reservations, boot CPU and source file remain intact.

The retained kona-v2.1/overlay-0 candidate passes this assembly for its SHARP493
72 Hz panel. Exactly two properties change. That panel choice is an offline
integration test, not a verified selection for the connected Pro Eye. The tool
does not change the stock default-panel property, infer bootloader selection,
modify a boot container, or access a headset. Its receipt keeps live selection,
GPIO state, boot, recovery and flash readiness explicitly unverified. The
candidate output and receipt are in
`output/pico-panel-mux-20261011-v1/offline-binding/`.

The complete kernel and all 38 modules compile with this patch. Module signatures
match the certificate embedded in the new Image, 2,715 imported symbol versions
match, dependency checks pass, and the diskless Android ABI fixture boots and
shuts down in 5.576 seconds. The Image SHA-256 is
`98a7d2d1cfdaed58d4ffa5f384edd687a1b5ae2d1a328863606d096c7ef3d544`.
These results verify build integration and virtual ABI behavior; the QEMU board
does not contain the physical Pico mux, panel or display controller.

Patch `0026-panel-gpio-errors.patch` additionally preserves two panel-startup
failures in the published DSI code. A failed mode-select GPIO operation no
longer gets replaced by the subsequent panel-test GPIO result. Failure to
configure panel VIO now stops startup before regulator enable and reset. It
does not change the successful sequence or the existing power-rollback policy.
The original code reproduces both masked errors; all 17 corrected startup
cases pass macOS and ARM64 sanitizer tests. The complete panel object compiles
with `-Werror` against the accepted kernel headers and has the same undefined
symbols. The patch applies without fuzz. This later patch has not yet been
included in a new full Image or physically tested; the earlier Image hash above
still identifies the preserved `0025` build. Evidence is in
`output/pico-panel-errors-20261011-v1/`.

Patch `0027-panel-shutdown-errors.patch` preserves the first failure from panel
test GPIO setup, pinctrl, either regulator group, backlight GPIO or panel VIO.
It still attempts every shutdown step in the original order, including the
25 ms delay and hardware-revision condition for the charge regulator. The
existing `dsi_panel_post_unprepare` caller returns this error to display teardown.
The legacy `vxr7200_panel_off` value alone remains insufficient to prove power
is off, since stock sets it before attempting shutdown.

All 49,152 fault combinations pass macOS and ARM64 sanitizer tests with the
operation sequence preserved. The original routine masks or replaces the first
error in 37,152 of those cases. The full panel object compiles with `-Werror`
and unchanged imports against the accepted kernel. This patch, like `0026`,
still requires a full Image build and physical validation. Evidence is in
`output/pico-panel-shutdown-20261011-v1/`.

The published `techpack/display/msm/dsi/dsi_panel.c` already contains
`dsi_panel_post_enable_vxr7200` and the 72/90/120 Hz panel/backlight calls; its
control flow agrees with the recovered stock routine at `0xffffff8008db7560`.
This provides reusable panel code. The missing bridge binding, power/mux
sequencing, event lifecycle, EDID/video detection and suspend behavior still
require implementation and physical verification. The unrelated AYN reference
driver has different GPIO bindings and unsafe probe cleanup; it is not a drop-in
replacement.

## Verification and remaining work

The transport and chip-ID probe pass **12,581** sanitizer checks on macOS and ARM64 Linux,
including incomplete transfers, negative errors, output preservation, every
possible fifth reply byte and register/value encoding. Another **16,384** cases
compare the exact original stock read/write routines with the new transport.
Only those two routines run in a private ARM64 process, with their I2C callees
replaced by in-memory callbacks. All request bytes and returned values agree.
A separate stock-probe comparison covers all **65,536** possible chip IDs,
verifying the four-byte reply length and upper-halfword selection. No kernel
boot, initializers, physical I2C access or firmware update occurs in these tests.

This is a tested transport component, not a bound replacement display driver.
Kernel/DRM integration, board selection, safe sequencing and physical rendering
remain open. Evidence, disassembly, stock-function harness and Ghidra project are
preserved under `output/pico-stock-driver-analysis-20261011-v1/`.

## RTT is diagnostic

The missing `CONFIG_PICO_RTT` component is distinct from tracking. Its probe at
`0xffffff80081b9f64` allocates a 64 KiB ring per CPU and registers scheduler,
IRQ and panic callbacks. The callbacks store timestamps and task/IRQ identifiers
in 16-byte records; panic handling flushes the rings for minidumps. This evidence
places RTT in crash diagnostics, rather than the essential sensor/pose data path.
It can be restored later without treating it as an IMU driver. Other boot and
physical-driver acceptance gates are unchanged.
