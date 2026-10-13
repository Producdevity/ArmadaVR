#include "ssc-calibration.h"

#include <float.h>
#include <math.h>
#include <string.h>

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
               FLT_MAX_EXP == 128, "SSC requires IEEE binary32");

static bool varint(const uint8_t *data, size_t length, size_t *position, uint64_t *value)
{
    *value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (*position == length) return false;
        uint8_t byte = data[(*position)++];
        if (shift == 63 && byte > 1) return false;
        *value |= (uint64_t)(byte & 127) << shift;
        if (!(byte & 128)) return true;
    }
    return false;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool ssc_calibration_decode(const void *payload, size_t length, struct ssc_calibration_value *value)
{
    if (!payload || !value || length > 4096) return false;
    const uint8_t *data = payload;
    struct ssc_calibration_value decoded = {0};
    float *arrays[] = {decoded.bias, decoded.scale, decoded.matrix};
    const size_t limits[] = {3, 3, 9};
    size_t position = 0, counts[3] = {0};
    bool has_accuracy = false;
    while (position < length) {
        uint64_t tag, number = 0;
        if (!varint(data, length, &position, &tag) || tag > UINT32_MAX || !(tag >> 3)) return false;
        unsigned field = (unsigned)(tag >> 3), wire = tag & 7;
        size_t size = 0;
        if (wire == 0) {
            if (!varint(data, length, &position, &number)) return false;
        } else if (wire == 1) size = 8;
        else if (wire == 2) {
            if (!varint(data, length, &position, &number) || number > length - position) return false;
            size = (size_t)number;
        } else if (wire == 5) size = 4;
        else return false;
        if (size > length - position) return false;
        if (field <= 3) {
            unsigned index = field - 1;
            if ((wire != 2 && wire != 5) || size % 4 || size / 4 > limits[index] - counts[index]) return false;
            for (size_t offset = 0; offset < size; offset += 4) {
                uint32_t bits = le32(data + position + offset);
                float *destination = &arrays[index][counts[index]++];
                memcpy(destination, &bits, sizeof(bits));
                if (!isfinite(*destination)) return false;
            }
        } else if (field == 4) {
            if (wire != 0 || number > 3) return false;
            decoded.accuracy = (unsigned)number;
            has_accuracy = true;
        } else if (field == 5) {
            if (wire != 5) return false;
            decoded.cal_id = le32(data + position);
            decoded.has_cal_id = true;
        }
        position += size;
    }
    if (counts[0] != 3 || (counts[1] && counts[1] != 3) ||
        (counts[2] && counts[2] != 9) || !has_accuracy) return false;
    decoded.has_scale = counts[1] != 0;
    decoded.has_matrix = counts[2] != 0;
    *value = decoded;
    return true;
}

bool ssc_calibration_begin(struct ssc_calibration *calibration,
                           const struct ssc_calibration_config *config, uint64_t generation)
{
    if (!calibration || !config || generation <= calibration->generation ||
        !config->clock.initialized || !config->clock.counter_hz || config->clock.host_ns < 0 ||
        config->clock.uncertainty_ns < 2 || config->max_report_age_ns < 0 || config->max_bias_age_ns <= 0 ||
        (!config->suid_high[0] && !config->suid_low[0]) || (!config->suid_high[1] && !config->suid_low[1]) ||
        (config->suid_high[0] == config->suid_high[1] && config->suid_low[0] == config->suid_low[1])) return false;
    *calibration = (struct ssc_calibration){.config = *config, .generation = generation, .active = true};
    return true;
}

void ssc_calibration_invalidate(struct ssc_calibration *calibration)
{
    if (calibration) calibration->active = false;
}

bool ssc_calibration_report(struct ssc_calibration *calibration, uint64_t generation,
                            uint32_t id, uint64_t high, uint64_t low, uint64_t ticks,
                            const void *payload, size_t length, int64_t now_ns)
{
    if (!calibration || !calibration->active || generation != calibration->generation ||
        id != 1022 || now_ns < calibration->last_received_ns) return false;
    unsigned sensor;
    for (sensor = 0; sensor < 2; sensor++)
        if (high == calibration->config.suid_high[sensor] && low == calibration->config.suid_low[sensor]) break;
    if (sensor == 2) return false;
    unsigned count = calibration->count[sensor];
    struct ssc_calibration_entry entry = {.ticks = ticks};
    struct ssc_timestamp time;
    if ((count && ticks <= calibration->entries[sensor][count - 1].ticks) ||
        !ssc_calibration_decode(payload, length, &entry.value) ||
        !ssc_clock_map(&calibration->config.clock, ticks, now_ns, calibration->config.max_report_age_ns, &time)) return false;
    if (count == SSC_CALIBRATION_HISTORY) {
        memmove(calibration->entries[sensor], calibration->entries[sensor] + 1,
                (count - 1) * sizeof(entry));
        count--;
    }
    calibration->entries[sensor][count] = entry;
    calibration->count[sensor] = count + 1;
    calibration->last_received_ns = now_ns;
    return true;
}

bool ssc_calibration_correct(const struct ssc_calibration *calibration,
                             const struct ssc_imu_event *sample, int64_t now_ns,
                             struct ssc_bias_sample *output)
{
    if (!calibration || !sample || !output || !calibration->active ||
        sample->generation != calibration->generation || (unsigned)sample->sensor > 1 ||
        sample->vector.accuracy == 0 || sample->vector.accuracy > 3) return false;
    struct ssc_timestamp time;
    if (!ssc_clock_map(&calibration->config.clock, sample->ticks, now_ns,
                      calibration->config.max_report_age_ns, &time) ||
        time.timestamp_ns != sample->time.timestamp_ns ||
        time.uncertainty_ns != sample->time.uncertainty_ns) return false;
    const struct ssc_calibration_entry *entry = NULL;
    for (unsigned i = 0; i < calibration->count[sample->sensor]; i++) {
        if (calibration->entries[sample->sensor][i].ticks > sample->ticks) break;
        entry = &calibration->entries[sample->sensor][i];
    }
    if (!entry || !entry->value.accuracy || entry->value.accuracy > 3 ||
        ((__int128)(sample->ticks - entry->ticks) * 1000000000 + 19199999) / 19200000 >
            calibration->config.max_bias_age_ns) return false;
    struct ssc_bias_sample corrected = {.raw = *sample, .calibration = *entry,
        .accuracy = sample->vector.accuracy < entry->value.accuracy ? sample->vector.accuracy : entry->value.accuracy};
    for (unsigned i = 0; i < 3; i++) {
        if (!isfinite(sample->vector.values[i])) return false;
        corrected.corrected[i] = sample->vector.values[i] - entry->value.bias[i];
        if (!isfinite(corrected.corrected[i])) return false;
    }
    *output = corrected;
    return true;
}
