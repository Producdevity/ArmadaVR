#include "fisheye62.h"

#include <float.h>
#include <math.h>

static bool positive_bernstein(const double coefficients[7], unsigned depth)
{
    double low = coefficients[0], high = low, scale = fabs(low);
    for (unsigned i = 1; i < 7; ++i) {
        if (!isfinite(coefficients[i]))
            return false;
        low = fmin(low, coefficients[i]);
        high = fmax(high, coefficients[i]);
        scale = fmax(scale, fabs(coefficients[i]));
    }
    double margin = 64 * DBL_EPSILON * fmax(1, scale);
    if (low > margin)
        return true;
    if (high <= margin || depth == 0)
        return false;
    double work[7], left[7], right[7];
    for (unsigned i = 0; i < 7; ++i)
        work[i] = coefficients[i];
    left[0] = work[0];
    right[6] = work[6];
    for (unsigned level = 1; level < 7; ++level) {
        for (unsigned i = 0; i < 7 - level; ++i)
            work[i] = 0.5 * work[i] + 0.5 * work[i + 1];
        left[level] = work[0];
        right[6 - level] = work[6 - level];
    }
    return positive_bernstein(left, depth - 1) && positive_bernstein(right, depth - 1);
}

static bool increasing_radius(const double parameters[11], double max_theta)
{
    /* The derivative is degree six in theta^2. Bernstein bounds certify
     * positivity over the entire interval, including between sample points. */
    double powers[7] = {1}, bernstein[7] = {0};
    double power = 1, theta2 = max_theta * max_theta;
    for (unsigned i = 1; i < 7; ++i) {
        power *= theta2;
        powers[i] = (2 * i + 1) * parameters[i + 2] * power;
        if (!isfinite(powers[i]))
            return false;
    }
    for (unsigned k = 0; k < 7; ++k) {
        double ratio = 1;
        bernstein[k] = 1;
        for (unsigned i = 1; i <= k; ++i) {
            ratio *= (double)(k - i + 1) / (7 - i);
            bernstein[k] += powers[i] * ratio;
        }
        if (!isfinite(bernstein[k]))
            return false;
    }
    return positive_bernstein(bernstein, 12);
}

static double distorted_radius(const double parameters[11], double theta)
{
    double value = parameters[8], square = theta * theta;
    for (int i = 7; i >= 3; --i)
        value = parameters[i] + square * value;
    return theta * (1 + square * value);
}

static double tangent_residual(const double parameters[11], double u, double v,
                               double x, double y, double residual[2])
{
    double radius2 = u * u + v * v;
    double tangent = 2 * (parameters[9] * u + parameters[10] * v);
    residual[0] = u + u * tangent + parameters[9] * radius2 - x;
    residual[1] = v + v * tangent + parameters[10] * radius2 - y;
    return hypot(residual[0], residual[1]);
}

bool fisheye62_unproject(const double parameters[11], const double pixel[2],
                        double max_theta, double point[3])
{
    if (!parameters || !pixel || !point || !isfinite(max_theta) ||
        max_theta <= 0 || max_theta >= acos(0))
        return false;
    for (unsigned i = 0; i < 11; ++i)
        if (!isfinite(parameters[i]))
            return false;
    if (!isfinite(pixel[0]) || !isfinite(pixel[1]) || parameters[0] <= 0 ||
        !increasing_radius(parameters, max_theta))
        return false;
    double limit = distorted_radius(parameters, max_theta);
    /* A positive-definite tangential Jacobian on this disk gives a unique
     * inverse. Its deviation from identity is bounded by 6*|p|*radius. */
    if (!isfinite(limit) || limit <= 0 ||
        !(6 * hypot(parameters[9], parameters[10]) * limit < 1 - 64 * DBL_EPSILON))
        return false;
    double x = (pixel[0] - parameters[1]) / parameters[0];
    double y = (pixel[1] - parameters[2]) / parameters[0];
    double target_radius = hypot(x, y);
    if (!isfinite(target_radius))
        return false;
    double u = x, v = y;
    if (target_radius > limit) {
        u *= limit / target_radius;
        v *= limit / target_radius;
    }
    double residual[2], tolerance = 64 * DBL_EPSILON * fmax(1, target_radius);
    double error = tangent_residual(parameters, u, v, x, y, residual);
    for (unsigned iteration = 0; iteration < 32 && error > tolerance; ++iteration) {
        double a = 1 + 6 * parameters[9] * u + 2 * parameters[10] * v;
        double b = 2 * (parameters[9] * v + parameters[10] * u);
        double d = 1 + 2 * parameters[9] * u + 6 * parameters[10] * v;
        double determinant = a * d - b * b;
        if (!isfinite(determinant) || determinant <= 0)
            return false;
        double du = (d * residual[0] - b * residual[1]) / determinant;
        double dv = (a * residual[1] - b * residual[0]) / determinant;
        bool progressed = false;
        for (unsigned step = 0; step < 32; ++step) {
            double candidate_u = u - du, candidate_v = v - dv, next[2];
            double next_error = tangent_residual(parameters, candidate_u, candidate_v, x, y, next);
            if (isfinite(next_error) && next_error < error &&
                hypot(candidate_u, candidate_v) <= limit + tolerance) {
                u = candidate_u;
                v = candidate_v;
                error = next_error;
                residual[0] = next[0];
                residual[1] = next[1];
                progressed = true;
                break;
            }
            du *= 0.5;
            dv *= 0.5;
        }
        if (!progressed)
            return false;
    }
    double radius = hypot(u, v);
    if (!isfinite(error) || error > tolerance || radius > limit + tolerance)
        return false;
    double low = 0, high = max_theta;
    for (unsigned i = 0; i < 64; ++i) {
        double middle = 0.5 * (low + high);
        if (distorted_radius(parameters, middle) < radius)
            low = middle;
        else
            high = middle;
    }
    double theta = 0.5 * (low + high);
    double scale = radius > DBL_MIN ? tan(theta) / radius : 1;
    double result[3] = {u * scale, v * scale, 1}, check[2];
    if (!fisheye62_project(parameters, result, check) ||
        fabs((check[0] - pixel[0]) / parameters[0]) > 4 * tolerance ||
        fabs((check[1] - pixel[1]) / parameters[0]) > 4 * tolerance)
        return false;
    point[0] = result[0];
    point[1] = result[1];
    point[2] = result[2];
    return true;
}

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
