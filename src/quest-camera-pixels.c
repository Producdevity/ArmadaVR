#include "quest-camera-pixels.h"
#include <string.h>

static uint8_t convert(unsigned value, enum quest_camera_pixel_curve curve)
{
    static const uint8_t base[16] = {2, 51, 79, 100, 119, 135, 149, 163, 175, 187, 198, 209, 219, 229, 238, 247};
    static const uint8_t slope[16] = {52, 29, 22, 19, 17, 15, 15, 13, 13, 12, 12, 11, 10, 10, 10, 9};
    static const uint8_t offset_base[16] = {0, 44, 74, 97, 116, 133, 148, 161, 174, 186, 197, 208, 218, 228, 238, 247};
    static const uint8_t offset_slope[16] = {0, 31, 24, 20, 18, 16, 14, 14, 13, 12, 12, 11, 11, 10, 10, 9};
    static const uint8_t low[64] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3,
        4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
        20, 21, 22, 23, 24, 25, 26, 27, 27, 28, 29, 30, 31, 31, 32, 33,
        33, 34, 35, 36, 36, 37, 38, 38, 39, 40, 40, 41, 41, 42, 43, 43,
    };
    if (curve == QUEST_CAMERA_CURVE_COPY) return (uint8_t)(value >> 2);
    unsigned index = value >> 6, fraction = value & 63u;
    if (curve == QUEST_CAMERA_CURVE_POINT5)
        return (uint8_t)(base[index] + (fraction * slope[index] >> 6));
    return value < 64 ? low[value] :
        (uint8_t)(offset_base[index] + (fraction * offset_slope[index] >> 6));
}

bool quest_camera_pixels_mono8(const void *source, size_t source_size,
                               const struct quest_camera_pixel_layout *layout,
                               void *destination, size_t destination_size)
{
    if (!source || !layout || !destination) return false;
    struct quest_camera_pixel_layout l = *layout;
    if (!l.width || l.width > 4096 || !l.height || l.height > 4096 || l.first_row > 1 ||
        !l.stride || l.stride > 65536 ||
        (l.format != QUEST_CAMERA_MONO8 && l.format != QUEST_CAMERA_MIPI_RAW10) ||
        (l.curve != QUEST_CAMERA_CURVE_COPY && l.curve != QUEST_CAMERA_CURVE_POINT5 &&
         l.curve != QUEST_CAMERA_CURVE_POINT5_OFFSET) ||
        (l.format == QUEST_CAMERA_MONO8 && l.curve != QUEST_CAMERA_CURVE_COPY) ||
        (l.format == QUEST_CAMERA_MIPI_RAW10 && l.width % 4) ||
        destination_size != (size_t)l.width * l.height) return false;
    size_t row_bytes = l.format == QUEST_CAMERA_MONO8 ? l.width : l.width / 4u * 5u;
    size_t required = (l.first_row + l.height - 1u) * l.stride + row_bytes;
    if (l.stride < row_bytes || source_size < required || source_size > 65536u * 4097u)
        return false;
    uintptr_t src = (uintptr_t)source, dst = (uintptr_t)destination;
    if (src > UINTPTR_MAX - source_size || dst > UINTPTR_MAX - destination_size ||
        (src < dst + destination_size && dst < src + source_size)) return false;
    const uint8_t *in = source;
    uint8_t *out = destination;
    for (unsigned y = 0; y < l.height; ++y) {
        const uint8_t *row = in + (y + l.first_row) * l.stride;
        uint8_t *result = out + (size_t)y * l.width;
        if (l.format == QUEST_CAMERA_MONO8) {
            memcpy(result, row, l.width);
            continue;
        }
        for (unsigned x = 0; x < l.width; x += 4) {
            const uint8_t *group = row + x / 4u * 5u;
            for (unsigned i = 0; i < 4; ++i)
                result[x + i] = convert((unsigned)group[i] << 2 | (group[4] >> (2u * i) & 3u), l.curve);
        }
    }
    return true;
}
