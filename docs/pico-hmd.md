# Pico HMD integration

The native Monado Pico driver can expose an HMD backed by the four-subscription
SSC head session. It reports orientation and angular velocity. Position, yaw
calibration, cameras, eye tracking and proximity sensing are not implemented.
No HMD is opened by default.

Set both `PICO_HMD_CONFIG` to a private calibration JSON file and
`PICO_HMD_COUNTER=1` to opt in. The counter option acknowledges that the CPU
counter and SSC timestamp origins must have been verified for the device.
It does not establish that relationship. Controller configuration remains
separate through `PICO_SSC_CONFIG` and `PICO_SSC_COUNTER`.

The HMD owns its sensor session. Destruction stops subscriptions, drains their
callbacks and joins the worker before freeing optics or tracking state. Stale,
missing or rejected sensor data produces an invalid orientation rather than a
stationary tracked pose. Queries use the requested monotonic display timestamp:
exact samples and interpolation come from a 4,096-entry history; forward
prediction uses calibrated gyro velocity and is limited to 50 ms, including
timestamp uncertainty. Requests before retained history, beyond that horizon
or with stale samples return an invalid relation. Stream restarts clear history.
Angular velocity is expressed in tracking-origin coordinates, as Monado requires.
This bounds extrapolation; it does not establish physical tracking accuracy.

## Calibration file

Version 1 uses RGB grids. Version 2 uses the analytic lens model described below.
Both versions require the common fields below. There are no physical defaults. The file
must be a regular file, at most 2 MiB; final symlinks, embedded NULs, trailing
JSON, duplicate required fields and malformed numbers are refused. Units and
coordinates must describe the active sensor and optical configuration, including
the current IPD. This format does not authenticate the measurements.

| Field | Contents |
| --- | --- |
| `schema_version`, `device`, `serial` | `1` or `2`, `PICOA7H10`, the unit identifier |
| `screen` | `width`, `height` in native panel pixels; `frame_interval_ns` |
| `views` | Two objects, left eye then right eye, as described below |
| `imu.suids` | Four `[high, low]` pairs: accel, gyro, accel calibration, gyro calibration; each half is a 16-digit lowercase hexadecimal string |
| `imu.sample_rates` | Two advertised rates in Hz, accel then gyro |
| `imu.sensor_to_head` | Row-major 3×3 proper rotation into Monado head coordinates |
| `imu.acceleration_scale`, `imu.angular_velocity_scale` | Positive multipliers converting bias-corrected samples to m/s² and rad/s |
| `imu.max_age_ns`, `imu.max_gap_ns` | Maximum sample age and gap between paired captures |
| `imu.minimum_accuracy` | Minimum SSC accuracy, 1–3 |
| `imu.startup_timeout_ns`, `imu.report_timeout_ns`, `imu.bias_max_age_ns` | Subscription startup, corrected-output watchdog and bias freshness limits |

Each view requires `viewport: [x, y, width, height]` in native screen coordinates,
`display: [width, height]` for the recommended render image, and `rotation` as
four values in Monado's 2×2 matrix layout. Viewports must fit the screen and not
overlap. `fov` contains left, right, up and down angles in radians; `eye_pose`
contains quaternion x/y/z/w followed by position x/y/z in metres, relative to
the head origin.

For version 1, `grid_width` and `grid_height` each range from 2 to 65. `distortion` contains
exactly their product in row-major order. Every entry is `[red_u, red_v,
green_u, green_v, blue_u, blue_v]`: render-texture coordinates sampled for that
normalized per-eye screen position. The driver interpolates each color channel
bilinearly. It copies the grids before the configuration is released. It does
not substitute an undistorted mesh or infer lens parameters from resolution.

The sensor session requires four distinct SUIDs and display rigid-body metadata
for both raw/calibration pairs. Unit files captured from Android do not by
themselves prove active native SUIDs, coordinate conventions or optical
calibration. No physical calibration file is distributed with the project.

## Offline coverage

Tests use the pinned Monado helpers and synthetic optical data. Native sanitizer
checks exercise 2×2 through 65×65 distortion grids, rotated viewports, independent
RGB sampling, invalid inputs, loss of orientation validity, copied calibration
ownership and repeated session teardown. Separate parser and prober tests cover
required calibration fields and explicit activation. These checks do not prove
physical optical correctness, tracking accuracy, scanout or recoverable boot.
Pose-time tests cover exact queries, interpolation, all three prediction axes,
sensor rotation and scaling, tracking-origin velocity, uncertainty limits,
stale data, stream changes and history wraparound.

The diskless Pico-kernel integration test runs the installed Monado service,
OpenXR loader and application over actual QRTR sockets with a synthetic SSC
server. It selects the Pico HMD and both controllers together, checks each
hand's button and touch transitions, both eye poses and FOVs, changing fused
orientation, distinct poses for queries 20 ms apart, angular velocity and refusal
of a distant future query. Orientation becomes invalid after reports stop. Controller input remains
available after head tracking is lost. Two fresh service starts must exit
cleanly. The test uses the null compositor and synthetic calibration; it does
not validate display output, optics, controller poses or positional tracking.

A separate rendering test uses `tests/pico-hmd-render.cpp` and
`tests/pico-hmd-render-vm.py` with the production OpenXR renderer, Xvfb and
lavapipe. Four fresh service starts cover graphics and compute composition,
each with identity and independently altered RGB distortion grids. During
synthetic head rotation, left/right press and release must produce the expected
pixels in both eyes for at least four captures spanning 1.5 seconds per phase.
The service runs as an unprivileged user, matching the target session and
avoiding real-time scheduling starvation in CPU emulation. This proves the
calibration-to-framebuffer path with software Vulkan, not Adreno execution or
physical panel scanout. The latest prediction-enabled run is retained under
`output/pico-pose-time-20261014-v1/render-vm-v4/`: four services, 16 stable phases
and 6,596 submitted projection frames. Its synthetic panel interval is 90 Hz;
this is not a measured frame rate. The VM uses four emulated CPUs, paired
100 Hz IMU samples, 10 Hz calibration refresh and alternating controller reports
at 20 Hz. Missed synthetic capture intervals are dropped rather than replayed
with stale timestamps. Late poses remain invalid under the production limits.
The separate two-service null-compositor run is in `runtime-vm-v5/`; each fresh
service resets the prior malformed controller stimulus before startup.

## Stock optical data

`tools/inspect-pico-optics.py` reads an explicitly selected, saved
`A7H10_0.ini` or `A7H10_1.ini` and `axisOffset.txt`. Both files require their
independently retained SHA-256. The output is private metadata, created without
replacing an existing file; it is not a runtime HMD configuration.

Analysis of the stock 5.8.4.0 HMD service establishes these contracts:

- The display configuration depends on product and glasses-holder selection.
  Its fallback to `A7H10_0.ini` does not prove that file is correct for a unit.
- The lens record contains an equation selector, 20 coefficient slots and four
  chromatic parameters. Slots 14–17 are overwritten with the four adjusted lens
  offsets on the FalconCV3 path. They must not be interpreted as 20 radial
  polynomial coefficients.
- Pro Eye consumes the first offset row. The non-eye Pro requires three rows:
  IPD state 3 selects the first, state 2 the second, and state 1 the third.
  The inspector rejects the wrong row count rather than substituting a model.
- The service applies IPD correction, four per-axis property corrections and
  clamping. It also adjusts lens separation for screen gap and permits a FOV
  override. The inspector preserves raw values and does not invent those inputs.

The saved ARM64 `libpxrcompositor.so` from stock `XRRuntime.apk` consumes the
lens record. For equation type 2, slots 0–10 define
`S(r²) = k0 + k1*r² + ... + k10*(r²)^10`. Given lens-relative coordinates
`(x, y)`, green tangents are `(x*S, y*S)`. Red multiplies that pair by
`1 + c0 + c1*r² + k18*r`; blue uses `1 + c2 + c3*r² + k19*r`.
Here `r = sqrt(x²+y²)` and `c0`–`c3` are the four chromatic parameters.
Slots 18 and 19 therefore carry additional chromatic terms.

The inspector accepts repeated `--sample-tangent X Y` arguments to evaluate
this equation. Samples are lens-relative tangents, not normalized texture UVs.
The scalar radial implementation was compared with 26,637 executions of the
saved stock ARM64 routine: both A7H10 profiles and each polynomial term in
isolation. Maximum error, divided by `max(1, abs(stock_result))`, was below
`2.8e-7`. The stock logger was stubbed in an isolated mapped copy; no Android
service or device was started. The RGB mesh and texture projection have additional offline comparisons below;
none of these tests establishes physical optical alignment.

The active configuration, screen geometry, IPD/property overrides, panel/view
rotation and physical alignment still need verification before installing
physical RGB grids. Camera calibration XML describes a
separate camera system; its coefficients are not display-lens calibration.


## Profile selection inspection

`--selection-model FILE --selection-sha256 SHA256` checks a supplied profile
against an explicit saved selection snapshot. The JSON contains exactly:

- `schema_version`: 1.
- `product_property`: `A7H10`, or an empty string for the stock default.
- `glasses_property`: `0`, `1`, or an empty string.
- `holder_file`: the saved holder file contents, or `null` if unavailable.
- `available_profiles`: a unique list containing zero, one or both supported
  filenames, `A7H10_0.ini` and `A7H10_1.ini`.

The glasses property takes precedence over the holder file. A missing or empty
holder with no property selects holder type 0. Trailing spaces, tabs, CR and LF
are removed from a numeric holder. Whitespace-only and unsupported contents are
refused: stock can use a whitespace-only holder literally in a filename.

If the requested file is absent, stock attempts `A7H10_0.ini` while retaining the
requested holder type. The report therefore keeps the candidate, selected file,
holder source, fallback flag and selected-file availability separately. The CLI
refuses a supplied configuration that differs from the selected filename or is
absent from the snapshot. This does not establish the snapshot's freshness or
the active headset selection; verification and runtime-usable flags remain false.

The original ARM64 selector was run through the point of INI loading for 240
synthetic snapshots. All 216 supported cases match; the other 24 contain
whitespace-only holders and are intentionally refused. File access, property
writes and short-string operations were isolated in the test process.

## IPD and axis adjustments

`--runtime-settings FILE --runtime-sha256 SHA256` resolves the FalconCV3 lens
separation and axis adjustments from explicit saved inputs. Both options are
required together. Version 1 of the settings JSON has exactly these fields:

| Field | Contents |
| --- | --- |
| `schema_version` | `1` |
| `ipd_enabled`, `ipd_correction_enabled` | Explicit booleans for the stock enable properties |
| `ipd_correction_mm` | Resolved stock IPD lookup result, in millimetres |
| `screen_gap_m` | Screen gap in metres, subtracted from lens separation |
| `axis_corrections_mm` | Four property corrections in left X/Y, right X/Y order |

Pro Eye starts from 63.4 mm regardless of IPD state. Pro with IPD enabled uses
57.4, 63.4 or 69.4 mm for states 1, 2 and 3; otherwise it keeps the selected
lens file's separation. The screen gap is then subtracted. IPD axis correction
is used only for Pro states 2 and 3 when correction is enabled: subtract it
from left X and add it to right X. Four independent property corrections follow,
then each axis is clamped to the stock range of −1 to +1 mm.

The resolver preserves binary32 rounding after each operation. Its report keeps
raw offsets, adjusted values before clamping, the clamped axis indices and a
separate resolved lens record with coefficient slots 14–17 replaced. Supplying
a screen model additionally requires its separation and offsets to agree with
that result. It does not choose active settings or produce an installable
calibration file.

Version 2 requires the same fields plus `fov_override_degrees`, the explicit
numeric value of `persist.pvrservice.lens.fov`. The stock service converts this
value to binary32 and accepts it only when strictly greater than 60 and less
than 160 degrees. Otherwise it keeps the lens file's FOV. A missing value is
not replaced with zero. Version 1 continues to leave the FOV unresolved.

The override is read when stock first loads the lens record and remains cached
on later calls. The inspector resolves a fresh load; it does not emulate a
running service's history. The scalar lens FOV does not establish the two render
projection matrices, which remain explicit screen-model inputs. Across 136
synthetic stock calls and 136 subsequent cached calls, the original ARM64 code
confirms the bounds, float rounding and cached behavior. Non-finite property
values ignored by stock are deliberately refused by the inspector.

An isolated ARM64 comparison executes the saved stock HMD service's original
cached `getLensInfo` and `getIPDLensAxisOffset` routines with synthetic model,
file and property inputs. All 9,600 output components across 1,920 cases agree
bit-for-bit, including both models, all states, enable combinations, positive
and negative corrections, screen gaps and clamping. The resolved IPD lookup
result is supplied at the test boundary. This does not verify that lookup,
initial profile selection, active hardware settings or physical alignment.
Evidence is retained under `output/pico-optical-settings-20261014-v1/`.


## IPD table inspection

To inspect an explicitly selected saved `IpdOffsetList*.txt`, supply all five
options: `--ipd-table FILE --ipd-table-sha256 SHA256 --ipd-sample INTEGER
--ipd-offset INTEGER --ipd-range-limit INTEGER`. Sample and offset are stock
integer sensor values; the limit must already be resolved for the current IPD
state and table type. The inspector does not select a fallback table or assume
that a saved file was active.

Stock uses the fifth column for sensor positions and the second for axis
correction. At the first inclusive bracketing interval, it returns the fraction
through that interval multiplied by the difference between its correction
values. It does **not** add the starting correction. Pro Eye, an absolute
hardware offset greater than the limit, or a sample outside the table returns
zero. Binary32 rounding is preserved throughout. The original ARM64 routine
agrees exactly across 5,520 synthetic cases, including shared endpoints and
both sides of the hardware-offset gate.

The parser requires 2–200 complete five-column rows, finite bounded values and
strictly increasing sensor positions after float conversion. It refuses malformed
stock files instead of reproducing their undefined behavior. The report retains
the parsed rows and selected interval. When runtime settings are also supplied,
their correction must agree with the table result. `ipd_lookup_verified` then
means agreement with that explicit table and sample, while active-setting and
table-selection verification remain false.

## Compositor axis inspection

`--compositor-axes FILE --compositor-axes-sha256 SHA256` resolves the stock
config-service axis stage after `--runtime-settings`. Its JSON contains exactly
`schema_version: 1`, `hwc_enabled` (boolean), `hwc_reverse` (0 or 180), and
`sdk_axis_overrides_mm` (four values in left-X, left-Y, right-X, right-Y order).
Each override is a finite value within ±1 mm or `null` for an absent SDK override.
An explicit zero replaces the service value; it does not mean absent. Stock
permits larger overrides, but this inspector refuses them because the current
screen model supports only that range.

When this stage runs, overrides replace its incoming axis values. If HWC is enabled and reversal
is 180 degrees, the compositor swaps the two eyes and negates both axes. This
happens before mesh generation, whose own reversal remains necessary. A later
successful VR-service lens reload replaces these offsets; see startup selection
below. The
inspector preserves the service values separately, returns the intermediate
and final offsets, and requires any supplied screen model to match the final
offsets and effective HWC orientation. It does not apply this optional stage
implicitly: the caller must establish that the config-service path is active.
Other SDK lens, geometry and projection overrides remain separate inputs.

The original ARM64 config-service function was executed with synthetic settings
for 3,072 cases covering all override masks, enabled/disabled HWC and both
orientations. All 12,288 output components, including signed zero, match exactly.
The test isolates the config lookup and two Android logging functions. It establishes the axis transformation, not the complete SDK settings
path or the headset's active configuration. Outputs remain inspection-only.

## SDK lens and frustum inspection

`--sdk-geometry FILE --sdk-geometry-sha256 SHA256` inspects explicit SDK overrides
on an explicitly supplied base lens. This stage alone does not establish startup
precedence. It requires `--runtime-settings` and
`--projection-model`; the original service lens and projection remain in the
report. The JSON has exactly these fields:

- `schema_version: 1`, `equation_type: 2`, `separate_distortion: false`,
  `disable_lens_correction: false`: the explicitly established effective modes.
- `lens_separation_m` and `meters_per_tan_angle`: nullable scalar overrides.
- `radial_coefficients`: eleven nullable coefficients, in stock index order.
- `chromatic_coefficients`: six nullable values. The first four replace the
  chromatic record; the last two replace the red/blue radius terms at lens
  coefficient indices 18 and 19.
- `left_frustum` and `right_frustum`: six nullable values each, ordered
  `[left, right, top, bottom, near, far]`.

`null` preserves a service value; zero replaces it. Values use the stock binary32
conversion. Non-finite values, invalid resolved frustums, unsupported modes and
unrecognized fields are refused. Schema 1 supports shared coefficients; disabled
lens correction is refused rather than mapped to a radial model. The screen model must match the final separation, tangent scale and
per-eye FOV. SDK axes remain the separate `--compositor-axes` input described
above. No active branch or physical settings are inferred from an absent input.

The original ARM64 config-service function matches all 63,488 float outputs
across 2,048 synthetic configurations, including absent values, single-field
changes and signed zero. A further 192 configurations run through the original
stock mesh and actual native HMD callback under sanitizers: 68,544 vertices and
411,264 RGB UV components pass the existing normalized tolerance. Maximum sampled
visible difference is 0.008273 pixels at a 1920-pixel render size. Deliberately
omitting the SDK stage makes 218,215 visible components differ by over half a
pixel. These are numerical tests with synthetic inputs, not active-unit optical
calibration, GPU scanout or flash-readiness evidence.

For separate-eye distortion, SDK schema 2 sets `separate_distortion: true` and
adds three required fields:

- `right_lens`: the explicit incoming right-eye record, containing a positive
  `meters_per_tan_angle` and eleven `radial_coefficients`.
- `right_meters_per_tan_angle`: a nullable SDK override.
- `right_radial_coefficients`: eleven nullable SDK overrides.

The right eye uses its own resolved tangent scale and radial coefficients.
Chromatic corrections remain shared, as in the original mesh. The matching
screen schema 2 requires `separate_distortion: true` and an explicit
`right_meters_per_tan_angle`; mode and scale mismatches are refused. Both eyes
produce the existing native per-eye analytic distortion format. No native
runtime binary or installed calibration changes are needed for this inspection.

Original SDK execution matches all 88,064 float values in 2,048 separate-eye
cases. A further 192 native/stock mesh comparisons cover 68,544 vertices and
411,264 RGB UV components. Maximum sampled visible difference is 0.001110 pixels
at 1920 pixels. A control that incorrectly reuses the left-eye model for the
right eye fails 167,014 visible components above half a pixel. These synthetic
checks establish the mapping, not which coefficients are active on a headset.

## Startup selection

`--startup-model FILE --startup-sha256 SHA256` adds an explicit startup snapshot.
Its JSON has `schema_version: 1`, `service_mode` (0, 1 or 2), and four booleans:
`config_service_enabled`, `use_service_parameters`, `config_file_compatible`,
`lens_query_succeeded`. These correspond to the branch state at initialization;
no current property values or successful service calls are assumed.

Stock reads config-service settings or a configuration file first. It then
queries VR-service display/lens information only when service mode is 1,
service parameters are enabled, and either config service is disabled or file
compatibility is enabled. A successful lens query replaces separation, tangent
scale, radial/chromatic coefficients and axis offsets. The projection frustums
remain from the earlier configuration. A failed query leaves the incoming lens
values intact. The inspector retains each stage, reports `effective_lens`, and
uses that lens with the final SDK projection for screen/grid checks. HWC mesh
reversal still applies after a service reload; the earlier SDK axis swap does
not survive that reload. Separate right-eye radial coefficients, tangent scale
and the separate-distortion mode survive; the inspector preserves them while
replacing the shared chromatic values and left-eye bank. Original-code tests
also check this preservation across all 864 startup cases.

The tool refuses a successful-query claim on a branch that never queries the
service, SDK overrides on a config-file branch, and missing service settings
when a service lens is required. Omitting the startup snapshot leaves the
individual stages as isolated inspections; it does not prove their values win
at initialization. Original ARM64 branch and lens-copy execution covers 864
cases across 27 branch/query combinations. Transport and unrelated setup are
isolated; headset startup and the active branch remain unverified.

## SDK display geometry and fixed IPD

`--sdk-display FILE --sdk-display-sha256 SHA256` accepts schema 1 with:

- `incoming`: explicit `width_pixels`, `height_pixels`, `width_m`, `height_m`,
  `screen_gap_m`, and `horizontal_offset_m`.
- `overrides`: the same six fields, each nullable to preserve its incoming value.
- `service_display`: `null`, or the independently saved service record containing
  `width_pixels`, `height_pixels`, `width_m`, and `screen_gap_m`.
- `ipd_mode`: `null`, 0 (dynamic), or 1 (fixed).
- `fixed_lens_separation_m`: a nullable positive separation.

The original SDK reader defaults absent IPD mode to 0 and absent fixed separation
to 0.046 metres; unlike the nullable display fields, these do not preserve their
previous values. Fixed mode selects that separate separation for mesh generation.
The underlying service/SDK lens record is retained. Screen resolution, physical
width, horizontal offset and selected separation must match the resolved values.
Physical height and screen gap remain in the report: the tested radial mesh uses
pixel aspect for height, and this stage does not subtract screen gap again.

When a startup branch can reload display information, startup schema 2 adds the
required boolean `display_query_succeeded`. It is independent of
`lens_query_succeeded`. Successful display reload replaces only the four fields
provided by that API; physical height, horizontal offset and fixed-IPD settings
survive. Missing service data or an unspecified query result is refused rather
than assumed. Original-code tests cover 2,048 SDK display settings and 864 startup
cases, including independent display/lens query results and right-bank retention.
These explicit snapshots do not establish current hardware settings.

A combined 192-case test includes 47 fixed-IPD cases and separate-eye optics.
All 68,544 vertices pass an independent native-equation check (maximum normalized
error 5.95e-8), and all sampled visible components pass the existing stock
comparison threshold. Maximum visible stock difference is 0.009195 pixels at
1920 pixels. One outside-texture component differs by 1.046e-5 normalized,
slightly above the unchanged 1e-5 stock threshold; the all-sample stock test
therefore remains a recorded failure. The native value is within 5.8e-9 of the
independent equation at that point. Neither production bounds nor test tolerances
were relaxed. Exact hexadecimal float transport avoids a separately diagnosed
decimal midpoint rounding difference between C and Python. These results do not
establish clipping equivalence, physical calibration or scanout.

## Display scaling inspection

`--display-scaling FILE --display-scaling-sha256 SHA256` resolves the separate
initialization block for stock `g`-prefixed display settings. Supply
`schema_version: 1` and explicit numeric values for `gFrustumDisplayWidth`,
`gFrustumDisplayHeight`, `gDisplayWidth`, `gDisplayHeight`, `gLensScale`,
`gLensScaleX`, `gLensScaleY`, `gWarpMeshMinX`, `gWarpMeshMaxX`, `gWarpMeshMinY`,
`gWarpMeshMaxY`, `gMeshDiscardUV`, and `gScreenWidthMM`.

If either lens scale is zero after binary32 conversion, both become `gLensScale`.
Only when all four dimensions are positive does stock replace X/Y scales and
symmetric mesh bounds with frustum/display ratios, raise the discard threshold
to at least the height ratio, and set `gScreenWidthMM` to twice `gDisplayWidth`.
Otherwise those size overrides remain inactive. The report retains the original
settings and identifies both branches.

This block does **not** update `xrWidthMeters`, the width used by the tested Pico
radial mesh. The inspector therefore reports these settings separately and does
not convert `gScreenWidthMM` into its screen model or apply these scales to the
Pico tangent mapping. The original initialization block matches 26,624 float
values across 2,048 synthetic cases; `xrWidthMeters` stays unchanged in every
case. This resolves the block's arithmetic and avoids mixing two distinct
geometry paths. Active settings, other mesh paths and physical dimensions still
require verification.

## Projection inspection

`--projection-model FILE --projection-sha256 SHA256` accepts a bounded JSON
object with `schema_version: 1`, `left` and `right`. Each eye contains six values
in `[left, right, top, bottom, near, far]` order, using the same length unit.
Horizontal and vertical bounds must straddle zero, with `0 < near < far`.
Degenerate float conversions, unsupported FOVs and non-finite values are refused.

The stock compositor converts each extent into an angle with `atan(extent/near)`.
The inspector preserves the binary32 input, division and result conventions and
returns `[left, right, up, down]` radians for each eye. A supplied screen model
must agree with these angles. It does not derive a symmetric projection from
the scalar lens FOV: stock permits independent eye frustums and application
FOV changes, so those settings must be established separately.

The original ARM64 `pxr_get_deviceinfo` function and its context-copy helper were
executed with 1,024 synthetic asymmetric stereo frustums. All 8,192 angle
components match the inspector bit-for-bit. The test resolves `atanf` to native
Linux libm; it does not compare the original Android Bionic implementation.
The saved Ghidra project and stock binary remain unchanged. These checks verify
conversion and ordering, not the projection matrices active on a headset.

A combined test carries resolved settings and per-eye frustums through the native
HMD callback for 192 configurations, including both model types, three IPD
states, both eyes and HWC reversal. Across 68,544 stock mesh vertices, the largest
visible difference is 0.001915 pixels at a 1920-pixel render size. Existing
normalized numerical tolerances pass. This is a sampled comparison with stock
floating-point calculations, not a continuous error bound or physical optical
calibration. Both saved profile files contain the same parsed lens values.

## Screen-to-texture inspection

The inspector also accepts `--screen-model FILE --screen-sha256 SHA256
--grid-size WIDTH HEIGHT`. These options must be supplied together. The bounded,
hash-bound JSON requires explicit resolved values; it does not derive them from
panel resolution, raw axis offsets or a fallback lens file.

| Screen model field | Meaning |
| --- | --- |
| `schema_version`, `layout` | `1` (shared) or `2` (separate eye), `side-by-side` |
| `width_pixels`, `height_pixels` | Combined logical stereo screen dimensions, before panel rotation; width must be even |
| `width_m` | Combined active screen width in metres |
| `lens_separation_m` | Resolved stock lens separation after IPD/screen-gap adjustments |
| `meters_per_tan_angle` | Resolved optical scale in metres per unit tangent |
| `horizontal_offset_m` | Stock horizontal screen offset in metres |
| `optical_offsets_mm` | Four already adjusted offsets: left X/Y, right X/Y, in millimetres |
| `hwc_reverse` | Effective stock HWC reversal, `0` or `180` degrees |
| `separate_distortion` | `false` for schema 1, `true` for schema 2 |
| `right_meters_per_tan_angle` | Required only in schema 2; resolved positive right-eye tangent scale |
| `fov` | Two explicit `[left, right, up, down]` render FOVs in radians |

For screen width `W`, optical scale `M`, lens separation `L`, and per-eye
pixel aspect ratio `A`, the stock pre-distortion tangents are
`x = (dx + (2*u-1)*W/4)/M` and `y = (dy + (2*v-1)*W/(4*A))/M`.
Here the stock mesh's `v` increases upward. The horizontal shift is
`L/2-W/4` for the left eye and its negative for the right eye, plus the common
horizontal offset and that eye's adjusted X offset. The vertical shift is the
negative adjusted Y offset. Stock HWC reversal negates both offsets and reverses
mesh placement; applying only one of these transformations is incorrect.

The exported grid uses top-left origins for both screen and texture coordinates,
as consumed by Monado's Vulkan compositor. After RGB distortion, texture
coordinates are `(x-tan(left))/(tan(right)-tan(left))` and
`(tan(up)-y)/(tan(up)-tan(down))`. This includes the vertical conversion from the
stock OpenGL mesh. Physical panel rotation remains a separate view transform;
HWC reversal does not establish a panel rotation or viewport arrangement.

The output contains two row-major RGB grids, in-texture vertex counts, and a
check against the grid driver's coordinate limit. Out-of-texture values are
preserved rather than clamped. The inspector does not perform stock triangle
clipping or certify interpolation accuracy. Output remains explicitly
`runtime_usable: false` and is not a complete HMD configuration.

Verification against saved stock 5.8.4.0 code covers 17,136 pre-clipping mesh
vertices: both A7H10 lens profiles, both eyes, three screen aspect ratios,
two lens-separation selections and HWC reversal. Maximum RGB tangent error,
normalized by `max(1, abs(stock))`, is below `1.8e-6`. The original mesh routine
runs in an isolated ARM64 process; allocation capture ends the call before
triangle clipping or GPU upload. Its input geometry is synthetic.

The saved stock vertex shader and fragment projection helper also run through
GLES transform feedback on llvmpipe, using those vertices and explicitly supplied
asymmetric FOV matrices. All 102,816 RGB UV components agree within `1.9e-7`
normalized error. This checks the projection arithmetic, not which texture
matrix Android selects on a physical headset. Evidence is retained under
`output/pico-optical-mapping-20261014-v1/`.


`--render-size WIDTH HEIGHT --max-error-pixels LIMIT` adds an interpolation
audit. It compares the grid's bilinear interpolation with the analytic mapping
at nine interior points per cell, separately for each color. A point is included
if either the exact or interpolated coordinate lands inside the render texture.
The report records the worst per-axis pixel error and its location; an empty
visible sample set cannot pass. This is sampled evidence, not a continuous
error bound or a physical accuracy measurement.

For the saved A7H10 profiles with explicitly synthetic 3664×1920 geometry,
17×17, 33×33 and 65×65 grids all exceed a 0.5-pixel limit at 1832×1920 render
resolution. The largest grid reaches approximately four pixels of error, and
some corner coordinates exceed the current grid driver's limit. These grids
must not be treated as accepted calibration or repaired by silently clamping
coordinates. The analytic path below avoids this driver-side interpolation.


## Analytic distortion

Schema version 2 replaces each view's `grid_width`, `grid_height` and
`distortion` fields with `analytic_distortion`. Mixing the two representations
is rejected. Common view geometry, eye pose, FOV and all IMU fields remain
required. The model object contains:

| Field | Contents |
| --- | --- |
| `type` | `pico-radial-2` |
| `tangent_scale`, `tangent_offset` | Two floats each; `x = scale[0]*u + offset[0]`, `y = scale[1]*v + offset[1]`, with top-left screen input |
| `radial_coefficients` | Eleven coefficients, constant term first |
| `chromatic_coefficients` | Six values: stock c0, c1, c2, c3, k18, k19 |

The offline inspector includes this model in each generated view. Its inputs
still require independent active-setting and physical validation; emitting a
model does not authorize installing it. The native driver copies the model,
evaluates RGB tangents directly in double precision and projects through the
view's explicit FOV. It does not interpolate an input grid or clamp coordinates.
Creation rejects ambiguous models, non-finite parameters, degenerate scales and
models whose conservative output bound exceeds one million texture units.
Out-of-texture coordinates are valid distortion results, not tracked pixels.

The actual native `xrt_device` callback has been compared against all 17,136
stock mesh vertices. Maximum visible error at 1920-pixel render resolution is
below 0.00064 pixels for those synthetic configurations. Sanitizer tests also
cover reversed axes, asymmetric FOVs, all color terms, malformed configuration
and ownership after the caller releases its configuration.

The compositor still discretizes this callback for rendering. The maintained
Monado patch uses a 256×256 distortion texture for compute composition and a
256-cell-per-axis graphics mesh (257×257 vertices per eye). With the synthetic
geometry above, nine samples per cell give maximum errors below 0.26 pixels for
bilinear texture sampling and 0.48 pixels for the graphics mesh's triangle
interpolation. These are sampled numerical results; physical timing and a
continuous error bound remain unverified. The denser resources consume more
memory and graphics work than the upstream defaults. `XRT_MESH_SIZE` can still
override graphics mesh density, so reducing it requires another accuracy audit.

Set `PICO_HMD_TEST_ANALYTIC=1` in the isolated rendering test to exercise schema
version 2. Four fresh services cover identity and offset radial/RGB models in
both graphics and compute composition. Independent pixel expectations include
per-channel texture borders. The recorded run submits 1,775 projection frames;
all 267,912 sampled eye pixels match across the 16 stable input phases. This
uses synthetic optics and software Vulkan, without physical panel acceptance.

Adding `PICO_HMD_TEST_ROTATED=1` tests opposite 90-degree view rotations with
the same four analytic cases. The pixel oracle rotates screen coordinates
before evaluating the optical model. This catches differences between the
graphics mesh and compute texture conventions. All 267,912 sampled pixels
also match in this run; it verifies the supplied rotation, not which rotation
a physical panel requires.
