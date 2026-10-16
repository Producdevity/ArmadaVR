#include "quest-camera-metadata.h"
#include <string.h>

static unsigned og_byte(const uint8_t *row, unsigned group, unsigned bits)
{
    size_t offset = group * (bits == 10 ? 5u : 4u) + 3u;
    return bits == 10 ? ((unsigned)row[offset] << 2 | row[offset + 1] >> 6) & 255u
                      : row[offset];
}

static unsigned ov_byte(const uint8_t *row, unsigned index, unsigned bits)
{
    return row[bits == 10 ? index / 4u * 5u + index % 4u : index];
}

static bool bounded_time(double value, uint16_t *out)
{
    if (value < 0 || value >= 65536.0)
        return false;
    *out = (uint16_t)value;
    return true;
}

bool quest_camera_metadata_decode(const void *data, size_t length,
                                   enum quest_camera_sensor sensor,
                                   const struct quest_camera_metadata_parameters *p,
                                   struct quest_camera_metadata *metadata)
{
    if (!data || !p || !metadata || !p->mcu_words[0] || length > 16384)
        return false;
    const uint8_t *row = data;
    struct quest_camera_metadata out = {0};
    if (sensor == QUEST_CAMERA_OG01A1B) {
        if (length < 10 || p->gain_multiplier || p->mcu_words[1] > UINT16_MAX ||
            p->mcu_words[2] > UINT16_MAX)
            return false;
        unsigned bits = row[5] == 0x35 ? 8u :
            (((row[5] << 2 | (row[9] & 3u)) & 255u) == 0x55 ? 10u : 0u);
        if (!bits || length < (bits == 10 ? 30u : 24u))
            return false;
        unsigned exposure = og_byte(row, 0, bits) << 8 | og_byte(row, 1, bits);
        double period = (double)p->mcu_words[0] / 120.0;
        if (!bounded_time(period * exposure + 43.0, &out.exposure_us) ||
            !bounded_time(period * (p->mcu_words[2] + 1u), &out.span_us))
            return false;
        out.bits_per_pixel = bits;
        out.gain_code = (uint16_t)((og_byte(row, 2, bits) & 15u) << 4 |
                                   (og_byte(row, 3, bits) >> 1 & 15u));
        out.frame_counter = og_byte(row, 4, bits);
        out.tag = (uint8_t)og_byte(row, 5, bits);
        out.matching_words[0] = (uint16_t)p->mcu_words[1];
        out.matching_words[1] = (uint16_t)p->mcu_words[2];
    } else if (sensor == QUEST_CAMERA_OV7251) {
        if (length < 21 || p->mcu_words[1] || p->mcu_words[2] ||
            p->gain_multiplier > UINT16_MAX)
            return false;
        unsigned bits = (row[16] == 0x40 || row[16] == 0x80) ? 8u :
            (row[20] == 0x40 || row[20] == 0x80) ? 10u : 0u;
        if (!bits || length < (bits == 10 ? 112u : 90u))
            return false;
        unsigned exposure = ov_byte(row, 6, bits) << 8 | ov_byte(row, 7, bits);
        unsigned gain = ov_byte(row, 3, bits) * (p->gain_multiplier ? p->gain_multiplier : 1u);
        out.matching_words[0] = (uint16_t)(ov_byte(row, 15, bits) << 8 | ov_byte(row, 16, bits));
        out.matching_words[1] = (uint16_t)(ov_byte(row, 17, bits) << 8 | ov_byte(row, 18, bits));
        double period = (double)p->mcu_words[0] / 48.0;
        if (gain > UINT16_MAX || !bounded_time(period * exposure, &out.exposure_us) ||
            !bounded_time(period * (out.matching_words[1] + 1u), &out.span_us))
            return false;
        out.bits_per_pixel = bits;
        out.gain_code = (uint16_t)gain;
        out.frame_counter = ov_byte(row, 89, bits);
        out.tag = (uint8_t)ov_byte(row, 82, bits);
        out.auxiliary_tag = (uint8_t)ov_byte(row, 84, bits);
    } else {
        return false;
    }
    memcpy(metadata, &out, sizeof(out));
    return true;
}
