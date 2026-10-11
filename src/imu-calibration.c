#include "imu-calibration.h"

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
