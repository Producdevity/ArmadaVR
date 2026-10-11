#include "syncboss-imu.h"

#include <float.h>
#include <math.h>
#include <string.h>

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24,
               "Syncboss requires binary32 floats");

static uint64_t read_le(const uint8_t *data, unsigned count)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= (uint64_t)data[i] << (8 * i);
    return value;
}

static float read_float(const uint8_t *data)
{
    uint32_t bits = (uint32_t)read_le(data, 4);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static float read_int16(const uint8_t *data)
{
    int32_t value = (int32_t)read_le(data, 2);
    return (float)(value >= 32768 ? value - 65536 : value);
}

static float compensate(float value, float delta, float coefficient)
{
    /* The stock routine rounds each operation separately, without fused multiply-add. */
    volatile float correction = delta * coefficient;
    correction = correction * 0x1p-17f;
    correction = correction + 1.0f;
    return value * correction;
}

bool syncboss_imu_parameters_valid(unsigned version,
                                   const struct syncboss_imu_parameters *parameters)
{
    if (version < 1 || version > 4)
        return false;
    bool integer_format = version == 2 || version == 3;
    if (integer_format &&
        (!parameters || !isfinite(parameters->lsb_per_dps) || parameters->lsb_per_dps <= 0 ||
         !isfinite(parameters->lsb_per_g) || parameters->lsb_per_g <= 0 ||
         !isfinite(parameters->lsb_per_c) || parameters->lsb_per_c < 1 ||
         parameters->lsb_per_c >= 0x1p31f || !isfinite(parameters->offset_c) ||
         parameters->offset_c < -0x1p31f || parameters->offset_c >= 0x1p31f))
        return false;
    return true;
}

bool syncboss_imu_decode(const void *payload, size_t length, unsigned version,
                         const struct syncboss_imu_parameters *parameters,
                         struct syncboss_imu_sample *sample)
{
    if (!payload || !sample || !syncboss_imu_parameters_valid(version, parameters) || length > 252)
        return false;
    bool integer_format = version == 2 || version == 3;
    if (length < (integer_format ? 22u : 36u))
        return false;

    const uint8_t *data = payload;
    struct syncboss_imu_sample result = {0};
    uint64_t stamp = read_le(data, 8);
    result.timestamp_ticks = version >= 3 ? stamp >> 16 : stamp;
    result.device_id = version >= 3 ? stamp & 15 : 0;
    const double radians_per_degree = 0.017453292519943295;
    if (integer_format) {
        /* Firmware truncates both temperature conversion parameters to signed int32. */
        result.temperature_c = read_int16(data + 20) / (float)(int32_t)parameters->lsb_per_c;
        result.temperature_c += (float)(int32_t)parameters->offset_c;
        for (unsigned axis = 0; axis < 3; ++axis) {
            float accel = read_int16(data + 8 + axis * 2) / parameters->lsb_per_g;
            float gyro = read_int16(data + 14 + axis * 2) / parameters->lsb_per_dps;
            result.acceleration_m_s2[axis] = accel * 9.80665f;
            result.angular_velocity_rad_s[axis] = (float)((double)gyro * radians_per_degree);
        }
        if (parameters->temperature_compensation) {
            const float accel_coefficients[3] = {-27, -27, -15};
            const float gyro_coefficients[3] = {-22, -19, -6};
            float delta = result.temperature_c - 20.0f;
            for (unsigned axis = 0; axis < 3; ++axis) {
                result.acceleration_m_s2[axis] = compensate(
                    result.acceleration_m_s2[axis], delta, accel_coefficients[axis]);
                result.angular_velocity_rad_s[axis] = compensate(
                    result.angular_velocity_rad_s[axis], delta, gyro_coefficients[axis]);
            }
        }
    } else {
        result.temperature_c = read_float(data + 32);
        for (unsigned axis = 0; axis < 3; ++axis) {
            result.acceleration_m_s2[axis] = read_float(data + 8 + axis * 4) * 9.80665f;
            result.angular_velocity_rad_s[axis] =
                (float)((double)read_float(data + 20 + axis * 4) * radians_per_degree);
        }
    }
    if (!isfinite(result.temperature_c))
        return false;
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!isfinite(result.acceleration_m_s2[axis]) ||
            !isfinite(result.angular_velocity_rad_s[axis]))
            return false;
    *sample = result;
    return true;
}

static unsigned driver_header_length(const uint8_t *bytes, size_t length)
{
    if (length < 2)
        return 0;
    unsigned header_length = bytes[0] == 2 ? 12 : bytes[0] == 3 ? 21 : 0;
    if (!header_length || bytes[1] != header_length || length < header_length)
        return 0;
    if (bytes[2] != 0 || bytes[3] != 1 || (bytes[0] == 3 && bytes[12] > 2))
        return 0;
    return header_length;
}

static bool sample_time(uint64_t timestamp, uint64_t offset, int64_t *timestamp_ns)
{
    if (offset & (UINT64_C(1) << 63)) {
        uint64_t magnitude = ~offset + 1;
        if (timestamp < magnitude)
            return false;
        timestamp -= magnitude;
    } else {
        if (timestamp > UINT64_MAX - offset)
            return false;
        timestamp += offset;
    }
    if (timestamp > INT64_MAX / 1000)
        return false;
    *timestamp_ns = (int64_t)timestamp * 1000;
    return true;
}

bool syncboss_imu_decode_fifo(const void *data, size_t length, unsigned version,
                              const struct syncboss_imu_parameters *parameters,
                              struct syncboss_imu_record *record)
{
    if (!data || !record || length > 276)
        return false;
    const uint8_t *bytes = data;
    unsigned header_length = driver_header_length(bytes, length);
    if (!header_length || length < header_length + 3)
        return false;
    const uint8_t *packet = bytes + header_length;
    if (packet[0] != 80 || packet[2] > 252 || length != header_length + 3u + packet[2])
        return false;
    struct syncboss_imu_record result = {0};
    if (!syncboss_imu_decode(packet + 3, packet[2], version, parameters, &result.sample) ||
        result.sample.device_id != 0 ||
        !sample_time(result.sample.timestamp_ticks, read_le(bytes + 4, 8), &result.timestamp_ns))
        return false;
    result.sequence = packet[1];
    *record = result;
    return true;
}

bool syncboss_imu_decode_direct(const void *data, size_t length, unsigned version,
                                const struct syncboss_imu_parameters *parameters,
                                struct syncboss_imu_direct_record *record)
{
    if (!data || !record || length != 104)
        return false;
    const uint8_t *bytes = data;
    if (read_le(bytes, 4) != 104 || read_le(bytes + 8, 4) != 80 ||
        read_le(bytes + 12, 4) == 0 || read_le(bytes + 16, 8) > INT64_MAX)
        return false;
    const uint8_t *header = bytes + 24;
    unsigned header_length = driver_header_length(header, 64);
    if (!header_length)
        return false;
    struct syncboss_imu_direct_record result = {0};
    if (!syncboss_imu_decode(header + header_length, 64 - header_length, version,
                            parameters, &result.sample) || result.sample.device_id != 0 ||
        !sample_time(result.sample.timestamp_ticks, read_le(header + 4, 8), &result.timestamp_ns))
        return false;
    result.delivery_ns = (int64_t)read_le(bytes + 16, 8);
    result.counter = (uint32_t)read_le(bytes + 12, 4);
    *record = result;
    return true;
}

enum syncboss_imu_stream_status syncboss_imu_stream_accept(
    struct syncboss_imu_stream *stream, const struct syncboss_imu_direct_record *record,
    int64_t now_ns, int64_t max_age_ns, uint32_t *dropped)
{
    if (!stream || !record || !dropped || max_age_ns <= 0 || now_ns < 0 ||
        record->counter == 0 || record->sample.device_id != 0 ||
        record->timestamp_ns < 0 || record->delivery_ns < record->timestamp_ns ||
        now_ns < record->delivery_ns || !isfinite(record->sample.temperature_c))
        return SYNCBOSS_IMU_INVALID;
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!isfinite(record->sample.acceleration_m_s2[axis]) ||
            !isfinite(record->sample.angular_velocity_rad_s[axis]))
            return SYNCBOSS_IMU_INVALID;
    if (now_ns - record->timestamp_ns > max_age_ns)
        return SYNCBOSS_IMU_STALE;
    uint32_t missing = 0;
    if (stream->initialized) {
        uint64_t distance = record->counter >= stream->counter ?
            record->counter - stream->counter :
            (uint64_t)UINT32_MAX - stream->counter + record->counter;
        if (distance == 0 || distance >= UINT64_C(0x80000000) ||
            record->sample.timestamp_ticks <= stream->timestamp_ticks ||
            record->timestamp_ns <= stream->timestamp_ns ||
            record->delivery_ns < stream->delivery_ns)
            return SYNCBOSS_IMU_OUT_OF_ORDER;
        missing = (uint32_t)(distance - 1);
    }
    stream->initialized = true;
    stream->counter = record->counter;
    stream->timestamp_ticks = record->sample.timestamp_ticks;
    stream->timestamp_ns = record->timestamp_ns;
    stream->delivery_ns = record->delivery_ns;
    *dropped = missing;
    return SYNCBOSS_IMU_ACCEPTED;
}
