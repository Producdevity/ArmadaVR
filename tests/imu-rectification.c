#include "imu-calibration.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void multiply(const double a[9], const double b[9], double result[9])
{
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned col = 0; col < 3; ++col) {
            result[row * 3 + col] = 0;
            for (unsigned k = 0; k < 3; ++k)
                result[row * 3 + col] += a[row * 3 + k] * b[k * 3 + col];
        }
}

static void rejected(const double matrix[9])
{
    double q[9], r[9], before[9];
    for (unsigned i = 0; i < 9; ++i)
        before[i] = q[i] = r[i] = 10 + i;
    assert(!imu_split_rectification(matrix, q, r));
    assert(!memcmp(q, before, sizeof(q)) && !memcmp(r, before, sizeof(r)));
}

int main(void)
{
    unsigned cases = 0;
    for (unsigned i = 0; i < 4096; ++i) {
        double a = i * 0.271, b = i * 0.194, c = i * 0.013;
        double x[9] = {1,0,0,0,cos(a),-sin(a),0,sin(a),cos(a)};
        double y[9] = {cos(b),0,sin(b),0,1,0,-sin(b),0,cos(b)};
        double z[9] = {cos(c),-sin(c),0,sin(c),cos(c),0,0,0,1};
        double xy[9], expected_q[9];
        multiply(x, y, xy); multiply(xy, z, expected_q);
        double scales[] = {1, 1e-100, 1e100};
        for (unsigned s = 0; s < 3; ++s) {
            double expected_r[9] = {0.5 + i * .0001, .02*sin(a), -.03*cos(b),
                                    0, 1.7, .04*cos(c), 0, 0, .9};
            for (unsigned k = 0; k < 9; ++k)
                expected_r[k] *= scales[s];
            double input[9], q[9], r[9], saved[9];
            multiply(expected_q, expected_r, input); memcpy(saved, input, sizeof(saved));
            assert(imu_split_rectification(input, q, r));
            assert(!memcmp(saved, input, sizeof(saved)));
            for (unsigned k = 0; k < 9; ++k) {
                assert(fabs(q[k] - expected_q[k]) < 1e-12);
                assert(fabs((r[k] - expected_r[k]) / scales[s]) < 1e-12);
            }
            ++cases;
        }
    }
    double input[9] = {1,0,0,0,1,0,0,0,1}, q[9], r[9];
    double invalid[] = {NAN, INFINITY, -INFINITY};
    for (unsigned v = 0; v < 3; ++v)
        for (unsigned i = 0; i < 9; ++i) {
            double saved = input[i]; input[i] = invalid[v]; rejected(input); input[i] = saved;
        }
    for (unsigned i = 0; i < 3; ++i) {
        input[i * 3 + i] = 0; rejected(input);
        input[i * 3 + i] = -1; rejected(input);
        input[i * 3 + i] = 1e-13; rejected(input);
        input[i * 3 + i] = 1;
    }
    double duplicate[9] = {1,1,0,0,0,0,0,0,1}; rejected(duplicate);
    double overflow[9] = {DBL_MAX,0,0,DBL_MAX,DBL_MAX,0,DBL_MAX,0,DBL_MAX}; rejected(overflow);
    assert(!imu_split_rectification(NULL, q, r));
    assert(!imu_split_rectification(input, NULL, r));
    assert(!imu_split_rectification(input, q, NULL));
    assert(!imu_split_rectification(input, q, q));
    assert(imu_split_rectification(input, input, r));
    assert(input[0] == 1 && input[4] == 1 && input[8] == 1);
    printf("%u IMU rectification split cases and rejection checks passed\n", cases);
    return 0;
}
