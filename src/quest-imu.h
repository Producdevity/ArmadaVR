#ifndef ARMADA_QUEST_IMU_H
#define ARMADA_QUEST_IMU_H

#include "imu-calibration.h"
#include "imu-time-align.h"
#include "syncboss-imu.h"

struct quest_imu_timing {
    int64_t acceleration_offset_ns, gyroscope_offset_ns, max_gap_ns;
};

struct quest_imu_config {
    unsigned format;
    struct syncboss_imu_parameters parameters;
    int64_t max_age_ns;
    enum imu_rectification_frame frame;
    double accelerometer[9], gyroscope[9], device_from_imu[9];
    const struct imu_temperature_offset *accelerometer_offsets, *gyroscope_offsets;
    size_t accelerometer_offset_count, gyroscope_offset_count;
    const struct quest_imu_timing *timing;
};

enum quest_imu_transport {
    QUEST_IMU_TRANSPORT_NONE,
    QUEST_IMU_TRANSPORT_DIRECT,
    QUEST_IMU_TRANSPORT_FIFO,
};

struct quest_imu {
    bool initialized;
    unsigned format;
    struct syncboss_imu_parameters parameters;
    int64_t max_age_ns;
    struct imu_rectification rectification;
    const struct imu_temperature_offset *accelerometer_offsets, *gyroscope_offsets;
    size_t accelerometer_offset_count, gyroscope_offset_count;
    struct syncboss_imu_stream stream;
    struct imu_time_alignment alignment;
    enum quest_imu_transport transport;
};

struct quest_imu_sample {
    struct syncboss_imu_sample sample;
    int64_t timestamp_ns, capture_timestamp_ns, delivery_ns;
    uint32_t counter;
};

enum quest_imu_status {
    QUEST_IMU_ACCEPTED,
    QUEST_IMU_INVALID,
    QUEST_IMU_STALE,
    QUEST_IMU_OUT_OF_ORDER,
    QUEST_IMU_NEEDS_HISTORY,
};

/* Zero-initialize imu before first use. Supply exactly one unit calibration,
 * explicit MCU parameters and frame; no model is selected automatically.
 * Matrices are row-major. Offset tables retain raw sensor axes and must remain
 * alive and immutable until reinitialization. Configuration is otherwise copied.
 * Timing is optional for unaligned diagnostics; when supplied, corrections
 * are added to capture time before temperature compensation and rectification.
 * Success starts a new stream and history; failure preserves both. The caller
 * serializes initialization and sample processing. */
bool quest_imu_init(struct quest_imu *imu, const struct quest_imu_config *config);

/* The caller supplies one stable direct-channel record and CLOCK_MONOTONIC now.
 * Accepted vectors use the chosen calibrated IMU frame. The prepared
 * device_from_imu rotation remains in imu->rectification for the consumer;
 * this function applies no extrinsics or pose estimation. timestamp_ns is the
 * effective aligned time; capture_timestamp_ns, ticks, temperature, delivery
 * and counter belong to the current raw record. Both raw and effective times
 * must be no later than now and within max_age_ns.
 * NEEDS_HISTORY consumes the record and sets dropped but leaves sample intact.
 * Missing records or excessive capture gaps discard interpolation history.
 * All other rejections preserve stream, history, sample and dropped.
 * Ring synchronization and channel restarts remain the caller's responsibility.
 * Switching from FIFO input requires reinitialization. */
enum quest_imu_status quest_imu_process_direct(
    struct quest_imu *imu, const void *data, size_t length, int64_t now_ns,
    struct quest_imu_sample *sample, uint32_t *dropped);

/* Process one complete, copied FIFO packet-80 record with sequence zero.
 * received_ns is CLOCK_MONOTONIC immediately after the read; now_ns is the
 * consumption time. Output delivery_ns holds received_ns and counter is zero:
 * FIFO has no sample counter, so lost records cannot be counted. Alignment
 * history resets on excessive capture gaps, according to the configured timing.
 * Direct and FIFO input cannot be mixed without reinitializing the processor.
 * Rejection preserves state/output; NEEDS_HISTORY consumes only stream/history.
 * Opening, filtering, reading, framing and MCU restart detection belong to the
 * caller. A successful direct-channel subscriber can suppress FIFO delivery. */
enum quest_imu_status quest_imu_process_fifo(
    struct quest_imu *imu, const void *data, size_t length,
    int64_t received_ns, int64_t now_ns, struct quest_imu_sample *sample);

#endif
