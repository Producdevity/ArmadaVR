#ifndef ARMADA_SYNCBOSS_IMU_H
#define ARMADA_SYNCBOSS_IMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct syncboss_imu_parameters {
    float lsb_per_dps;
    float lsb_per_g;
    float offset_c;
    float lsb_per_c;
    bool temperature_compensation;
};

struct syncboss_imu_sample {
    uint64_t device_id;
    uint64_t timestamp_ticks;
    float temperature_c;
    float acceleration_m_s2[3];
    float angular_velocity_rad_s[3];
};

/* Decode one raw MCU payload, without FIFO/direct-channel framing or clock offset.
 * Version and integer-format parameters must come from the matching device config.
 * Trailing transport padding is ignored. Failure leaves sample unchanged.
 * Values retain the sensor axes; this does not apply factory calibration. */
bool syncboss_imu_decode(const void *payload, size_t length, unsigned version,
                         const struct syncboss_imu_parameters *parameters,
                         struct syncboss_imu_sample *sample);

struct syncboss_imu_record {
    struct syncboss_imu_sample sample;
    int64_t timestamp_ns;
    uint8_t sequence;
};

/* Decode exactly one FIFO packet-80 record for HMD device 0.
 * A valid NSYNC offset maps MCU microseconds to CLOCK_MONOTONIC nanoseconds.
 * Reject other packets, incomplete records and unrepresentable timestamps.
 * Failure leaves record unchanged; calibration and stream ordering are external. */
bool syncboss_imu_decode_fifo(const void *data, size_t length, unsigned version,
                              const struct syncboss_imu_parameters *parameters,
                              struct syncboss_imu_record *record);

#endif
