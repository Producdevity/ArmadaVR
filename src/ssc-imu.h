#ifndef ARMADA_SSC_IMU_H
#define ARMADA_SSC_IMU_H

#include "ssc-clock.h"
#include <stddef.h>

enum ssc_imu_sensor { SSC_IMU_ACCELERATION, SSC_IMU_GYROSCOPE };

struct ssc_imu_vector {
    float values[3];
    unsigned accuracy;
};

struct ssc_imu_config {
    struct ssc_clock clock;
    uint64_t suid_high[2], suid_low[2];
    int64_t max_age_ns;
};

struct ssc_imu_event {
    struct ssc_imu_vector vector;
    struct ssc_timestamp time;
    enum ssc_imu_sensor sensor;
    uint64_t generation, ticks;
};

struct ssc_imu_stream {
    struct ssc_imu_config config;
    uint64_t generation, last_ticks[2];
    int64_t last_time_ns[2], last_received_ns;
    bool active, seen[2];
};

/* Decode only the sns_std_sensor_event payload: exactly three finite floats
 * and required accuracy 0..3. Packed/unpacked floats may be mixed. Unknown
 * scalar fields are skipped; groups and payloads over 4096 bytes are refused.
 * Output is unchanged on failure. No units, axes or calibration are inferred. */
bool ssc_imu_decode(const void *payload, size_t length, struct ssc_imu_vector *vector);

/* Zero-initialize once and confine all calls to one thread. Supply two distinct,
 * verified SUIDs and a fresh clock after reconnect, suspend or DSP restart.
 * Capture the returned generation in both subscription callbacks. */
bool ssc_imu_begin(struct ssc_imu_stream *stream, const struct ssc_imu_config *config);
void ssc_imu_invalidate(struct ssc_imu_stream *stream);

/* Accept only message 1025, matching generation/SUID and a current timestamp.
 * Ordering is independent per sensor; samples are not paired or resampled.
 * Accuracy zero remains observable and must not be treated as calibrated data.
 * Rejection preserves stream/output; no payload pointer escapes the call. */
bool ssc_imu_report(struct ssc_imu_stream *stream, uint64_t generation, uint32_t id,
                    uint64_t high, uint64_t low, uint64_t ticks, const void *payload,
                    size_t length, int64_t now_ns, struct ssc_imu_event *event);

#endif
