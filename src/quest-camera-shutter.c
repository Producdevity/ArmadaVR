#include "quest-camera-shutter.h"
#include <string.h>
#include <errno.h>

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

static bool host_time(uint64_t time, uint64_t offset, int64_t *out)
{
    if (!time) return false;
    if (offset & (UINT64_C(1) << 63)) {
        uint64_t magnitude = ~offset + 1;
        if (time <= magnitude) return false;
        time -= magnitude;
    } else {
        if (time > UINT64_MAX - offset) return false;
        time += offset;
    }
    if (time > INT64_MAX / UINT64_C(1000)) return false;
    *out = (int64_t)time * INT64_C(1000);
    return true;
}
bool quest_camera_shutter_decode_fifo(const void *record, size_t length, uint32_t camera_bit,
                                       struct quest_camera_shutter_record *output)
{
    if (!record || !output || length < 3) return false;
    const uint8_t *p = record;
    if (!((p[0] == 2 && p[1] == 12) || (p[0] == 3 && p[1] == 21))) return false;
    size_t header = p[1];
    if (length < header + 3 || p[2] || p[3] != 1 || p[header] != 254 ||
        p[header + 1] || p[header + 2] > 252 || length != header + 3 + p[header + 2]) return false;
    struct quest_camera_shutter_record value = {0};
    uint64_t offset = read_le(p + 4, 8);
    memcpy(&value.nsync_offset_us, &offset, sizeof(offset));
    if (!quest_camera_shutter_decode(p + header + 3, p[header + 2], camera_bit, &value.shutter) ||
        !host_time(value.shutter.timestamp_us, offset, &value.timestamp_ns) ||
        !host_time(value.shutter.sof_us, offset, &value.sof_ns)) return false;
    *output = value;
    return true;
}

int quest_camera_shutter_select(const struct quest_camera_metadata *metadata,
                                const struct quest_camera_match_window *window,
                                const struct quest_camera_shutter_candidate *candidates, size_t count,
                                struct quest_camera_shutter *shutter)
{
    if (!metadata || !window || !shutter || (count && !candidates) || count > 16 ||
        (metadata->bits_per_pixel != 8 && metadata->bits_per_pixel != 10) ||
        !window->generation || !window->camera_bit || (window->camera_bit & (window->camera_bit - 1u)) ||
        window->now_ns <= 0 || window->max_age_ns <= 0 || window->max_age_ns > INT64_C(1000000000) ||
        !window->minimum_timestamp_us || window->maximum_timestamp_us < window->minimum_timestamp_us ||
        window->maximum_timestamp_us <= window->after_timestamp_us) return -EINVAL;
    const struct quest_camera_shutter *match = NULL;
    for (size_t i = 0; i < count; ++i) {
        const struct quest_camera_shutter_candidate *candidate = &candidates[i];
        if (candidate->generation != window->generation) continue;
        const struct quest_camera_shutter *s = &candidate->shutter;
        if (!s->camera_mask || !s->timestamp_us || candidate->received_ns <= 0 ||
            candidate->received_ns > window->now_ns ||
            (s->type != 6 && s->type != 21 && s->type != 22)) return -EPROTO;
        if (!(s->camera_mask & window->camera_bit) ||
            window->now_ns - candidate->received_ns > window->max_age_ns ||
            s->timestamp_us <= window->after_timestamp_us ||
            s->timestamp_us < window->minimum_timestamp_us || s->timestamp_us > window->maximum_timestamp_us) continue;
        if ((window->require_auxiliary_tag || s->type == 22) && s->auxiliary_tag != metadata->auxiliary_tag) continue;
        if (s->type != 21 && s->tag != metadata->tag) continue;
        if (match) return -EEXIST;
        match = s;
    }
    if (!match) return 0;
    memmove(shutter, match, sizeof(*shutter));
    return 1;
}
