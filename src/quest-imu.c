#include "quest-imu.h"

bool quest_imu_init(struct quest_imu *imu, const struct quest_imu_config *config)
{
    if (!imu || !config || config->max_age_ns <= 0 ||
        !syncboss_imu_parameters_valid(config->format, &config->parameters))
        return false;
    struct quest_imu result = {0};
    if (config->timing && !imu_time_alignment_init(&result.alignment,
        config->timing->acceleration_offset_ns, config->timing->gyroscope_offset_ns,
        config->timing->max_gap_ns))
        return false;
    float offset[3];
    if (!imu_prepare_rectification(config->accelerometer, config->gyroscope,
                                   config->device_from_imu, config->frame, &result.rectification) ||
        !imu_offset_at_temperature(config->accelerometer_offsets,
                                    config->accelerometer_offset_count, 0, offset) ||
        !imu_offset_at_temperature(config->gyroscope_offsets,
                                    config->gyroscope_offset_count, 0, offset))
        return false;
    result.format = config->format;
    result.parameters = config->parameters;
    result.max_age_ns = config->max_age_ns;
    result.accelerometer_offsets = config->accelerometer_offsets;
    result.gyroscope_offsets = config->gyroscope_offsets;
    result.accelerometer_offset_count = config->accelerometer_offset_count;
    result.gyroscope_offset_count = config->gyroscope_offset_count;
    result.initialized = true;
    *imu = result;
    return true;
}

enum quest_imu_status quest_imu_process_direct(
    struct quest_imu *imu, const void *data, size_t length, int64_t now_ns,
    struct quest_imu_sample *sample, uint32_t *dropped)
{
    if (!imu || !imu->initialized || !sample || !dropped)
        return QUEST_IMU_INVALID;
    struct syncboss_imu_direct_record raw;
    if (!syncboss_imu_decode_direct(data, length, imu->format, &imu->parameters, &raw))
        return QUEST_IMU_INVALID;
    struct syncboss_imu_stream next = imu->stream;
    uint32_t missing;
    enum syncboss_imu_stream_status status = syncboss_imu_stream_accept(
        &next, &raw, now_ns, imu->max_age_ns, &missing);
    switch (status) {
    case SYNCBOSS_IMU_INVALID: return QUEST_IMU_INVALID;
    case SYNCBOSS_IMU_STALE: return QUEST_IMU_STALE;
    case SYNCBOSS_IMU_OUT_OF_ORDER: return QUEST_IMU_OUT_OF_ORDER;
    case SYNCBOSS_IMU_ACCEPTED: break;
    default: return QUEST_IMU_INVALID;
    }
    struct quest_imu_sample result = {
        .sample = raw.sample, .timestamp_ns = raw.timestamp_ns,
        .capture_timestamp_ns = raw.timestamp_ns,
        .delivery_ns = raw.delivery_ns, .counter = raw.counter,
    };
    struct imu_time_alignment alignment = imu->alignment;
    if (alignment.initialized) {
        if (missing)
            alignment.count = 0;
        struct imu_timed_sample input = {
            .timestamp_ns = raw.timestamp_ns, .temperature_c = raw.sample.temperature_c,
        }, aligned;
        for (unsigned axis = 0; axis < 3; ++axis) {
            input.acceleration[axis] = raw.sample.acceleration_m_s2[axis];
            input.angular_velocity[axis] = raw.sample.angular_velocity_rad_s[axis];
        }
        enum imu_time_alignment_status alignment_status = imu_time_align(&alignment, &input, &aligned);
        if (alignment_status == IMU_TIME_NEEDS_HISTORY) {
            imu->stream = next;
            imu->alignment = alignment;
            *dropped = missing;
            return QUEST_IMU_NEEDS_HISTORY;
        }
        if (alignment_status != IMU_TIME_ALIGNED)
            return QUEST_IMU_INVALID;
        if (aligned.timestamp_ns > now_ns)
            return QUEST_IMU_INVALID;
        if (now_ns - aligned.timestamp_ns > imu->max_age_ns)
            return QUEST_IMU_STALE;
        result.timestamp_ns = aligned.timestamp_ns;
        for (unsigned axis = 0; axis < 3; ++axis) {
            result.sample.acceleration_m_s2[axis] = aligned.acceleration[axis];
            result.sample.angular_velocity_rad_s[axis] = aligned.angular_velocity[axis];
        }
    }
    float accel_offset[3], gyro_offset[3];
    if (!imu_offset_at_temperature(imu->accelerometer_offsets, imu->accelerometer_offset_count,
                                    result.sample.temperature_c, accel_offset) ||
        !imu_offset_at_temperature(imu->gyroscope_offsets, imu->gyroscope_offset_count,
                                    result.sample.temperature_c, gyro_offset) ||
        !imu_rectify_vector(imu->rectification.accelerometer, accel_offset,
                            result.sample.acceleration_m_s2, result.sample.acceleration_m_s2) ||
        !imu_rectify_vector(imu->rectification.gyroscope, gyro_offset,
                            result.sample.angular_velocity_rad_s, result.sample.angular_velocity_rad_s))
        return QUEST_IMU_INVALID;
    *sample = result;
    *dropped = missing;
    imu->stream = next;
    imu->alignment = alignment;
    return QUEST_IMU_ACCEPTED;
}
