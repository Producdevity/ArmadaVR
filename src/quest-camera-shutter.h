#ifndef REFRAMED_QUEST_CAMERA_SHUTTER_H
#define REFRAMED_QUEST_CAMERA_SHUTTER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "quest-camera-metadata.h"

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
struct quest_camera_shutter_candidate {
    struct quest_camera_shutter shutter;
    uint64_t generation;
    int64_t received_ns;
};
struct quest_camera_match_window {
    uint64_t generation, after_timestamp_us, minimum_timestamp_us, maximum_timestamp_us;
    int64_t now_ns, max_age_ns;
    uint32_t camera_bit;
    bool require_auxiliary_tag;
};

/* Select exactly one of at most 16 decoded candidates. The caller supplies the
 * active generation, an independently verified raw-shutter timestamp interval
 * for this image, and the last consumed timestamp (zero only for a new stream).
 * Require the active device's auxiliary-tag policy. Stock type 21 has no tag
 * test, so the interval is mandatory even when the row is valid. Stale/foreign
 * candidates are ignored; two compatible candidates are an error. Return 1 for
 * one match, 0 for none, or negative errno; output changes only on 1. The owner
 * advances after_timestamp_us on success and invalidates on clock/restart loss.
 * This does not derive the interval from SOF, convert clocks, or infer exposure
 * center. Neither row counter nor queue position identifies an MCU frame. */
int quest_camera_shutter_select(const struct quest_camera_metadata *metadata,
                                const struct quest_camera_match_window *window,
                                const struct quest_camera_shutter_candidate *candidates, size_t count,
                                struct quest_camera_shutter *shutter);
#endif
