#include "ssc-imu.h"

#include <float.h>
#include <math.h>
#include <string.h>

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
               FLT_MAX_EXP == 128, "SSC requires IEEE binary32");

static bool varint(const uint8_t *data, size_t length, size_t *position, uint64_t *value)
{
    *value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (*position == length)
            return false;
        uint8_t byte = data[(*position)++];
        if (shift == 63 && byte > 1)
            return false;
        *value |= (uint64_t)(byte & 127) << shift;
        if (!(byte & 128))
            return true;
    }
    return false;
}

bool ssc_imu_decode(const void *payload, size_t length, struct ssc_imu_vector *vector)
{
    if (!payload || !vector || length > 4096)
        return false;
    const uint8_t *data = payload;
    struct ssc_imu_vector decoded = {0};
    size_t position = 0, count = 0;
    bool has_accuracy = false;
    while (position < length) {
        uint64_t tag, value = 0;
        if (!varint(data, length, &position, &tag) || tag > UINT32_MAX || !(tag >> 3))
            return false;
        unsigned field = (unsigned)(tag >> 3), wire = tag & 7;
        size_t size = 0;
        if (wire == 0) {
            if (!varint(data, length, &position, &value))
                return false;
        } else if (wire == 1) {
            size = 8;
        } else if (wire == 2) {
            if (!varint(data, length, &position, &value) || value > length - position)
                return false;
            size = (size_t)value;
        } else if (wire == 5) {
            size = 4;
        } else {
            return false;
        }
        if (size > length - position)
            return false;
        if (field == 1) {
            if ((wire != 2 && wire != 5) || size % 4 || size / 4 > 3 - count)
                return false;
            for (size_t offset = 0; offset < size; offset += 4) {
                const uint8_t *p = data + position + offset;
                uint32_t bits = (uint32_t)p[0] | (uint32_t)p[1] << 8 |
                                (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
                memcpy(&decoded.values[count], &bits, sizeof(bits));
                if (!isfinite(decoded.values[count]))
                    return false;
                count++;
            }
        } else if (field == 2) {
            if (wire != 0 || value > 3)
                return false;
            decoded.accuracy = (unsigned)value;
            has_accuracy = true;
        }
        position += size;
    }
    if (count != 3 || !has_accuracy)
        return false;
    *vector = decoded;
    return true;
}

bool ssc_imu_begin(struct ssc_imu_stream *stream, const struct ssc_imu_config *config)
{
    if (!stream || !config || stream->generation == UINT64_MAX || config->max_age_ns < 0 ||
        !config->clock.initialized || !config->clock.counter_hz || config->clock.host_ns < 0 ||
        config->clock.uncertainty_ns < 2 || (!config->suid_high[0] && !config->suid_low[0]) ||
        (!config->suid_high[1] && !config->suid_low[1]) ||
        (config->suid_high[0] == config->suid_high[1] && config->suid_low[0] == config->suid_low[1]))
        return false;
    *stream = (struct ssc_imu_stream){
        .config = *config,
        .generation = stream->generation + 1,
        .active = true,
    };
    return true;
}

void ssc_imu_invalidate(struct ssc_imu_stream *stream)
{
    if (stream)
        stream->active = false;
}

bool ssc_imu_report(struct ssc_imu_stream *stream, uint64_t generation, uint32_t id,
                    uint64_t high, uint64_t low, uint64_t ticks, const void *payload,
                    size_t length, int64_t now_ns, struct ssc_imu_event *event)
{
    if (!stream || !event || !stream->active || generation != stream->generation ||
        id != 1025 || now_ns < stream->last_received_ns)
        return false;
    unsigned sensor;
    for (sensor = 0; sensor < 2; sensor++)
        if (high == stream->config.suid_high[sensor] && low == stream->config.suid_low[sensor])
            break;
    if (sensor == 2)
        return false;
    struct ssc_imu_event decoded = {.sensor = (enum ssc_imu_sensor)sensor,
                                   .generation = generation, .ticks = ticks};
    if (!ssc_imu_decode(payload, length, &decoded.vector) ||
        !ssc_clock_map(&stream->config.clock, ticks, now_ns, stream->config.max_age_ns, &decoded.time) ||
        (stream->seen[sensor] && (ticks <= stream->last_ticks[sensor] ||
                                 decoded.time.timestamp_ns <= stream->last_time_ns[sensor])))
        return false;
    stream->seen[sensor] = true;
    stream->last_ticks[sensor] = ticks;
    stream->last_time_ns[sensor] = decoded.time.timestamp_ns;
    stream->last_received_ns = now_ns;
    *event = decoded;
    return true;
}
