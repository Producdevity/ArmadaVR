#include "imu-time-align.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct imu_timed_sample sample_at(int64_t time)
{
    float t = (float)(time - 1000000000) / 1000000;
    return (struct imu_timed_sample){time, {t, 2*t, -t}, {-3*t, t, 4*t}, 25+t};
}

int main(void)
{
    unsigned comparisons = 0;
    for (int a = -3; a <= 3; ++a) {
        for (int g = -3; g <= 3; ++g) {
            struct imu_time_alignment state;
            assert(imu_time_alignment_init(&state, a * 400000, g * 400000, 2000000));
            for (unsigned i = 0; i < 128; ++i) {
                struct imu_timed_sample sample = sample_at(1000000000 + (int64_t)i * 1000000);
                struct imu_timed_sample output = {0}, before = output;
                enum imu_time_alignment_status status = imu_time_align(&state, &sample, &output);
                int64_t delta = (int64_t)abs(a - g) * 400000;
                if ((int64_t)i * 1000000 < delta) {
                    assert(status == IMU_TIME_NEEDS_HISTORY);
                    assert(memcmp(&output, &before, sizeof(output)) == 0);
                    continue;
                }
                assert(status == IMU_TIME_ALIGNED);
                int low = a < g ? a : g;
                struct imu_timed_sample acceleration = sample_at(sample.timestamp_ns + (low-a) * 400000);
                struct imu_timed_sample gyroscope = sample_at(sample.timestamp_ns + (low-g) * 400000);
                assert(output.timestamp_ns == sample.timestamp_ns + low * 400000);
                assert(output.temperature_c == sample.temperature_c);
                for (unsigned axis = 0; axis < 3; ++axis) {
                    assert(fabsf(output.acceleration[axis] - acceleration.acceleration[axis]) < 0.0001f);
                    assert(fabsf(output.angular_velocity[axis] - gyroscope.angular_velocity[axis]) < 0.0001f);
                }
                ++comparisons;
            }
        }
    }
    struct imu_time_alignment state, saved;
    struct imu_timed_sample sample = sample_at(1000000000), output = {0}, before;
    assert(imu_time_alignment_init(&state, 0, 500000, 2000000));
    assert(imu_time_align(&state, &sample, &output) == IMU_TIME_NEEDS_HISTORY);
    saved = state; before = output;
    assert(!imu_time_alignment_init(&state, 0, 0, 0));
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
    assert(imu_time_align(&state, &sample, &output) == IMU_TIME_OUT_OF_ORDER);
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
    sample.timestamp_ns -= 1;
    assert(imu_time_align(&state, &sample, &output) == IMU_TIME_OUT_OF_ORDER);
    sample = sample_at(1001000000); sample.acceleration[1] = NAN;
    assert(imu_time_align(&state, &sample, &output) == IMU_TIME_INVALID);
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
    assert(memcmp(&output, &before, sizeof(output)) == 0);
    sample = sample_at(1001000000);
    assert(imu_time_align(&state, &sample, &output) == IMU_TIME_ALIGNED);
    before = output;
    sample = sample_at(1100000000);
    assert(imu_time_align(&state, &sample, &output) == IMU_TIME_NEEDS_HISTORY);
    assert(state.count == 1 && memcmp(&output, &before, sizeof(output)) == 0);
    sample = sample_at(1101000000);
    assert(imu_time_align(&state, &sample, &output) == IMU_TIME_ALIGNED);

    assert(imu_time_alignment_init(&state, INT64_MAX, 0, 1));
    saved = state; before = output;
    assert(imu_time_align(&state, &sample, &output) == IMU_TIME_INVALID);
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
    assert(memcmp(&output, &before, sizeof(output)) == 0);
    assert(imu_time_alignment_init(&state, INT64_MIN, 0, 1));
    saved = state;
    assert(imu_time_align(&state, &sample, &output) == IMU_TIME_INVALID);
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
    assert(imu_time_alignment_init(&state, 0, 5000000, 2000000));
    for (unsigned i = 0; i < 20; ++i) {
        sample = sample_at(1000000000 + (int64_t)i * 1000000);
        assert(imu_time_align(&state, &sample, &output) == IMU_TIME_NEEDS_HISTORY);
    }
    assert(state.count == IMU_TIME_ALIGN_HISTORY);
    assert(imu_time_alignment_init(&state, 0, 0, 2000000));
    assert(imu_time_align(&state, &sample, &sample) == IMU_TIME_ALIGNED);
    assert(imu_time_align(NULL, &sample, &output) == IMU_TIME_INVALID);
    assert(imu_time_align(&state, NULL, &output) == IMU_TIME_INVALID);
    assert(imu_time_align(&state, &sample, NULL) == IMU_TIME_INVALID);
    printf("%u time-aligned vector comparisons and rejection checks passed\n", comparisons);
}
