#include "pico-camera.h"
#include <limits.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static bool image_valid(const struct pico_camera_frame *f)
{
    return f->pixels && f->width && f->width <= 4096 && f->height && f->height <= 4096 &&
           f->size == (size_t)f->width * f->height && f->classification <= 3 &&
           f->qtimer_ns > 0 && f->center_ns > 0 && f->center_ns <= f->qtimer_ns && f->line_ns >= 0;
}

bool pico_camera_decode(const void *snapshot, size_t length, struct pico_camera_frame *frame)
{
    if (!snapshot || !frame || length < PICO_CAMERA_HEADER_SIZE ||
        length > PICO_CAMERA_HEADER_SIZE + PICO_CAMERA_MAX_PIXELS) return false;
    const uint8_t *p = snapshot;
    if (le32(p + 9) != PICO_CAMERA_MONO8 || le32(p + 0x45) || le32(p + 0x49) ||
        le64(p + 0x19) > INT64_MAX || le64(p + 0x21) > INT64_MAX || le64(p + 0x29) > INT64_MAX)
        return false;
    struct pico_camera_frame value = {
        .camera_id = p[0], .width = le32(p + 1), .height = le32(p + 5),
        .exposure_lines = le32(p + 0xd), .timestamp = le64(p + 0x11),
        .qtimer_ns = (int64_t)le64(p + 0x19), .center_ns = (int64_t)le64(p + 0x21),
        .line_ns = (int64_t)le64(p + 0x29), .frame_number = le64(p + 0x31),
        .size = le32(p + 0x39), .gain = le32(p + 0x3d), .classification = le32(p + 0x41),
        .extra = le32(p + 0x55), .pixels = p + PICO_CAMERA_HEADER_SIZE,
    };
    if (value.size != length - PICO_CAMERA_HEADER_SIZE || !image_valid(&value)) return false;
    *frame = value;
    return true;
}

bool pico_camera_center(int64_t qtimer_ns, uint32_t exposure_lines,
                         int64_t line_ns, int64_t readout_ns, int64_t *center_ns)
{
    if (!center_ns || qtimer_ns <= 52000 || !exposure_lines || exposure_lines > 737 ||
        line_ns <= 0 || readout_ns < 0 || line_ns > INT64_MAX / exposure_lines) return false;
    int64_t available = qtimer_ns - 52000;
    if (readout_ns >= available) return false;
    available -= readout_ns;
    int64_t half_exposure = line_ns * exposure_lines / 2;
    if (half_exposure >= available) return false;
    *center_ns = available - half_exposure;
    return true;
}

bool pico_camera_head_set(const struct pico_camera_frame *frames, size_t count,
                           const uint8_t *camera_ids, int64_t max_skew_ns,
                           struct pico_camera_frame_set *set)
{
    if (!frames || !camera_ids || !set || (count != 2 && count != 4) ||
        max_skew_ns < 0 || max_skew_ns > PICO_CAMERA_MAX_SKEW_NS) return false;
    struct pico_camera_frame_set value = {.count = count, .first_ns = INT64_MAX};
    for (size_t i = 0; i < count; ++i) {
        if (!image_valid(&frames[i]) || frames[i].camera_id != camera_ids[i] ||
            frames[i].classification != 1 || !frames[i].exposure_lines ||
            frames[i].exposure_lines > 737 || frames[i].line_ns <= 0) return false;
        for (size_t j = 0; j < i; ++j) if (camera_ids[i] == camera_ids[j]) return false;
        value.frames[i] = frames[i];
        if (frames[i].center_ns < value.first_ns) value.first_ns = frames[i].center_ns;
        if (frames[i].center_ns > value.last_ns) value.last_ns = frames[i].center_ns;
    }
    if (value.last_ns - value.first_ns > max_skew_ns) return false;
    *set = value;
    return true;
}
