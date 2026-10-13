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

Version 1 requires every field below. There are no physical defaults. The file
must be a regular file, at most 2 MiB; final symlinks, embedded NULs, trailing
JSON, duplicate required fields and malformed numbers are refused. Units and
coordinates must describe the active sensor and optical configuration, including
the current IPD. This format does not authenticate the measurements.

| Field | Contents |
| --- | --- |
| `schema_version`, `device`, `serial` | `1`, `PICOA7H10`, the unit identifier |
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

`grid_width` and `grid_height` each range from 2 to 65. `distortion` contains
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

The stock screen-to-texture equation, active configuration and physical alignment
still need verification before generating the driver's RGB distortion grids.
Camera calibration XML describes a separate camera system; its coefficients are
not display-lens calibration.
