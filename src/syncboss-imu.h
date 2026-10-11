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

#endif
