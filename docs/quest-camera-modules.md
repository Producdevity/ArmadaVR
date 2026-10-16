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

## Peripheral configuration

`tools/quest-camera-config.py CONFIG --sha256 EXPECTED` inspects the selected
camera fields in the retained `pcl_config.bin`. It accepts Eureka configuration
and preserves field presence: omitted proto3 scalar values have schema defaults,
not measured hardware values. It reports camera identities, template references,
PHY mapping, nominal modes, metadata flags, camera sets and Syncboss frame-slot
defaults. It neither merges templates nor applies runtime overrides.

In build `52433670036000520`, the four tracking cameras map to PHYs 4, 1, 5 and 2,
respectively. The configuration explicitly selects embedded metadata rows for
these cameras. Glacier's nominal stream is 640 × 513 with an active array of
640 × 512; Canyon's nominal stream is 640 × 481. This establishes the stored
metadata mode but does not establish which row to remove or how its timestamp
relates to exposure. The Glacier calibration remains 1280 × 1024; rescaling its
intrinsics still requires a verified pixel-coordinate transform.

The tracking camera set stores a 66,666 µs synchronization period. Each of its
four generic frame slots defaults to 5,000 µs exposure and gain 48. These are
configuration defaults, not observed capture settings or a measured frame rate.
The mode tables' 60/120 FPS values cannot substitute for the MCU schedule.

The stock `libdevicesensors` reads ISP scaling from the first stream in each
camera's peripheral configuration. Near-zero or absent scaling defaults to one.
The CamX PHY builder derives the per-lane rate from pixel clock and bit depth
with a separate settings-manager margin. The offline encoder therefore still
requires an explicit verified lane rate. Sensor startup and stopping also use
Syncboss-controlled state beyond the module's register tables.

The inspector matches Google Protobuf using the firmware's embedded descriptors
for the original configuration and 256 mutated configurations. The original
protobuf parses and serializes byte for byte. Bounds, malformed tags and
varints, duplicate identities, invalid references, nonfinite numbers, unexpected
wire types and checksum failures are covered by host tests. Only the inspector
and synthetic tests belong in Git; the configuration, descriptors and analysis
remain private evidence.
