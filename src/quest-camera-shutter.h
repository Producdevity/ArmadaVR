#ifndef REFRAMED_QUEST_CAMERA_SHUTTER_H
#define REFRAMED_QUEST_CAMERA_SHUTTER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct quest_camera_shutter {
    uint64_t timestamp_us, sof_us;
    uint32_t camera_mask, frame_counter;
    uint16_t exposure_us, gain_code;
    uint8_t type, tag, auxiliary_tag;
};

/* Decode one packet-254 payload for one explicitly selected camera bit. Supports
 * stock tracking types 6, 21 and 22. Additional cameras can carry exposure/gain
 * overrides in ascending mask-bit order; absent overrides reuse the base values.
 * Peripheral metadata is not interpreted. Require the exact declared size and
 * reject orphan overrides. Failure preserves the output; no device is opened.
 * Times remain raw MCU microseconds, without NSYNC or exposure correction.
 * Tags still need unambiguous matching to a current frame in the same generation.
 * In particular, type 21 does not require a matching row tag in the stock code. */
bool quest_camera_shutter_decode(const void *payload, size_t length, uint32_t camera_bit,
                                  struct quest_camera_shutter *shutter);
#endif
