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

bool pico_controller_decode_ssc(const void *payload, size_t length,
                                unsigned mcu_type, unsigned radio_type,
                                bool touch_supported,
                                struct pico_controller_sample *sample,
                                unsigned *accuracy)
{
    if (!payload || !sample || !accuracy || length > 4096)
        return false;
    const uint8_t *data = payload;
    uint8_t record[100];
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
            if ((wire != 2 && wire != 5) || size % 4 || size > sizeof(record) - used)
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
    struct pico_controller_sample result;
    if (!has_status || used != sizeof(record) ||
        !pico_controller_decode(record, used, mcu_type, radio_type, touch_supported, &result))
        return false;
    *sample = result;
    *accuracy = status;
    return true;
}
