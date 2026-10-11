#ifndef ARMADA_QUEST_IMU_H
#define ARMADA_QUEST_IMU_H

#include "imu-calibration.h"
#include "syncboss-imu.h"

struct quest_imu_config {
    unsigned format;
    struct syncboss_imu_parameters parameters;
    int64_t max_age_ns;
    enum imu_rectification_frame frame;
    double accelerometer[9], gyroscope[9], device_from_imu[9];
    const struct imu_temperature_offset *accelerometer_offsets, *gyroscope_offsets;
    size_t accelerometer_offset_count, gyroscope_offset_count;
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
};

/* Zero-initialize imu before first use. Supply exactly one unit calibration,
 * explicit MCU parameters and frame; no model is selected automatically.
 * Matrices are row-major. Offset tables retain raw sensor axes and must remain
 * alive and immutable until reinitialization. Configuration is otherwise copied.
 * Success starts a new stream; failure preserves the previous configuration
 * and stream. The caller serializes initialization and sample processing. */
bool quest_imu_init(struct quest_imu *imu, const struct quest_imu_config *config);

/* The caller supplies one stable direct-channel record and CLOCK_MONOTONIC now.
 * Accepted vectors use the chosen calibrated IMU frame. The prepared
 * device_from_imu rotation remains in imu->rectification for the consumer;
 * this function applies no extrinsics, time correction or pose estimation.
 * Rejection preserves stream, sample and dropped, including calibration errors.
 * Ring synchronization and channel restarts remain the caller's responsibility. */
enum syncboss_imu_stream_status quest_imu_process_direct(
    struct quest_imu *imu, const void *data, size_t length, int64_t now_ns,
    struct syncboss_imu_direct_record *sample, uint32_t *dropped);

#endif
