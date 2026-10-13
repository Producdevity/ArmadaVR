# Pico HMD integration

The native Monado Pico driver can expose an HMD backed by the four-subscription
SSC head session. It reports orientation only. Position, yaw calibration,
prediction, cameras, eye tracking and proximity sensing are not implemented.
No HMD is opened by default.

Set both `PICO_HMD_CONFIG` to a private calibration JSON file and
`PICO_HMD_COUNTER=1` to opt in. The counter option acknowledges that the CPU
counter and SSC timestamp origins must have been verified for the device.
It does not establish that relationship. Controller configuration remains
separate through `PICO_SSC_CONFIG` and `PICO_SSC_COUNTER`.

The HMD owns its sensor session. Destruction stops subscriptions, drains their
callbacks and joins the worker before freeing optics or tracking state. Stale,
missing or rejected sensor data produces an invalid orientation rather than a
stationary tracked pose. The latest accepted capture is returned without
extrapolating to the requested display time.

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

The diskless Pico-kernel integration test runs the installed Monado service,
OpenXR loader and application over actual QRTR sockets with a synthetic SSC
server. It selects the Pico HMD and both controllers together, checks each
hand's button and touch transitions, both eye poses and FOVs, changing fused
orientation and invalid orientation after reports stop. Controller input remains
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
physical panel scanout. Test evidence is retained under
`output/pico-hmd-display-20261013-v1/`.

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


## Screen-to-texture inspection

The inspector also accepts `--screen-model FILE --screen-sha256 SHA256
--grid-size WIDTH HEIGHT`. These options must be supplied together. The bounded,
hash-bound JSON requires explicit resolved values; it does not derive them from
panel resolution, raw axis offsets or a fallback lens file.

| Screen model field | Meaning |
| --- | --- |
| `schema_version`, `layout` | `1`, `side-by-side` |
| `width_pixels`, `height_pixels` | Combined logical stereo screen dimensions, before panel rotation; width must be even |
| `width_m` | Combined active screen width in metres |
| `lens_separation_m` | Resolved stock lens separation after IPD/screen-gap adjustments |
| `meters_per_tan_angle` | Resolved optical scale in metres per unit tangent |
| `horizontal_offset_m` | Stock horizontal screen offset in metres |
| `optical_offsets_mm` | Four already adjusted offsets: left X/Y, right X/Y, in millimetres |
| `hwc_reverse` | Effective stock HWC reversal, `0` or `180` degrees |
| `separate_distortion` | Must be `false`; separate-eye coefficient overrides are not supported |
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
