#include "imu-calibration.h"

#include <float.h>
#include <math.h>

bool imu_offset_at_temperature(const struct imu_temperature_offset *rows, size_t count,
                               float temperature, float offset[3])
{
    if (!rows || !offset || count == 0 || count > 1024 || !isfinite(temperature))
        return false;
    for (size_t i = 0; i < count; ++i) {
        if (!isfinite(rows[i].temperature) || (i && rows[i - 1].temperature >= rows[i].temperature))
            return false;
        for (unsigned axis = 0; axis < 3; ++axis)
            if (!isfinite(rows[i].offset[axis]))
                return false;
    }
    size_t upper = 0;
    while (upper < count && rows[upper].temperature < temperature)
        ++upper;
    float result[3];
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (upper == 0)
            result[axis] = rows[0].offset[axis];
        else if (upper == count)
            result[axis] = rows[count - 1].offset[axis];
        else if (temperature == rows[upper].temperature)
            result[axis] = rows[upper].offset[axis];
        else {
            float delta = temperature - rows[upper - 1].temperature;
            float span = rows[upper].temperature - rows[upper - 1].temperature;
            float change = rows[upper].offset[axis] - rows[upper - 1].offset[axis];
            if (!isfinite(delta) || !isfinite(span) || !isfinite(change))
                return false;
            result[axis] = rows[upper - 1].offset[axis] + change * delta / span;
        }
        if (!isfinite(result[axis]))
            return false;
    }
    for (unsigned axis = 0; axis < 3; ++axis)
        offset[axis] = result[axis];
    return true;
}

bool imu_rectify_vector(const float rectification[9], const float offset[3],
                        const float raw[3], float calibrated[3])
{
    if (!rectification || !offset || !raw || !calibrated)
        return false;
    float centered[3], result[3] = {0};
    for (unsigned i = 0; i < 9; ++i)
        if (!isfinite(rectification[i]))
            return false;
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (!isfinite(offset[axis]) || !isfinite(raw[axis]))
            return false;
        centered[axis] = raw[axis] - offset[axis];
        if (!isfinite(centered[axis]))
            return false;
    }
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned column = 0; column < 3; ++column)
            result[row] += rectification[row * 3 + column] * centered[column];
        if (!isfinite(result[row]))
            return false;
    }
    for (unsigned axis = 0; axis < 3; ++axis)
        calibrated[axis] = result[axis];
    return true;
}

static double dot3(const double left[3], const double right[3])
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

bool imu_split_rectification(const double rectification[9], double rotation[9],
                             double correction[9])
{
    if (!rectification || !rotation || !correction || rotation == correction)
        return false;
    double scale = 0;
    for (unsigned i = 0; i < 9; ++i) {
        if (!isfinite(rectification[i]))
            return false;
        scale = fmax(scale, fabs(rectification[i]));
    }
    if (scale == 0)
        return false;
    double columns[3][3], q[3][3], r[9] = {0};
    for (unsigned column = 0; column < 3; ++column)
        for (unsigned row = 0; row < 3; ++row)
            columns[column][row] = rectification[row * 3 + column] / scale;
    r[0] = hypot(hypot(columns[0][0], columns[0][1]), columns[0][2]);
    if (r[0] <= 1e-12)
        return false;
    for (unsigned i = 0; i < 3; ++i)
        q[0][i] = columns[0][i] / r[0];
    r[1] = dot3(q[0], columns[1]);
    for (unsigned i = 0; i < 3; ++i)
        q[1][i] = columns[1][i] - q[0][i] * r[1];
    double residual = dot3(q[0], q[1]);
    r[1] += residual;
    for (unsigned i = 0; i < 3; ++i)
        q[1][i] -= q[0][i] * residual;
    r[4] = hypot(hypot(q[1][0], q[1][1]), q[1][2]);
    if (r[4] <= 1e-12)
        return false;
    for (unsigned i = 0; i < 3; ++i)
        q[1][i] /= r[4];
    q[2][0] = q[0][1] * q[1][2] - q[0][2] * q[1][1];
    q[2][1] = q[0][2] * q[1][0] - q[0][0] * q[1][2];
    q[2][2] = q[0][0] * q[1][1] - q[0][1] * q[1][0];
    double norm = hypot(hypot(q[2][0], q[2][1]), q[2][2]);
    if (norm <= 1e-12)
        return false;
    for (unsigned i = 0; i < 3; ++i)
        q[2][i] /= norm;
    r[2] = dot3(q[0], columns[2]);
    r[5] = dot3(q[1], columns[2]);
    r[8] = dot3(q[2], columns[2]);
    if (r[8] <= 1e-12)
        return false;
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned column = 0; column < 3; ++column) {
            double value = 0;
            for (unsigned k = 0; k < 3; ++k)
                value += q[k][row] * r[k * 3 + column];
            if (!isfinite(value) || fabs(value - columns[column][row]) > 1e-12)
                return false;
        }
    }
    for (unsigned i = 0; i < 9; ++i) {
        r[i] *= scale;
        if (!isfinite(r[i]))
            return false;
    }
    if (r[0] <= 0 || r[4] <= 0 || r[8] <= 0)
        return false;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column) {
            rotation[row * 3 + column] = q[column][row];
            correction[row * 3 + column] = r[row * 3 + column];
        }
    return true;
}

static bool proper_rotation(const double matrix[9])
{
    for (unsigned i = 0; i < 9; ++i)
        if (!isfinite(matrix[i]) || fabs(matrix[i]) > 1 + 1e-10)
            return false;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column) {
            double dot = 0;
            for (unsigned k = 0; k < 3; ++k)
                dot += matrix[row * 3 + k] * matrix[column * 3 + k];
            if (fabs(dot - (row == column ? 1 : 0)) > 1e-10)
                return false;
        }
    double determinant = matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7])
                       - matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6])
                       + matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
    return fabs(determinant - 1) <= 1e-10;
}

static bool matrix_to_float(const double matrix[9], float result[9])
{
    for (unsigned i = 0; i < 9; ++i) {
        if (!isfinite(matrix[i]) || fabs(matrix[i]) > FLT_MAX)
            return false;
        result[i] = (float)matrix[i];
        if (matrix[i] != 0 && result[i] == 0)
            return false;
    }
    return true;
}

bool imu_prepare_rectification(const double accelerometer[9], const double gyroscope[9],
                               const double device_from_imu[9], enum imu_rectification_frame frame,
                               struct imu_rectification *result)
{
    if (!accelerometer || !gyroscope || !device_from_imu || !result ||
        (frame != IMU_FRAME_ORIGINAL && frame != IMU_FRAME_ACCELEROMETER) ||
        !proper_rotation(device_from_imu))
        return false;
    double qa[9], ra[9], qg[9], rg[9];
    if (!imu_split_rectification(accelerometer, qa, ra) ||
        !imu_split_rectification(gyroscope, qg, rg))
        return false;
    struct imu_rectification prepared = {0};
    if (frame == IMU_FRAME_ORIGINAL) {
        if (!matrix_to_float(accelerometer, prepared.accelerometer) ||
            !matrix_to_float(gyroscope, prepared.gyroscope))
            return false;
        for (unsigned i = 0; i < 9; ++i)
            prepared.device_from_imu[i] = device_from_imu[i];
    } else {
        double aligned_gyro[9] = {0};
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned column = 0; column < 3; ++column)
                for (unsigned k = 0; k < 3; ++k) {
                    aligned_gyro[row * 3 + column] += qa[k * 3 + row] * gyroscope[k * 3 + column];
                    prepared.device_from_imu[row * 3 + column] +=
                        device_from_imu[row * 3 + k] * qa[k * 3 + column];
                }
        if (!matrix_to_float(ra, prepared.accelerometer) ||
            !matrix_to_float(aligned_gyro, prepared.gyroscope) ||
            !proper_rotation(prepared.device_from_imu))
            return false;
    }
    *result = prepared;
    return true;
}
