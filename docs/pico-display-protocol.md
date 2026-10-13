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

The tool also accepts an explicit `--display` primary DSI node and
`--expected-default-panel` path. Together these change that display's default
to the selected panel. It validates both panel phandles, their common SDE
controller and the previous selection; a stale expectation fails before output
creation. Only the default-panel property is added to the allowed edits.

October 12 read-only observations narrow the connected Pro Eye's selection:
Android reports boot DTB index 3 and overlay index 0, matching the retained
Kona v2.1 candidate. SurfaceFlinger identifies its primary display as
`sharp 493 120`, with a 1920 by 3664 buffer and reported 72 Hz refresh while
powered off. The Sharp120 tree node supports 120/90/72 Hz dynamic refresh, so
72 Hz does not identify the separate Sharp72 node. A persistent property instead
says `jdi493120`; it is not sufficient to override the compositor's identity.

The merged stock tree defaults to JDI 4K 55UHD72. The published driver's
`msm_drm.dsi_display0=<node>:<config>` override selects a panel by name; without
it, the driver follows `qcom,dsi-default-panel`. Thus the earlier generic Linux
command line cannot rely on that stock default. An offline Sharp120 candidate
now adds mux ownership and explicitly replaces the default, changing exactly
three properties with memory, reservations and all other properties intact.
Its SHA-256 is
`11437f408e63ede811db1bb380d4d62c5d91cac451f7ecd9f9ad0120afef9781`.
Evidence is in `output/pico-panel-selection-20261012-v1/`.

This correlates stock userspace identity with a candidate panel; the live FDT,
physical GPIO state and replacement display operation remain unverified.
The candidate is a merged tree. Installing it as a base and applying the stock
overlay again would overwrite the selection.

[`bind-pico-overlay.py`](../tools/bind-pico-overlay.py) carries the same changes
into the stock overlay instead. It requires explicit hashes of both the base
and overlay, and the same panel, bridge, display and previous-default paths.
Each changed node must belong to exactly one overlay fragment. The new bridge
phandle uses overlay-local numbering; the added panel reference includes its
local relocation. The existing default-panel relocation is preserved. Applying
the result must match the complete directly bound tree, including memory and
boot CPU metadata. Unrelated overlay properties and both inputs remain intact.

The authenticated base3/overlay0 pair passes this transformation. The new overlay
has SHA-256 `d0a87072661435731896f78e17b5eed0b5a3fe634de53bb9665423d84d999b2e`.
Its merged properties exactly match the Sharp120 candidate above; its binary
hash differs because libfdt orders the edited properties differently. Synthetic
compiled overlays also pass relocation at three different base phandle ranges;
ambiguous targets, wrong hashes and missing relocation data are refused.
This produces an offline overlay candidate, not verified bootloader acceptance.

[`repack-dtbo.py`](../tools/repack-dtbo.py) replaces one explicitly hashed entry
in a contiguous Android v0 overlay table. It preserves the entry order,
identifiers and every other overlay byte, and requires an explicit output size
limit. Data after the original table, including AVB metadata and partition
padding, is omitted: the result is unsigned. A same-payload replacement exactly
reproduces the original table, and growth/shrink tests cover every entry position.

The b1977 table now packages the modified overlay at index0; all other 15
overlays match stock. Its 4,292,934-byte unsigned container has SHA-256
`882ccd3b646e68628286a14250786d11aa106246967fce7a31f2340c8ba469f3`.
The paired Android v2 container keeps all seven original boot DTBs and includes
the physical kernel and GPU-bearing initramfs. Unpacking both containers and
applying the stock-reported base3/overlay0 pair reproduces the intended Sharp120
tree. The new boot container is 62,164,992 bytes, SHA-256
`13b69e7d09b10f341258ac35d733fb1f21946ebbdfb136528c7de569d851cd49`.
The pair has no root filesystem or verified custom-boot/recovery acceptance.
Its receipt is `output/pico-panel-selection-20261012-v1/container-pair.json`;
no image was sent to a headset.

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
symbols. The patch applies without fuzz. The earlier Image hash above identifies
the preserved `0025` build; the complete build through `0027` is recorded below.
Startup-failure evidence is in
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
and unchanged imports against the accepted kernel. Physical validation remains
open. Shutdown-failure evidence is in
`output/pico-panel-shutdown-20261011-v1/`.

The full Linux-userspace kernel through `0027` subsequently compiled in 558.7
seconds using the pinned compiler and a 4 GiB memory limit. All 18 output hashes,
38 module signatures, 2,715 imported symbol versions and dependency closure
pass verification. A modified signed payload is rejected. The exact Image
boots and shuts down the isolated diskless Android ABI fixture in 5.56 seconds.
Its SHA-256 is
`ae8d5a0d2ad579fd30da2ce47786208cb45187b9aab3690fa9a7c5c6207c9823`;
the build receipt SHA-256 is
`77d0fac6fa723c64b8ec60a642c20b86b9cb3a609ad3bf4f98ab18afd99230ed`.
Artifacts are in `output/kernel/pico-neo3/linux-panel-errors-20261011-v1/`, with
acceptance evidence in `output/pico-panel-errors-build-20261011-v1/`. No headset
was booted with this Image. Panel selection, GPIO behavior and display/power
acceptance remain physical requirements.

Patch `0028-panel-probe-cleanup.patch` closes resource leaks during panel
initialization. Failure to acquire the charge regulator group now releases the
main group; later failures release both groups. Failed pinctrl state lookup
releases its acquired handle and clears the failed state. Null pinctrl results
return an error instead of reporting success. Deferred mux and backlight probes
can retry without accumulating handles. The successful acquisition sequence and
panel power operations are unchanged.

The maintained regression applies the patch to retained upstream lifecycle
routines and checks 4,992 injected failures, including 64 repeated retries per
scenario, optional mux bindings, external bridge mode and subsequent successful
initialization. The old routines produce 4,224 cleanup failures; the patched
routines produce none under macOS and ARM64 Linux sanitizers. GNU patch applies
it without fuzz to the accepted full source. The complete panel object compiles
with `-Werror` and unchanged undefined symbols. Evidence is preserved in
`output/pico-display-lifecycle-20261011-v1/`.

Patch `0029-panel-vio-cleanup.patch` fixes the lower-level GPIO acquisition
failure: when the backlight pin cannot be requested, the previously acquired
VIO pin is released before the earlier pins. All 128 optional-pin combinations
are exercised with three injected errors and repeated retries. The original
routine leaks VIO in 3,072 of 43,008 cases; the patched routine leaks none.
The optional panel-test pin retains its existing nonfatal failure policy.
Host and ARM64 Linux sanitizers, GNU patch application and full panel-object
compilation with `-Werror` pass. Evidence is in
`output/pico-panel-gpio-20261012-v1/`.

The complete kernel through `0029` passes verification of all 18 artifact hashes,
38 signed modules, 2,715 imported symbol versions and dependency closure. A
modified signed module is rejected. Its physical configuration, without added
QEMU transports, boots and shuts down the diskless ABI fixture in 5.791 seconds.
All 320 repository tests also pass. The final Image SHA-256 is
`f900a94ddb6de8273115b00f09bc2fd822b94476959e311696da2fb0e8f7107f`;
the build receipt SHA-256 is
`c30a2b0867f95eda74f8927b689aeec9df093caf071fce0634bc214ec54b1077`.
Artifacts are in `output/kernel/pico-neo3/linux-panel-gpio-20261012-v1/`, with
acceptance evidence in `output/pico-panel-gpio-20261012-v1/acceptance.json`.
This verifies build integration and virtual ABI behavior. Physical panel/mux
operation, custom boot and independent recovery remain unverified.

Patch `0030-panel-regulator-reference.patch` keeps a failed first regulator
enable from acquiring a software reference. Previously, the next attempt
skipped the hardware operation and reported success while the rails remained
off. A successful retry and nested references retain the existing behavior.
The regression applies the actual patch to the upstream routine and exercises
768 retry/reference sequences: the original produces 20,352 invariant failures,
the fixed routine none. macOS and ARM64 Linux sanitizer checks, full object
compilation with `-Werror` and all 321 repository tests pass. Evidence is in
`output/pico-panel-startup-20261012-v1/`.

This fix does not verify the lower helper's partial-enable rollback or disable
error handling. The vendor's disabled panel-reset rollback also remains under
review because its comment records a sleep-to-active shutdown regression.
Changing those paths requires checking the charge regulator and suspend
sequence together.

Patch `0034-panel-enable-load-rollback.patch` fixes one confirmed failure in
that lower helper. When `regulator_enable` fails, the rollback previously
skipped restoring the failing regulator's load request. It now enters the
existing load-reset step before voltage rollback. The original error and the
shutdown sequence for previously enabled rails are preserved. Successful
startup and shutdown are unchanged.

The exact-source regression covers 432 failed-enable/retry sequences across
one to eight rails, every failing rail position, three errors, voltage
capability and configured delays. The original leaves a stale load in all
432 cases; the patch leaves none. macOS and ARM64 Linux address/undefined
sanitizer checks pass. This does not establish successful physical rollback
when the regulator's cleanup operations themselves fail.

The complete kernel through `0034` builds with all 38 signed modules and
2,715 matching imported symbol versions. Signature corruption is rejected.
The Image SHA-256 is
`0c9d1c19e7ae046e6a18fc72376d39d5f0c0db487a580868352be0766f7b5c7a`.
Its physical configuration passes the diskless USB diagnostic and the real
FastRPC/ION ioctl tests, with normal shutdown. The virtual board has no Pico
panel or DSP. Build, source correspondence, module and VM evidence are retained
under `output/pico-panel-load-20261013-v1/`; physical acceptance remains open.

The complete kernel through `0030` passes all 18 artifact hashes, 38 module
signatures, 2,715 imported symbol versions, dependency closure and modified
signature rejection. Its exact physical configuration passes the diskless ABI
fixture and normal shutdown in 5.733 seconds. The Image SHA-256 is
`9d00bdde7875eafc6b887257cbe04212dda790e8a62cf84367c97d620f344be5`;
the build receipt SHA-256 is
`5d01dfcef0cf953f5727b23c5d4f82702bb08d21797f65eb351eea5dedcc01cb`.
Artifacts are in `output/kernel/pico-neo3/linux-panel-regulator-20261012-v1/`;
acceptance is recorded in
`output/pico-panel-startup-20261012-v1/acceptance.json`. Hardware acceptance
remains separate.

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
