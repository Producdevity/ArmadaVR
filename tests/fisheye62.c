#include "fisheye62.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void close_to(double actual, double expected)
{
    assert(fabs(actual - expected) <= 1e-10 * fmax(1.0, fabs(expected)));
}

int main(void)
{
    double parameters[11] = {500, 640, 512};
    double point[3] = {0, 0, 1}, pixel[2] = {0};
    assert(fisheye62_project(parameters, point, pixel));
    assert(pixel[0] == 640 && pixel[1] == 512);
    unsigned cases = 1;
    for (int polar = 1; polar <= 80; ++polar) {
        double theta = polar * acos(-1.0) / 180;
        for (int azimuth = 0; azimuth < 360; azimuth += 7) {
            double phi = azimuth * acos(-1.0) / 180;
            point[0] = sin(theta) * cos(phi);
            point[1] = sin(theta) * sin(phi);
            point[2] = cos(theta);
            assert(fisheye62_project(parameters, point, pixel));
            close_to(pixel[0], 640 + 500 * theta * cos(phi));
            close_to(pixel[1], 512 + 500 * theta * sin(phi));
            for (unsigned coefficient = 3; coefficient < 11; ++coefficient) {
                parameters[coefficient] = 0.001;
                assert(fisheye62_project(parameters, point, pixel));
                double radius = theta;
                if (coefficient < 9)
                    radius += 0.001 * pow(theta, 2 * (coefficient - 3) + 3);
                double u = radius * cos(phi), v = radius * sin(phi);
                double du = 0, dv = 0;
                if (coefficient == 9) {
                    du = 0.001 * (3 * u * u + v * v);
                    dv = 0.002 * u * v;
                } else if (coefficient == 10) {
                    du = 0.002 * u * v;
                    dv = 0.001 * (u * u + 3 * v * v);
                }
                close_to(pixel[0], 640 + 500 * (u + du));
                close_to(pixel[1], 512 + 500 * (v + dv));
                parameters[coefficient] = 0;
                ++cases;
            }
            ++cases;
        }
    }
    double invalid[] = {NAN, INFINITY, -INFINITY};
    double original[2] = {123, 456};
    memcpy(pixel, original, sizeof(pixel));
    for (unsigned value = 0; value < 3; ++value) {
        for (unsigned index = 0; index < 11; ++index) {
            double saved = parameters[index];
            parameters[index] = invalid[value];
            assert(!fisheye62_project(parameters, point, pixel));
            assert(!memcmp(pixel, original, sizeof(pixel)));
            parameters[index] = saved;
        }
        for (unsigned index = 0; index < 3; ++index) {
            double saved = point[index];
            point[index] = invalid[value];
            assert(!fisheye62_project(parameters, point, pixel));
            assert(!memcmp(pixel, original, sizeof(pixel)));
            point[index] = saved;
        }
    }
    for (int depth = -1; depth <= 0; ++depth) {
        point[2] = depth;
        assert(!fisheye62_project(parameters, point, pixel));
        assert(!memcmp(pixel, original, sizeof(pixel)));
    }
    point[2] = DBL_MIN;
    point[0] = DBL_MAX;
    assert(!fisheye62_project(parameters, point, pixel));
    assert(!memcmp(pixel, original, sizeof(pixel)));
    point[0] = 1; point[2] = 1;
    parameters[0] = 0;
    assert(!fisheye62_project(parameters, point, pixel));
    parameters[0] = DBL_MAX;
    parameters[9] = DBL_MAX;
    assert(!fisheye62_project(parameters, point, pixel));
    assert(!memcmp(pixel, original, sizeof(pixel)));
    assert(!fisheye62_project(NULL, point, pixel));
    assert(!fisheye62_project(parameters, NULL, pixel));
    assert(!fisheye62_project(parameters, point, NULL));
    printf("%u Fisheye62 projection cases and invalid-input checks passed\n", cases);
    return 0;
}
