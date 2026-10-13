#ifndef REFRAMED_FISHEYE62_H
#define REFRAMED_FISHEYE62_H

#include <stdbool.h>

/* Parameters: f, cx, cy, six radial coefficients, then two tangential
 * coefficients. Point is in the camera frame with positive Z. Image bounds,
 * calibrated field of view and extrinsics remain the caller's responsibility.
 * Returns false without changing pixel for invalid or non-finite results. */
bool fisheye62_project(const double parameters[11], const double point[3], double pixel[2]);

/* Invert a pixel to a camera-frame ray with Z=1 within max_theta radians of
 * the optical axis (0 < max_theta < pi/2). The caller supplies a calibrated
 * angular limit and checks image bounds. Reject non-invertible/uncertifiable
 * models and failed convergence without changing point. This is a lens model,
 * not a camera-to-IMU transform or a calibration for rescaled camera streams. */
bool fisheye62_unproject(const double parameters[11], const double pixel[2],
                        double max_theta, double point[3]);

#endif
