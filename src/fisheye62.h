#ifndef ARMADA_FISHEYE62_H
#define ARMADA_FISHEYE62_H

#include <stdbool.h>

/* Parameters: f, cx, cy, six radial coefficients, then two tangential
 * coefficients. Point is in the camera frame with positive Z. Image bounds,
 * calibrated field of view and extrinsics remain the caller's responsibility.
 * Returns false without changing pixel for invalid or non-finite results. */
bool fisheye62_project(const double parameters[11], const double point[3], double pixel[2]);

#endif
