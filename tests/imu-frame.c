#include "imu-calibration.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const double identity[9] = {1,0,0,0,1,0,0,0,1};

static void multiply(const double a[9], const double b[9], double result[9])
{
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned col = 0; col < 3; ++col) {
            result[row * 3 + col] = 0;
            for (unsigned k = 0; k < 3; ++k)
                result[row * 3 + col] += a[row * 3 + k] * b[k * 3 + col];
        }
}

static void rotate(double angle, double result[9])
{
    double x[9] = {1,0,0,0,cos(angle),-sin(angle),0,sin(angle),cos(angle)};
    double y[9] = {cos(angle * .31),0,sin(angle * .31),0,1,0,
                   -sin(angle * .31),0,cos(angle * .31)};
    double z[9] = {cos(angle * .79),-sin(angle * .79),0,
                   sin(angle * .79),cos(angle * .79),0,0,0,1};
    double xy[9];
    multiply(x, y, xy); multiply(xy, z, result);
}

static void rejected(const double *accel, const double *gyro, const double *device,
                     enum imu_rectification_frame frame)
{
    struct imu_rectification output, before;
    memset(&output, 0xa5, sizeof(output)); memcpy(&before, &output, sizeof(before));
    assert(!imu_prepare_rectification(accel, gyro, device, frame, &output));
    assert(!memcmp(&output, &before, sizeof(output)));
}

int main(void)
{
    unsigned cases = 0;
    for (unsigned i = 0; i < 4096; ++i) {
        double d[9], qa[9], qg[9], a[9], g[9];
        double ra[9] = {1.1,.03,-.02,0,.9,.06,0,0,1.05};
        double rg[9] = {.95,-.04,.05,0,1.02,.01,0,0,1.08};
        rotate(i * .731, d); rotate(i * .231, qa); rotate(i * .173, qg);
        multiply(qa, ra, a); multiply(qg, rg, g);
        for (unsigned mode = 0; mode < 2; ++mode) {
            struct imu_rectification output;
            assert(imu_prepare_rectification(a, g, d, mode, &output));
            for (unsigned sensor = 0; sensor < 2; ++sensor) {
                const double *original = sensor ? g : a;
                const float *prepared = sensor ? output.gyroscope : output.accelerometer;
                double converted[9], before[9], after[9];
                for (unsigned k = 0; k < 9; ++k)
                    converted[k] = prepared[k];
                multiply(d, original, before);
                multiply(output.device_from_imu, converted, after);
                for (unsigned k = 0; k < 9; ++k)
                    assert(fabs(before[k] - after[k]) < 2e-7);
                if (mode == IMU_FRAME_ORIGINAL)
                    for (unsigned k = 0; k < 9; ++k)
                        assert(prepared[k] == (float)original[k]);
            }
            if (mode == IMU_FRAME_ACCELEROMETER) {
                assert(output.accelerometer[3] == 0 && output.accelerometer[6] == 0 &&
                       output.accelerometer[7] == 0);
                assert(output.accelerometer[0] > 0 && output.accelerometer[4] > 0 &&
                       output.accelerometer[8] > 0);
            } else {
                assert(!memcmp(d, output.device_from_imu, sizeof(d)));
            }
            ++cases;
        }
    }
    double bad[9];
    double values[] = {NAN, INFINITY, -INFINITY};
    for (unsigned mode = 0; mode < 2; ++mode) {
        rejected(NULL, identity, identity, mode);
        rejected(identity, NULL, identity, mode);
        rejected(identity, identity, NULL, mode);
        assert(!imu_prepare_rectification(identity, identity, identity, mode, NULL));
        for (unsigned index = 0; index < 9; ++index)
            for (unsigned v = 0; v < 3; ++v) {
                memcpy(bad, identity, sizeof(bad)); bad[index] = values[v];
                rejected(bad, identity, identity, mode);
                rejected(identity, bad, identity, mode);
                rejected(identity, identity, bad, mode);
            }
        double scales[] = {0, -1, 1e-13, 1e100, 1e-100};
        for (unsigned s = 0; s < 5; ++s) {
            memcpy(bad, identity, sizeof(bad)); bad[0] = scales[s];
            rejected(bad, identity, identity, mode);
            rejected(identity, bad, identity, mode);
            rejected(identity, identity, bad, mode);
        }
        for (unsigned s = 0; s < 2; ++s) {
            memcpy(bad, identity, sizeof(bad));
            bad[0] = bad[4] = bad[8] = s ? 1e-100 : DBL_MAX;
            rejected(bad, identity, identity, mode);
            rejected(identity, bad, identity, mode);
        }
        memcpy(bad, identity, sizeof(bad)); bad[1] = 1e-6;
        rejected(identity, identity, bad, mode);
    }
    rejected(identity, identity, identity, (enum imu_rectification_frame)-1);
    rejected(identity, identity, identity, (enum imu_rectification_frame)2);
    puts("8192 IMU frame invariance cases and rejection checks passed");
    assert(cases == 8192);
    return 0;
}
