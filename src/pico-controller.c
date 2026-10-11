#include "pico-controller.h"

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
