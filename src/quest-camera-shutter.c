#include "quest-camera-shutter.h"
#include <string.h>

static uint64_t read_le(const uint8_t *p, unsigned count)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

bool quest_camera_shutter_decode(const void *payload, size_t length, uint32_t camera_bit,
                                  struct quest_camera_shutter *shutter)
{
    if (!payload || !shutter || !camera_bit || (camera_bit & (camera_bit - 1u)) ||
        length < 89 || length > 213) return false;
    const uint8_t *p = payload;
    if (p[0] != 6 && p[0] != 21 && p[0] != 22) return false;
    uint32_t mask = (uint32_t)read_le(p + 1, 4);
    if (!(mask & camera_bit)) return false;
    unsigned count = 0, rank = 0;
    for (uint32_t bit = 1; bit; bit <<= 1) {
        if (mask & bit) {
            ++count;
            if (bit < camera_bit) ++rank;
        }
    }
    unsigned extra = p[88];
    if (extra >= count || length != 89u + 4u * extra) return false;
    const uint8_t *exposure = p + (rank && rank <= extra ? 89u + 4u * (rank - 1u) : 18u);
    struct quest_camera_shutter result = {
        .timestamp_us = read_le(p + 6, 8), .sof_us = read_le(p + 24, 8),
        .camera_mask = mask, .frame_counter = (uint32_t)read_le(p + 14, 4),
        .exposure_us = (uint16_t)read_le(exposure, 2),
        .gain_code = (uint16_t)read_le(exposure + 2, 2),
        .type = p[0], .tag = p[5], .auxiliary_tag = p[22],
    };
    memcpy(shutter, &result, sizeof(result));
    return true;
}
