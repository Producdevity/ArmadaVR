#ifndef REFRAMED_QUEST_CAMERA_METADATA_H
#define REFRAMED_QUEST_CAMERA_METADATA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum quest_camera_sensor {
    QUEST_CAMERA_OG01A1B = 1,
    QUEST_CAMERA_OV7251 = 2,
};

struct quest_camera_metadata_parameters {
    uint32_t mcu_words[3];
    uint32_t gain_multiplier;
};

struct quest_camera_metadata {
    uint32_t bits_per_pixel;
    uint32_t frame_counter;
    uint16_t exposure_us, gain_code;
    uint16_t matching_words[2], span_us;
    uint8_t tag, auxiliary_tag;
};

/* Parameters must come from the active MCU metadata-matching configuration.
 * OG01A1B uses all three MCU words and requires gain_multiplier=0. OV7251 uses
 * only word 0 (words 1/2 must be zero) and its device descriptor's multiplier;
 * zero means no multiplication. No nominal sensor-mode defaults are inferred.
 * Accept one stable embedded row in stock packed-10 or 8-bit form, preserving
 * stock format-detection precedence. This is not an image-format detector.
 * Reject unknown tags, truncation and arithmetic overflow; output is unchanged.
 * Row tags still need matching to MCU shutter packets. Neither a timestamp nor
 * exposure-center alignment can be obtained from this row alone. */
bool quest_camera_metadata_decode(const void *row, size_t length,
                                   enum quest_camera_sensor sensor,
                                   const struct quest_camera_metadata_parameters *parameters,
                                   struct quest_camera_metadata *metadata);
#endif
