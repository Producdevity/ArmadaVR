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
