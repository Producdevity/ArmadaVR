#include "imu-time-align.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

bool imu_time_alignment_init(struct imu_time_alignment *state,
                             int64_t acceleration_offset_ns,
                             int64_t gyroscope_offset_ns, int64_t max_gap_ns)
{
    if (state == NULL || max_gap_ns <= 0)
        return false;
    *state = (struct imu_time_alignment){
        .initialized = true,
        .acceleration_offset_ns = acceleration_offset_ns,
        .gyroscope_offset_ns = gyroscope_offset_ns,
        .max_gap_ns = max_gap_ns,
    };
    return true;
}

static bool corrected_time(int64_t timestamp, int64_t offset, int64_t *result)
{
    if ((offset > 0 && timestamp > INT64_MAX - offset) ||
        (offset < 0 && timestamp < INT64_MIN - offset))
        return false;
    *result = timestamp + offset;
    return *result > 0;
}

enum imu_time_alignment_status imu_time_align(
    struct imu_time_alignment *state, const struct imu_timed_sample *sample,
    struct imu_timed_sample *aligned)
{
    if (state == NULL || sample == NULL || aligned == NULL || !state->initialized ||
        state->count > IMU_TIME_ALIGN_HISTORY || state->max_gap_ns <= 0 ||
        sample->timestamp_ns <= 0 || !isfinite(sample->temperature_c))
        return IMU_TIME_INVALID;
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!isfinite(sample->acceleration[axis]) || !isfinite(sample->angular_velocity[axis]))
            return IMU_TIME_INVALID;

    int64_t acceleration_time, gyroscope_time;
    if (!corrected_time(sample->timestamp_ns, state->acceleration_offset_ns, &acceleration_time) ||
        !corrected_time(sample->timestamp_ns, state->gyroscope_offset_ns, &gyroscope_time))
        return IMU_TIME_INVALID;

    struct imu_time_alignment next = *state;
    if (next.count != 0) {
        int64_t previous = next.history[next.count - 1].timestamp_ns;
        if (sample->timestamp_ns <= previous)
            return IMU_TIME_OUT_OF_ORDER;
        if (sample->timestamp_ns - previous > next.max_gap_ns)
            next.count = 0;
    }
    if (next.count == IMU_TIME_ALIGN_HISTORY) {
        memmove(next.history, next.history + 1,
                (IMU_TIME_ALIGN_HISTORY - 1) * sizeof(next.history[0]));
        --next.count;
    }
    next.history[next.count++] = *sample;
    struct imu_timed_sample result = *sample;
    result.timestamp_ns = acceleration_time < gyroscope_time ? acceleration_time : gyroscope_time;
    if (acceleration_time != gyroscope_time) {
        bool interpolate_acceleration = acceleration_time > gyroscope_time;
        int64_t offset = interpolate_acceleration ? state->acceleration_offset_ns : state->gyroscope_offset_ns;
        unsigned upper = 0;
        int64_t lower_time = 0, upper_time = 0;
        for (; upper < next.count; ++upper) {
            if (!corrected_time(next.history[upper].timestamp_ns, offset, &upper_time))
                return IMU_TIME_INVALID;
            if (upper_time >= result.timestamp_ns)
                break;
            lower_time = upper_time;
        }
        if (upper == next.count || (upper == 0 && upper_time != result.timestamp_ns)) {
            *state = next;
            return IMU_TIME_NEEDS_HISTORY;
        }
        unsigned lower = upper_time == result.timestamp_ns ? upper : upper - 1;
        float weight = lower == upper ? 0.0f :
            (float)(((double)(result.timestamp_ns - lower_time) / 1e9) /
                    ((double)(upper_time - lower_time) / 1e9));
        const float *a = interpolate_acceleration ? next.history[lower].acceleration : next.history[lower].angular_velocity;
        const float *b = interpolate_acceleration ? next.history[upper].acceleration : next.history[upper].angular_velocity;
        float *destination = interpolate_acceleration ? result.acceleration : result.angular_velocity;
        for (unsigned axis = 0; axis < 3; ++axis) {
            destination[axis] = fmaf(b[axis], weight, a[axis] * (1.0f - weight));
            if (!isfinite(destination[axis]))
                return IMU_TIME_INVALID;
        }
    }
    *state = next;
    *aligned = result;
    return IMU_TIME_ALIGNED;
}
