#include "imu-calibration.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct imu_temperature_offset rows[] = {
        {10, {1, -2, 3}}, {20, {3, 2, -1}}, {40, {1, 4, -3}},
    };
    float result[3];
    unsigned cases = 0;
    for (int t = -200; t <= 600; ++t) {
        float temperature = t / 10.0f;
        assert(imu_offset_at_temperature(rows, 3, temperature, result));
        float u = fmaxf(0, fminf(1, (temperature - 10) / 10));
        float v = fmaxf(0, fminf(1, (temperature - 20) / 20));
        float expected[] = {1 + 2 * u - 2 * v, -2 + 4 * u + 2 * v, 3 - 4 * u - 2 * v};
        for (unsigned axis = 0; axis < 3; ++axis)
            assert(fabsf(result[axis] - expected[axis]) < 1e-6f);
        assert(imu_offset_at_temperature(rows, 1, temperature, result));
        assert(!memcmp(result, rows[0].offset, sizeof(result)));
        ++cases;
    }
    float matrix[] = {0, -2, 0, 3, 0, 0, 0, 0, 4};
    float raw[] = {5, 6, 7}, offset[] = {1, 2, 3};
    assert(imu_rectify_vector(matrix, offset, raw, raw));
    assert(raw[0] == -8 && raw[1] == 12 && raw[2] == 16);
    float saved[] = {123, 456, 789};
    memcpy(result, saved, sizeof(result));
    assert(!imu_offset_at_temperature(NULL, 3, 15, result));
    assert(!imu_offset_at_temperature(rows, 0, 15, result));
    assert(!imu_offset_at_temperature(rows, 1025, 15, result));
    assert(!imu_offset_at_temperature(rows, 3, NAN, result));
    assert(!imu_offset_at_temperature(rows, 3, INFINITY, result));
    assert(!imu_offset_at_temperature(rows, 3, 15, NULL));
    rows[1].temperature = 10;
    assert(!imu_offset_at_temperature(rows, 3, 15, result));
    rows[1].temperature = 9;
    assert(!imu_offset_at_temperature(rows, 3, 15, result));
    rows[1].temperature = 20;
    rows[2].offset[2] = NAN;
    assert(!imu_offset_at_temperature(rows, 3, 15, result));
    rows[2].offset[2] = -3;
    rows[0].temperature = -FLT_MAX; rows[1].temperature = FLT_MAX;
    assert(!imu_offset_at_temperature(rows, 2, 0, result));
    rows[0].temperature = 0; rows[1].temperature = 1;
    rows[0].offset[0] = -FLT_MAX; rows[1].offset[0] = FLT_MAX;
    assert(!imu_offset_at_temperature(rows, 2, 0.5f, result));
    assert(!memcmp(result, saved, sizeof(result)));
    assert(!imu_rectify_vector(NULL, offset, raw, result));
    assert(!imu_rectify_vector(matrix, NULL, raw, result));
    assert(!imu_rectify_vector(matrix, offset, NULL, result));
    assert(!imu_rectify_vector(matrix, offset, raw, NULL));
    matrix[8] = INFINITY;
    assert(!imu_rectify_vector(matrix, offset, raw, result));
    matrix[8] = FLT_MAX;
    assert(!imu_rectify_vector(matrix, offset, raw, result));
    matrix[8] = 4; offset[0] = NAN;
    assert(!imu_rectify_vector(matrix, offset, raw, result));
    offset[0] = -FLT_MAX; raw[0] = FLT_MAX;
    assert(!imu_rectify_vector(matrix, offset, raw, result));
    assert(!memcmp(result, saved, sizeof(result)));
    printf("%u IMU calibration temperature cases and validation checks passed\n", cases);
    return 0;
}
