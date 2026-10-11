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

/* Floating-point formats (1 and 4) do not use the integer conversion parameters. */
bool syncboss_imu_parameters_valid(unsigned version,
                                   const struct syncboss_imu_parameters *parameters);

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

struct syncboss_imu_direct_record {
    struct syncboss_imu_sample sample;
    int64_t timestamp_ns;
    int64_t delivery_ns;
    uint32_t counter;
};

/* Decode one stable 104-byte Syncboss driver direct-channel record, not an
 * Android converted sensor event. The caller owns ring synchronization and
 * must copy only published records. Counter zero and invalid NSYNC are rejected.
 * Both timestamps use CLOCK_MONOTONIC; delivery is not sample capture time.
 * Failure leaves record unchanged. Stream ordering and offset freshness are
 * the receiver's responsibility. */
bool syncboss_imu_decode_direct(const void *data, size_t length, unsigned version,
                                const struct syncboss_imu_parameters *parameters,
                                struct syncboss_imu_direct_record *record);

struct syncboss_imu_stream {
    bool initialized;
    uint32_t counter;
    uint64_t timestamp_ticks;
    int64_t timestamp_ns;
    int64_t delivery_ns;
};

enum syncboss_imu_stream_status {
    SYNCBOSS_IMU_ACCEPTED,
    SYNCBOSS_IMU_INVALID,
    SYNCBOSS_IMU_STALE,
    SYNCBOSS_IMU_OUT_OF_ORDER,
};

/* Zero-initialize a new channel's stream. now_ns must use CLOCK_MONOTONIC.
 * max_age_ns bounds capture-to-consumption age; it must be positive. The caller
 * serializes access. Accepted counter gaps report lost records, including across
 * UINT32_MAX -> 1. Rejected records change neither stream nor dropped. A new
 * channel or MCU reset requires an explicit stream reset by the caller. */
enum syncboss_imu_stream_status syncboss_imu_stream_accept(
    struct syncboss_imu_stream *stream, const struct syncboss_imu_direct_record *record,
    int64_t now_ns, int64_t max_age_ns, uint32_t *dropped);

#endif
