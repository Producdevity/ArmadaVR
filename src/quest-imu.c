#include "quest-imu.h"

bool quest_imu_init(struct quest_imu *imu, const struct quest_imu_config *config)
{
    if (!imu || !config || config->max_age_ns <= 0 ||
        !syncboss_imu_parameters_valid(config->format, &config->parameters))
        return false;
    struct quest_imu result = {0};
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

enum syncboss_imu_stream_status quest_imu_process_direct(
    struct quest_imu *imu, const void *data, size_t length, int64_t now_ns,
    struct syncboss_imu_direct_record *sample, uint32_t *dropped)
{
    if (!imu || !imu->initialized || !sample || !dropped)
        return SYNCBOSS_IMU_INVALID;
    struct syncboss_imu_direct_record result;
    if (!syncboss_imu_decode_direct(data, length, imu->format, &imu->parameters, &result))
        return SYNCBOSS_IMU_INVALID;
    struct syncboss_imu_stream next = imu->stream;
    uint32_t missing;
    enum syncboss_imu_stream_status status = syncboss_imu_stream_accept(
        &next, &result, now_ns, imu->max_age_ns, &missing);
    if (status != SYNCBOSS_IMU_ACCEPTED)
        return status;
    float accel_offset[3], gyro_offset[3];
    if (!imu_offset_at_temperature(imu->accelerometer_offsets, imu->accelerometer_offset_count,
                                    result.sample.temperature_c, accel_offset) ||
        !imu_offset_at_temperature(imu->gyroscope_offsets, imu->gyroscope_offset_count,
                                    result.sample.temperature_c, gyro_offset) ||
        !imu_rectify_vector(imu->rectification.accelerometer, accel_offset,
                            result.sample.acceleration_m_s2, result.sample.acceleration_m_s2) ||
        !imu_rectify_vector(imu->rectification.gyroscope, gyro_offset,
                            result.sample.angular_velocity_rad_s, result.sample.angular_velocity_rad_s))
        return SYNCBOSS_IMU_INVALID;
    *sample = result;
    *dropped = missing;
    imu->stream = next;
    return SYNCBOSS_IMU_ACCEPTED;
}
