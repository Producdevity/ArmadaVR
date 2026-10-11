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

bool syncboss_imu_decode(const void *payload, size_t length, unsigned version,
                         const struct syncboss_imu_parameters *parameters,
                         struct syncboss_imu_sample *sample)
{
    if (!payload || !sample || version < 1 || version > 4 || length > 252)
        return false;
    bool integer_format = version == 2 || version == 3;
    if (length < (integer_format ? 22u : 36u))
        return false;
    if (integer_format &&
        (!parameters || !isfinite(parameters->lsb_per_dps) || parameters->lsb_per_dps <= 0 ||
         !isfinite(parameters->lsb_per_g) || parameters->lsb_per_g <= 0 ||
         !isfinite(parameters->lsb_per_c) || parameters->lsb_per_c < 1 ||
         parameters->lsb_per_c >= 0x1p31f || !isfinite(parameters->offset_c) ||
         parameters->offset_c < -0x1p31f || parameters->offset_c >= 0x1p31f))
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
