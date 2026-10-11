#include "fisheye62.h"

#include <float.h>
#include <math.h>

bool fisheye62_project(const double parameters[11], const double point[3], double pixel[2])
{
    if (!parameters || !point || !pixel)
        return false;
    for (unsigned i = 0; i < 11; ++i)
        if (!isfinite(parameters[i]))
            return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!isfinite(point[i]))
            return false;
    if (parameters[0] <= 0 || point[2] <= 0)
        return false;

    double x = point[0] / point[2], y = point[1] / point[2];
    double radius = hypot(x, y);
    if (!isfinite(radius))
        return false;
    double theta = atan(radius);
    double theta2 = theta * theta;
    double radial = parameters[8];
    for (int i = 7; i >= 3; --i)
        radial = parameters[i] + theta2 * radial;
    double scale = (radius > DBL_EPSILON ? theta / radius : 1.0) * (1.0 + theta2 * radial);
    double u = x * scale, v = y * scale;
    double radius2 = u * u + v * v;
    double tangent = 2.0 * (parameters[9] * u + parameters[10] * v);
    double result[2] = {
        parameters[1] + parameters[0] * (u + u * tangent + parameters[9] * radius2),
        parameters[2] + parameters[0] * (v + v * tangent + parameters[10] * radius2),
    };
    if (!isfinite(result[0]) || !isfinite(result[1]))
        return false;
    pixel[0] = result[0];
    pixel[1] = result[1];
    return true;
}
