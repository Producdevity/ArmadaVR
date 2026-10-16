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

## Embedded tracking-camera metadata

`quest_camera_metadata_decode` decodes the OG01A1B and OV7251 embedded row
formats from authenticated build `52433670036000520`. It supports the stock
8-bit and packed-10 representations, with bounded lengths, unaligned input and
unchanged output on failure. It returns exposure duration, encoded gain, frame
counter, matching words and tags. It refuses values that would wrap the stock
16-bit fields. Parameters are explicit: the three OG01A1B words come from the
MCU metadata-matching response; OV7251 uses its first MCU word and the device
descriptor's gain multiplier. Nominal camera-module timings cannot substitute
for that active response. Matching words and the derived span retain their
stock representation; their relation to physical readout remains unverified.

All returned fields match the original isolated ARM64 routines across 24,000
synthetic cases (two sensors, two row formats). Guard-page truncation tests and
address/undefined-behavior sanitizers cover bounds and arithmetic rejection.
The original library is `libsyncboss.so`, SHA-256
`a3164d43adb9de5eddb9fa38fd221701a6c4bbf3faa12a216a462b4831555c57`,
with decoder entries `0x139d0` and `0x1478c`. No constructors, service startup or
device access are used by the comparison. Firmware and private analysis remain
under `output/quest-offline-parity-20261015-v1/`.

These rows do not contain an independently usable capture timestamp. Stock
code matches their tags to recent MCU shutter packets, then separately applies
timestamp adjustments. Receiving those packets, handling ambiguity and counter
wrap, verifying exposure-center time, and mapping the active image geometry to
calibration are still required. Row-format detection is not authorization to
interpret an entire image as that pixel format.

`quest_camera_shutter_decode` reads packet-254 payloads for tracking types 6,
21 and 22. It selects one explicit camera bit and handles the stock per-camera
exposure/gain overrides in ascending mask-bit order. It requires the exact
89-byte base plus declared four-byte overrides, rejecting truncated packets,
unknown types and orphan overrides. Peripheral metadata is left uninterpreted.
The raw timestamp, SOF timestamp, counter and tags remain separate; neither
NSYNC nor exposure adjustment is inferred. This parser opens no stream and
performs no matching.

The isolated stock packet handler at `0x28b60` agrees with every decoded field
across 2,000 synthetic packets and 6,031 camera records. Only its clock and mutex
helpers are replaced with inert test stubs; no device service is initialized.
The stock handler preserves packet timestamps and separately records SOF plus
NSYNC and host arrival time. Types 6 and 22 use row tags for matching, while type
21 may match without a row tag. A live receiver must therefore reject stale or
ambiguous matches, rather than assuming a decoded packet belongs to an image.

## CPU pixel conversion

`quest_camera_pixels_mono8` produces contiguous mono8 pixels from explicitly
configured mono8 or MIPI RAW10 rows. The caller selects the active dimensions,
source stride, whether to skip one leading metadata row, and the conversion
curve. Input/output overlap, truncation, invalid dimensions and incompatible
formats are refused before writing. It neither maps DMA buffers nor chooses
camera geometry or calibration. Already-compressed mono8 accepts copying only.

For RAW10, copying discards the two low bits; the two supported point-5 curves
reproduce the stock CPU approximations, including their low-intensity behavior.
They are not interchangeable with applying a mathematical square root. The
active curve must be established before capture; a stored gamma value alone
does not select it. Source padding and a leading metadata row are excluded
from the resulting image only as explicitly specified by the caller.

The original ARM64 SIMD and scalar reference functions from
`libgammacorrection.so`, SHA-256
`fd07d092e8d408de585a5cdb6cb58f92968aee582d3f541da4a9a8fd582dda6f`,
agree byte-for-byte with the converter across 612 synthetic images and 492,544
pixels. This includes every 10-bit value in all four MIPI group positions,
multiple image rows, source padding and explicit metadata-row exclusion.
Guard-page and sanitizer tests check memory bounds and failure preservation.
No firmware initialization, capture, physical calibration or target performance
claim follows from these tests.

### Command packets and RDI buffers

`quest-camera-packet.c` encodes the vendor packet ABI with explicit command
allocations, one optional image, and a separate kernel-writable command tail.
Commands sharing an allocation must have identical allocation sizes and disjoint
capacity ranges. Image and command handles cannot alias. Sensor NOP packets are
72 bytes: the sensor parser rejects allocations no larger than its 64-byte
`cam_packet` structure even when no payload is used.

The RDI encoder supports MIPI RAW8/RAW10 and PLAIN16_10 on ports 0 through 3.
It requires 16-byte row alignment and enough memory for all padded rows. PLAIN8
and PLAIN16_8 are excluded because the retained VFE RDI parser treats them
differently or does not support them. No command registers, clock rates, resource
ownership, or physical mode are inferred by these encoders.

`tools/test-quest-camera-packet.py` authenticates the pinned kernel archive,
compares packets against its UAPI, and executes its unchanged packet-validation
and kernel-buffer-selection functions with modeled memory allocations. It tests
allocation failures, descriptor boundaries and writable-tail placement under
ASan/UBSan. This verifies serialization and parser contracts, not hardware capture
or the complete sensor/PHY/ISP startup sequence.

`quest-camera-blob.c` adds single-ISP clock, CSID clock, single-frame HFR and
bandwidth-v2 payloads for one selected RDI port. It preserves explicit 64-bit
rates and does not derive hardware votes from nominal sensor resolution. The
native packet test compares every field against the pinned vendor structures
and passes concatenated blobs through the original generic-command parser,
including truncated commands and handler failures. The complete hardware
initialization still needs mode-specific register commands and validated votes.

### Active metadata parameters

`quest-camera-mcu.c` builds the metadata-matching query for one representative
camera per hardware type and decodes the fixed 60-byte response. The request is
Syncboss command 2, subcommand `0x4e`, with a count and five mask slots. Response
words are stored in three columns of five entries, not consecutive triples.
The implementation requires distinct one-hot camera IDs and an already allocated
sequence in the kernel's 1..254 range. The control owner must validate the response
type, sequence and MCU generation before decoding its payload.

The decoder retains raw words, including zero values. The sensor-specific row
parser separately validates timing parameters. Mode changes and MCU restarts
invalidate the relationship; there is no fallback to nominal module values.
The maintained encoder/decoder matches 1,000 isolated calls through the original
metadata-fetch routine with synthetic responses. Another 40,734 sanitizer cases
cover sequences, camera masks, count limits, guarded payload truncations and
unchanged output on failure. No query was sent to a physical MCU.

`quest-mcu-control.c` supplies sequence allocation/release, one-attempt packet
writes and bounded control-reply reads using explicit, already owned descriptors.
The separate control FIFO returns one copied record per read, unlike the stream
FIFO's batch interface. Replies must match type, sequence and exact payload size;
driver messages invalidate the channel, and foreign sequences are ignored only
after complete record validation. A failed or short write is never retried.
These primitives do not open or enable the MCU. The transaction owner below
adds deadlines and generation checks; node ownership and hardware startup/
shutdown ordering remain external.

The native camera-control test checks the sequence ioctls against the pinned
Syncboss header and exercises the metadata query through modeled writes and
v2/v3 replies. Failure tests preserve sequence ownership where release failed
and prevent malformed replies from overwriting caller output.

The camera request transport also provides the Quest V3 link operation with an
explicit streaming/triggered mode and independent-CRM flag. The current stock
CSL library uses this 1,040-byte layout. The maintained API bounds each link to
eight owned devices, clears unused entries and retains a positive returned
handle even on failure. A failed ioctl can still have created kernel state.

The V2 synchronization operation supports one to sixteen explicit link
descriptors (link handle, sensor ID and local/remote flag). The pinned kernel
accepts those fields but does not use the master, sensor ID or remote flag in
its synchronization implementation. This operation therefore does not establish
MCU frame synchronization or select the physical master. No remote sensor is
acquired by these additions.

ARM64 tests against the pinned vendor headers pass 1,912 cases, including all
four supported trigger/independent combinations, all link counts, every master
position, interrupted and failed ioctls, and preserved acquisition handles. The
remaining camera startup owner must select the actual topology and coordinate
MCU streaming with the ISP and PHY lifecycle before physical capture is usable.

`quest_camera_shutter_select` now applies the stock OG01A1B/OV7251 matching rules
within an explicit raw-timestamp interval for the current generation. Type 6
checks the row tag, type 22 also checks the auxiliary tag, and type 21 requires
only the camera mask unless the device's global auxiliary policy adds a check.
The selector requires exactly one fresh match, excludes already consumed
packet timestamps and leaves output untouched on ambiguity or malformed input.
The owner must supply the verified interval and advance its consumption floor;
this helper does not establish the image clock or exposure-center relationship.

The tag predicates match 12,288 isolated calls through the current firmware's
original ARM64 matcher, including all byte tags and both sensor types. Another
6,188 sanitizer cases cover ambiguity, generation isolation, staleness, replay
floors, exact interval boundaries and arithmetic extremes. Queue position and
the wrapping embedded row counter are not used as frame identity.

### MCU transaction ownership

`quest-mcu-transaction.c` owns one allocation/send/reply/release sequence on
explicitly borrowed command and nonblocking control descriptors. It copies the
request before starting, checks an explicit deadline and the current MCU
generation before each read, and delivers the reply only after sequence release
succeeds. Complete foreign-sequence records are ignored without extending the
deadline. Receipt of a response does not establish firmware success: the camera
command owner must still interpret the returned status.

Timeouts, restarts, clock regression, short writes, malformed replies and failed
sequence operations invalidate this generation once. No command or failed
release is retried, and uncertain owners cannot be destroyed normally. The
caller must stop other users, independently quiesce control I/O and close both
descriptors before discarding uncertain state. This does not imply that the
camera command failed to execute or that camera DMA has stopped.

Offline tests exercise the real transport with modeled syscalls: all 254 usable
sequences, both header versions, copied request storage, exact reply boundaries,
foreign records, silent deadlines and failures at every command stage.
