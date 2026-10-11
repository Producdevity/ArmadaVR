#include "fisheye62.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void unchanged(const double parameters[11], const double pixel[2], double limit)
{
    double result[3] = {123, 456, 789}, saved[3];
    memcpy(saved, result, sizeof(saved));
    assert(!fisheye62_unproject(parameters, pixel, limit, result));
    assert(!memcmp(saved, result, sizeof(saved)));
}

int main(void)
{
    double parameters[11] = {500, 640, 512}, pixel[2], actual[3];
    double limit = 1.4;
    unsigned cases = 0;
    for (unsigned model = 0; model < 10; ++model) {
        memset(parameters + 3, 0, 8 * sizeof(double));
        if (model > 0 && model < 9)
            parameters[model + 2] = 0.001;
        if (model == 9) {
            parameters[3] = -0.08;
            parameters[4] = 0.03;
            parameters[5] = -0.004;
            parameters[9] = -0.002;
            parameters[10] = 0.003;
        }
        for (unsigned polar = 0; polar <= 100; ++polar) {
            double theta = limit * polar / 100;
            for (unsigned azimuth = 0; azimuth < 360; azimuth += 13) {
                double phi = azimuth * acos(-1) / 180;
                double ray[3] = {sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta)};
                assert(fisheye62_project(parameters, ray, pixel));
                if (!fisheye62_unproject(parameters, pixel, limit, actual)) {
                    fprintf(stderr, "inverse failed: model %u polar %u azimuth %u\n", model, polar, azimuth);
                    assert(0);
                }
                assert(fabs(actual[0] - ray[0] / ray[2]) < 1e-10);
                assert(fabs(actual[1] - ray[1] / ray[2]) < 1e-10);
                assert(actual[2] == 1);
                ++cases;
            }
        }
    }
    memset(parameters + 3, 0, 8 * sizeof(double));
    for (unsigned i = 0; i <= 100; ++i) {
        double theta = limit * i / 100;
        pixel[0] = parameters[1] + parameters[0] * theta;
        pixel[1] = parameters[2];
        assert(fisheye62_unproject(parameters, pixel, limit, actual));
        assert(fabs(actual[0] - tan(theta)) < 1e-11);
        assert(actual[1] == 0 && actual[2] == 1);
        ++cases;
    }
    pixel[0] = 640; pixel[1] = 512;
    double invalid[] = {NAN, INFINITY, -INFINITY};
    for (unsigned value = 0; value < 3; ++value) {
        for (unsigned index = 0; index < 11; ++index) {
            double saved = parameters[index]; parameters[index] = invalid[value];
            unchanged(parameters, pixel, limit); parameters[index] = saved;
        }
        for (unsigned index = 0; index < 2; ++index) {
            double saved = pixel[index]; pixel[index] = invalid[value];
            unchanged(parameters, pixel, limit); pixel[index] = saved;
        }
        unchanged(parameters, pixel, invalid[value]);
    }
    unchanged(parameters, pixel, -1);
    unchanged(parameters, pixel, 0);
    unchanged(parameters, pixel, acos(0));
    unchanged(parameters, pixel, 2);
    parameters[0] = 0; unchanged(parameters, pixel, limit);
    parameters[0] = -1; unchanged(parameters, pixel, limit);
    parameters[0] = 500;
    pixel[0] = 640 + 500 * (limit + 1e-8); unchanged(parameters, pixel, limit);
    pixel[0] = DBL_MAX; unchanged(parameters, pixel, limit);
    pixel[0] = 640;
    parameters[3] = -5.0 / 3; parameters[4] = 1;
    unchanged(parameters, pixel, 1);
    parameters[3] = 0; parameters[4] = 0;
    parameters[9] = 0.2; unchanged(parameters, pixel, limit);
    parameters[9] = 0; parameters[8] = DBL_MAX; unchanged(parameters, pixel, limit);
    parameters[8] = 0;
    assert(!fisheye62_unproject(NULL, pixel, limit, actual));
    assert(!fisheye62_unproject(parameters, NULL, limit, actual));
    assert(!fisheye62_unproject(parameters, pixel, limit, NULL));
    double aliased[3] = {650, 520, 0};
    assert(fisheye62_unproject(parameters, aliased, limit, aliased));
    assert(fisheye62_project(parameters, aliased, pixel));
    assert(fabs(pixel[0] - 650) < 1e-10 && fabs(pixel[1] - 520) < 1e-10);
    printf("%u inverse projection cases and rejection checks passed\n", cases);
    return 0;
}
