#include "pico-controller.h"

#include <string.h>

static uint8_t crc8(const uint8_t *data, size_t length)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint8_t)((crc >> 1) ^ ((crc & 1) ? 0x8c : 0));
    }
    return crc;
}

bool pico_controller_decode(const void *record, size_t length, unsigned mcu_type,
                            unsigned radio_type, bool touch_supported,
                            struct pico_controller_sample *sample)
{
    if (!record || !sample || length != 100 || mcu_type < 1 || mcu_type > 2 ||
        radio_type < 1 || radio_type > 2)
        return false;
    const uint8_t *data = record;
    if (data[0] != 0xa5 || crc8(data, 99) != data[99] ||
        (mcu_type == 1 ? data[1] != 1 : !(data[1] & 4)))
        return false;

    struct pico_controller_sample result = {
        .hand = (data[7] & 0x40) ? 1 : 2,
        .buttons = data[7] & 0xbf,
        .touches = touch_supported ? data[8] & 0x1f : 0,
        .trigger = data[3],
        .grip = data[6],
        .stick_x = data[4],
        .stick_y = data[5],
        .battery_level = data[9] & 7,
    };
    if (radio_type == 1) {
        result.battery_level = 1;
        const uint8_t thresholds[] = {5, 25, 50, 75};
        for (unsigned i = 0; i < sizeof(thresholds); ++i)
            result.battery_level += data[9] > thresholds[i];
    }
    *sample = result;
    return true;
}

static bool varint(const uint8_t *data, size_t length, size_t *position,
                   uint64_t *value)
{
    *value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (*position == length)
            return false;
        uint8_t byte = data[(*position)++];
        if (shift == 63 && byte > 1)
            return false;
        *value |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            return true;
    }
    return false;
}

static bool decode_ssc_record(const void *payload, size_t length, uint8_t record[100],
                              unsigned *accuracy)
{
    if (!payload || length > 4096) return false;
    const uint8_t *data = payload;
    size_t position = 0, used = 0;
    unsigned status = 0;
    bool has_status = false;
    while (position < length) {
        uint64_t tag, value = 0;
        if (!varint(data, length, &position, &tag) || tag > UINT32_MAX ||
            (tag >> 3) == 0)
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
            if ((wire != 2 && wire != 5) || size % 4 || size > 100 - used)
                return false;
            memcpy(record + used, data + position, size);
            used += size;
        } else if (field == 2) {
            if (wire != 0 || value > 3)
                return false;
            status = (unsigned)value;
            has_status = true;
        }
        position += size;
    }
    if (!has_status || used != 100) return false;
    *accuracy = status;
    return true;
}

bool pico_controller_decode_ssc(const void *payload, size_t length,
                                unsigned mcu_type, unsigned radio_type,
                                bool touch_supported,
                                struct pico_controller_sample *sample,
                                unsigned *accuracy)
{
    uint8_t record[100]; unsigned status;
    struct pico_controller_sample result;
    if (!sample || !accuracy || !decode_ssc_record(payload, length, record, &status) ||
        !pico_controller_decode(record, sizeof(record), mcu_type, radio_type, touch_supported, &result))
        return false;
    *sample = result;
    *accuracy = status;
    return true;
}

static uint64_t read_u64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static int16_t read_i16(const uint8_t *p)
{
    int32_t value = p[0] | ((uint32_t)p[1] << 8);
    return (int16_t)(value >= 32768 ? value - 65536 : value);
}

bool pico_controller_decode_imu(const void *record, size_t length, unsigned mcu_type,
                                unsigned radio_type, unsigned connected_mask,
                                struct pico_controller_imu_batch *batch)
{
    if (!record || !batch || length != 100 || mcu_type < 1 || mcu_type > 2 ||
        radio_type < 1 || radio_type > 2 || !connected_mask || connected_mask > 3)
        return false;
    const uint8_t *data = record;
    if (data[0] != 0xa5 || !(data[1] & 1) || crc8(data, 99) != data[99])
        return false;
    const bool enabled[2] = {
        (mcu_type == 2 && (connected_mask & 2)) || radio_type == 1,
        (mcu_type == 2 && (connected_mask & 1)) || mcu_type == 1,
    };
    struct pico_controller_imu_batch result = {.station_time = read_u64(data + 56)};
    for (unsigned slot = 0; slot < 2; slot++) {
        const uint8_t *raw = data + 10 + slot * 23;
        uint64_t timestamp = read_u64(raw + 14);
        if (!enabled[slot] || !timestamp) continue;
        struct pico_controller_imu_sample *sample = &result.samples[result.count++];
        sample->timestamp_us = timestamp;
        sample->hand = mcu_type == 2 ? 2 - slot : (data[7] & 0x40 ? 1 : 2);
        sample->sequence = raw[22];
        for (unsigned i = 0; i < 3; i++) {
            sample->gyroscope[i] = read_i16(raw + i * 2);
            sample->acceleration[i] = read_i16(raw + 6 + i * 2);
        }
        sample->temperature = read_i16(raw + 12);
    }
    *batch = result;
    return true;
}

bool pico_controller_decode_imu_ssc(const void *payload, size_t length, unsigned mcu_type,
                                    unsigned radio_type, unsigned connected_mask,
                                    struct pico_controller_imu_batch *batch, unsigned *accuracy)
{
    uint8_t record[100]; unsigned status;
    struct pico_controller_imu_batch result;
    if (!batch || !accuracy || !decode_ssc_record(payload, length, record, &status) ||
        !pico_controller_decode_imu(record, sizeof(record), mcu_type, radio_type, connected_mask, &result))
        return false;
    *batch = result;
    *accuracy = status;
    return true;
}
