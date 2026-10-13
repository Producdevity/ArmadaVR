#ifndef REFRAMED_IMU_TIME_ALIGN_H
#define REFRAMED_IMU_TIME_ALIGN_H

#include <stdbool.h>
#include <stdint.h>

#define IMU_TIME_ALIGN_HISTORY 5

struct imu_timed_sample {
    int64_t timestamp_ns;
    float acceleration[3], angular_velocity[3];
    float temperature_c;
};

struct imu_time_alignment {
    bool initialized;
    int64_t acceleration_offset_ns, gyroscope_offset_ns, max_gap_ns;
    unsigned count;
    struct imu_timed_sample history[IMU_TIME_ALIGN_HISTORY];
};

enum imu_time_alignment_status {
    IMU_TIME_ALIGNED,
    IMU_TIME_NEEDS_HISTORY,
    IMU_TIME_INVALID,
    IMU_TIME_OUT_OF_ORDER,
};

/* Offsets are added to capture time. Supply an explicitly selected model's
 * corrections, already converted to nanoseconds. Success clears history;
 * failure preserves state. No calibration model or clock mapping is inferred. */
bool imu_time_alignment_init(struct imu_time_alignment *state,
                             int64_t acceleration_offset_ns,
                             int64_t gyroscope_offset_ns, int64_t max_gap_ns);

/* Align raw vectors before temperature compensation and rectification. Output
 * uses the smaller corrected timestamp and the current sample's temperature.
 * The vector with the larger offset is interpolated from at most five samples;
 * insufficient history produces no output. A gap exceeding max_gap_ns clears
 * history. Invalid/out-of-order input preserves state and output; NEEDS_HISTORY
 * advances history only. The caller serializes access and resets on restart. */
enum imu_time_alignment_status imu_time_align(
    struct imu_time_alignment *state, const struct imu_timed_sample *sample,
    struct imu_timed_sample *aligned);

#endif
