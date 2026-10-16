# Quest camera module inventory

`tools/quest-camera-module.py MODULE --sha256 EXPECTED` inspects a retained camera
module without accessing hardware. It accepts the compact QTI 3.0.1 container
used by the authenticated Quest firmware build `52433670036000520`. Pico's padded
version 2 parser cannot decode this format.

The partial schema covers camera IDs, bus and PHY parameters, power sequences,
resolution groups, stream geometry, initialization and stream register tables.
It checks section bounds, symbol identities and references, exact schema sizes,
field widths and the supplied checksum. It does not generate a capture plan.

The six retained modules report these nominal settings:

| IDs | Sensor | Stream dimensions | Format | FPS | D-PHY lanes | Settle count |
| --- | --- | --- | --- | --- | --- | --- |
| 0, 1 | OG01A1B | 640 × 513 | RAW10 | 120 | 1 | 17 |
| 2, 3 | OV7251 | 640 × 481 | RAW10 | 60 | 1 | 23 |
| 4, 5 | IMX471 | 2328 × 1748 | RAW10 | 90 | 4 | 26 |

These are file contents, not measured active modes. The stock camera scheduling
configuration assigns IDs 0–3 to world tracking and IDs 4–5 to color. The retained
tracking calibration uses 1280 × 1024 and 640 × 480, respectively. Binning,
cropping, metadata rows, clock alignment and runtime mode overrides must be
resolved before applying that calibration to captured pixels.

All six module mode tables are empty. Their initialization, stream-on and
stream-off tables each contain the same single `0x0100 = 1` write. They therefore
do not establish a complete sensor startup or shutdown sequence. Stock firmware
control and the native kernel's actual stop semantics remain separate work.

Verification compares every reported field with the original ARM64 parser in
`camera.qcom.so` (SHA-256
`2bac5ae49ae3de1acbc047e2a3c31f2442257244c5973e20faf3a991a81c70f1`).
The six original modules and 64 synthetic variants match, including nonempty
per-mode register tables. The original parsing instructions execute in an
isolated container with allocator and string-copy shims; no camera is opened.
Unit tests cover malformed sections, references, optional values, every
truncated prefix, nonfinite frame rates and checksum refusal. Firmware binaries,
private analysis and machine-specific evidence remain outside Git.
