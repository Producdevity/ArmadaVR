#ifndef REFRAMED_PICO_IMU_H
#define REFRAMED_PICO_IMU_H

#include "ssc-clock.h"
#include <stddef.h>

#define PICO_IMU_PENDING 16

struct pico_imu_record {
    int64_t counter_ns;
    float vector[3], vendor_aux[3];
};

struct pico_imu {
    struct ssc_clock clock;
    int32_t tokens[2];
    int64_t max_age_ns, last_ns[2];
    uint32_t sequence;
    bool initialized;
    unsigned count[2];
    uint64_t unpaired;
    struct pico_imu_record records[2][PICO_IMU_PENDING];
};

struct pico_imu_sample {
    int64_t counter_ns, monotonic_ns, uncertainty_ns;
    float acceleration[3], angular_velocity[3];
    float acceleration_aux[3], gyroscope_aux[3];
};

enum pico_imu_status {
    PICO_IMU_ACCEPTED,
    PICO_IMU_NEEDS_PAIR,
    PICO_IMU_INVALID,
    PICO_IMU_OUT_OF_ORDER,
    PICO_IMU_STALE,
};

/* Supply a current CPU-counter/CLOCK_MONOTONIC calibration and the two Android
 * direct-report tokens, acceleration first. The timestamp domain must have been
 * verified as counter nanoseconds. No clock origin, units, calibration or device
 * axes are inferred. Reset after reconnect, suspend, clock change or sensor reset.
 * Successful initialization clears history; failure preserves state. */
bool pico_imu_init(struct pico_imu *imu, const struct ssc_clock *clock,
                   int32_t acceleration_token, int32_t gyroscope_token, int64_t max_age_ns);

/* Consume one stable copied 104-byte little-endian Android direct record.
 * Pair only equal capture timestamps, buffering up to 16 records per sensor.
 * Gaps clear both queues; overflow evicts the oldest unpaired record. The
 * saturating unpaired counter includes gaps, unmatched records and evictions.
 * All six raw
 * floats are preserved without interpreting the vendor auxiliary values.
 * ACCEPTED and NEEDS_PAIR advance state and set dropped; only ACCEPTED writes
 * sample. Other results preserve state and outputs. The caller serializes access,
 * owns ring synchronization and must not mix independent channels. */
enum pico_imu_status pico_imu_process(struct pico_imu *imu, const void *record,
                                     size_t length, int64_t now_ns,
                                     struct pico_imu_sample *sample, uint32_t *dropped);

#endif
